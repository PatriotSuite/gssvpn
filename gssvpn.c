/*
 * Copyright 2011 Jonathan Reams
 * 
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 * 
 * http://www.apache.org/licenses/LICENSE-2.0
 * 
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "config.h"
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <netdb.h>
#include <fcntl.h>
#include <stdio.h>
#include <gssapi/gssapi.h>
#include <unistd.h>
#include <net/if.h>
#if defined(HAVE_IF_TUN)
#include <linux/if_tun.h>
#endif
#ifdef HAVE_LINUX_SOCKIOS_H
#include <linux/sockios.h>
#else
#include <net/if_dl.h>
#endif
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include "libev/ev.h"
#include "gssvpn.h"

gss_ctx_id_t context = GSS_C_NO_CONTEXT;
OM_uint32 gssstate = GSS_S_CONTINUE_NEEDED;
int tapfd, netfd, verbose = 0, init = 0;
struct sockaddr_in server;
char * tapdev, *service, *hostname, *netinit_util = NULL;
ev_child netinit_child;
ev_timer init_retry;
int daemonize = 0;
ev_tstamp last_init_activity;

gss_ctx_id_t get_context(struct sockaddr_in* peer) {
	if(gssstate == GSS_S_COMPLETE)
		return context;
	return NULL;
}

void init_retry_cb(struct ev_loop * loop, ev_timer * w, int revents) {
	ev_tstamp now = ev_now (EV_A);
	ev_tstamp timeout = last_init_activity + 10;
	if(timeout < now) {
		if(gssstate != GSS_S_COMPLETE) {
			logit(1, "Did not receive GSS packet from server. Retrying.");
			do_gssinit(NULL);
		}
		else if(init != 1) {
			logit(1, "Did not receive netinit packet from server. Retrying.");
			send_packet(netfd, NULL, &server, PAC_NETINIT);
		}
		else
			ev_timer_stop(loop, w);
	} else {
		w->repeat = timeout - now;
		ev_timer_again(loop, w);	
	}
}

void netinit_cb(struct ev_loop * loop, ev_child * c, int revents) {
	if(c->rstatus == 0) {
		init = 1;
		logit(0, "Netinit okay. Starting normal operation.");
		return;
	}

	logit(1, "Received error code from netinit util %d", c->rstatus);
	send_packet(netfd, NULL, &server, PAC_SHUTDOWN);
	ev_child_stop(loop, &netinit_child);
	ev_break(loop, EVBREAK_ALL);
}

int do_netinit(struct ev_loop * loop, gss_buffer_desc * in) {
	struct netinit ni;
	struct ifreq ifr;
	pid_t pid;

	if(in->length > sizeof(ni)) {
		logit(1, "Received a netinit packet %d bytes too long",
			in->length - sizeof(ni));
		return -1;
	}

	memcpy(&ni, in->value, in->length);
	memset(&ifr, 0, sizeof(ifr));
	strncpy(ifr.ifr_name, tapdev, IFNAMSIZ);
#ifdef SIOCSIFHWADDR
	memcpy(ifr.ifr_hwaddr.sa_data, ni.mac, sizeof(ni.mac));
	if(ioctl(tapfd, SIOCSIFHWADDR, &ifr) < 0) {
#else
	memcpy(ifr.ifr_addr.sa_data, ni.mac, sizeof(ni.mac));
	ifr.ifr_addr.sa_family = AF_LINK;
	ifr.ifr_addr.sa_len = sizeof(ni.mac);
	if(ioctl(tapfd, SIOCSIFLLADDR, &ifr) < 0) {
#endif
		logit(1, "Error setting MAC address for %s: %s", tapdev,
			strerror(errno));
		return -1;
	}

	if(!ni.len) {
		logit(-1, "Received no netinit data, but that's okay! Starting normal operation.");
		init = 1;
		return 0;
	}

	if(!netinit_util) {
		logit(0, "Received %d bytes of netinit data, but no netinit util. Starting normal operation.",
			ni.len);
		init = 1;
		return 0;
	}

	init = 0;
	last_init_activity = ev_now(loop);
	pid = fork();
	if(pid == 0) {
		uint8_t * lock = netinit_util + (strlen(netinit_util) - 1);
		char ** args = malloc(sizeof(char*)* 256);
		int argcount = 0;
		OM_uint32 min;

		close(tapfd);
		close(netfd);

		while(*lock != '/' && lock != netinit_util)
			lock--;
		if(*lock == '/')
			lock++;

		args[argcount++] = netinit_util;
		args[argcount++] = tapdev;
		lock = ni.payload;
		while(lock - ni.payload < ni.len && argcount < 255) {
			uint8_t * save = lock;
			while(*lock != '\n' && lock - ni.payload < ni.len) lock++;
			if(*lock == '\n') {
				*lock = 0;
				args[argcount++] = save;
				lock++;
			}
		}
		args[argcount] = NULL;
		if(execv(netinit_util, args) < 0)
			exit(-1);
	} else {
		ev_child_init(&netinit_child, netinit_cb, pid, 0);
		ev_child_start(loop, &netinit_child);
	}

	return 0;
}

int do_gssinit(gss_buffer_desc * in) {
	gss_name_t target_name;
	char prodid[512];
	gss_buffer_desc tokenout = { 512, &prodid };
	OM_uint32 min;

	tokenout.length = snprintf(prodid, 512, "%s@%s", service, hostname);
	gssstate = gss_import_name(&min, &tokenout, 
					(gss_OID)GSS_C_NT_HOSTBASED_SERVICE,
					&target_name);
	tokenout.value = NULL;
	tokenout.length = 0;

	if(context == GSS_C_NO_CONTEXT)
		gss_delete_sec_context(&min, &context, NULL);
	gssstate = gss_init_sec_context(&min, GSS_C_NO_CREDENTIAL,
					&context, target_name, NULL, 0, 0, NULL,
					in, NULL, &tokenout, NULL, NULL);

	if(gssstate != GSS_S_COMPLETE && gssstate != GSS_S_CONTINUE_NEEDED) {
		if(context != GSS_C_NO_CONTEXT)
			gss_delete_sec_context(&min, &context, GSS_C_NO_BUFFER);
		display_gss_err(gssstate, min);
		return -1;
	}

	gss_release_name(&min, &target_name);

	if(tokenout.length) {
		int rc;
		rc = send_packet(netfd, &tokenout, &server, PAC_GSSINIT);
		gss_release_buffer(&min, &tokenout);
		if(rc < 0)
			return -1;
	}
	return 0;
}

void netfd_read_cb(struct ev_loop * loop, ev_io * ios, int revents) {
	int rc;
	uint8_t pac;
	struct sockaddr_in peer;
	gss_buffer_desc packet = GSS_C_EMPTY_BUFFER;
	OM_uint32 min;

	rc = recv_packet(netfd, &packet, &pac, &peer);
	if(rc != 0)
		return;
	
	if(pac == PAC_DATA) {
		if(verbose)
			logit(-1, "Writing %d bytes to TAP", packet.length);

		size_t s = write(tapfd, packet.value, packet.length);
		if(s < 0)
			logit(1, "Error writing packet to tap: %s", strerror(errno));
		else if(s < packet.length)
			logit(1, "Sent less than expected to tap: %d < %d",
				s, packet.length);
		gss_release_buffer(&min, &packet);
		return;
	}
	else if(pac == PAC_NETINIT)
		do_netinit(loop, &packet);
	else if(pac == PAC_GSSINIT)
		do_gssinit(&packet);
	else if(pac == PAC_SHUTDOWN)
		ev_break(loop, EVBREAK_ALL);
	if(packet.length)
		gss_release_buffer(&min, &packet);
}

void tapfd_read_cb(struct ev_loop * loop, ev_io * ios, int revents) {
	uint8_t inbuff[1550];
	gss_buffer_desc plaintext = { 1550, inbuff };

	plaintext.length = read(tapfd, inbuff, 1550);
	if(plaintext.length < 0) {
		logit(1, "Error receiving packet from TAP: %s",
			strerror(errno));
		return;
	}
	else if(verbose)
		logit(-1, "Received packet from TAP of %d bytes",
			plaintext.length);

	send_packet(netfd, &plaintext, &server, PAC_DATA);
	return;
}

int main(int argc, char ** argv) {
	ev_io tapio, netio;
	struct ev_loop * loop;
	char ch;
	short port = 0;
	struct hostent * hostinfo;
	OM_uint32 min;

	memset(&server, 0, sizeof(struct sockaddr_in));
	
	while((ch = getopt(argc, argv, "vh:p:s:i:a:")) != -1) {
		switch(ch) {
			case 'v':
				verbose = 1;
				break;
			case 'h':
				hostname = strdup(optarg);
				break;
			case 'p':
				port = atoi(optarg);
				break;
			case 's':
				service = strdup(optarg);
				break;
			case 'i':
				tapdev = strdup(optarg);
				break;
			case 'a': {
				if(access(optarg, R_OK|X_OK) < 0) {
					logit(1, "Unable to access %s for read/execute: %s",
						optarg, strerror(errno));
					return -1;
				}
				netinit_util = strdup(optarg);
				break;
			}
		}
	}

	if(hostname == NULL) {
		logit(1, "Must enter hostname to connect to");
		return -1;
	}
	hostinfo = gethostbyname(hostname);
	if(hostinfo == NULL) {
		logit(1, "Unable to resolve %s: %s",
						hostname, strerror(errno));
		return -1;
	}

	if(service == NULL)
		service = strdup("gssvpn");
	if(port == 0)
		port = 2106;
	if(tapdev == NULL)
		tapdev = strdup("tap0");

	memset(&server, 0, sizeof(struct sockaddr_in));
	server.sin_family = AF_INET;
	memcpy(&server.sin_addr, hostinfo->h_addr, sizeof(server.sin_addr));
	server.sin_port = htons(port);

	netfd = open_net(0);
	if(netfd < 0)
		return -1;

	tapfd = open_tap(tapdev);
	if(tapfd < 0)
		return -1;

	loop = ev_default_loop(0);
	ev_io_init(&netio, netfd_read_cb, netfd, EV_READ);
	ev_io_start(loop, &netio);
	ev_io_init(&tapio, tapfd_read_cb, tapfd, EV_READ);
	ev_io_start(loop, &tapio);

	if(do_gssinit(NULL) < 0) {
		close(tapfd);
		close(netfd);
		return -1;
	}

	ev_timer_init(&init_retry, init_retry_cb, 10, 0);
	last_init_activity = ev_now(loop);
	ev_timer_start(loop, &init_retry);

	ev_run(loop, 0);

	close(tapfd);
	close(netfd);
	if(context == GSS_C_NO_CONTEXT)
		gss_delete_sec_context(&min, &context, NULL);

	return 0;
}

