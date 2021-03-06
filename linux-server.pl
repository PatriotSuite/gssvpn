#!/usr/bin/perl

#
# This will generate some network information to send to GSSVPN clients.
# It expects the parameters to be:
#
# linux-server.pl <principal name> <ip address> <port number>
#
# At the very least, it should return a mac address suitable for parsing
# by ether_aton. It can also optionally print options to pass back to the
# client in the format of "optionname\noptionvalue\n" The client will
# strip off the new lines and pack them into argv.
#
# This client will do PAM authorization to determine whether the user
# is allowed to use GSSVPN. If the script returns non-zero, the client
# will be shut down.
#

my $princname = shift @ARGV;
my $remoteip = shift @ARGV;
my $remoteport = shift @ARGV;

my $ipmatch = "((([0-9]|[1-9][0-9]|1[0-9]{2}|2[0-4][0-9]|25[0-5])\.){3}([0-9]|[1-9][0-9]|1[0-9]{2}|2[0-4][0-9]|25[0-5]))+";
my @routes = ( );
my @netroutes = ( );
my $subnet;
my $gateway;
my $clientip;
my $clientmac;

open my $conf, '<', '/etc/gssvpn.conf' or die "Cannot open gssvpn config!";
while(<$conf>) {
	if($_ =~ /route\s+$ipmatch(\/(\d|[1-2]\d|3[0-2]){1,2})?\s*$/) {
		my $ip = $1;
		my $cidr = $5;
		if($cidr) {
			push @netroutes, "$ip$cidr";
		} else {
			push @routes, $ip;
		}
	}
	elsif($_ =~ /gateway\s+$ipmatch\s*$/) {
		$gateway = $1;
	}
	elsif($_ =~ /subnet\s+$ipmatch\s*$/) {
		$subnet = $1;
	}
	elsif($_ =~ /user\s+([^\s^\{]+)\s*{/) {
		$inuser = $1;
		if($inuser !~ /$princname/i) {
			undef $inuser;
		}
	}
	elsif($_ =~ /\}/) {
		undef $inuser;
		$clientip && $subnet && last;
	}
	elsif($inuser && $_ =~ /ip\s+($ipmatch)\s*$/) {
		$clientip = $1;
	}
	elsif($inuser && $_ =~ /dhcp/i) {
		$clientip = 'dhcp';
	}
}
close $conf;

(!($clientip && $subnet)) && die "Must specify client ip and subnet.";

if($clientip =~ /dhcp/) {
	print "dhcp\nsubnet\n$subnet\ngateway\n$gateway\n";
} else {
	print "ip\n$clientip\nsubnet\n$subnet\ngateway\n$gateway\n";
}

!$gateway && exit 0;

foreach $route (@routes) {
	print "route\n$route\n";
}
foreach $route (@netroutes) {
	print "netroute\n$route\n";
}

exit 0;
