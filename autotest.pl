#!/usr/bin/perl
use strict;

# Source
while (1) {
	`iperf -u -c 192.168.55.70 -b 40M -n 100M`;
	#`make rebuild`;
	sleep(130);
}

while (0) {
	`iperf -u -c 192.168.55.70 -b 40M -n 200M`;
	sleep(130);
}

# Relay and Destination 
#  Do not test them in the smae time
while (0) {
	`make rebuild`;
	sleep(300);
}
