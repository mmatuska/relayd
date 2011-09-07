FreeBSD port of relayd and relayctl
===================================

This is the [FreeBSD][1] port of the [OpenBSD][2] relayd and relayctl.

relayd is a daemon to relay and dynamically redirect incoming connections
to a target host.  Its main purposes are to run as a load-balancer,
application layer gateway, or transparent proxy.  The daemon is able to
monitor groups of hosts for availability, which is determined by checking
for a specific service common to a host group.  When availability is con-
firmed, Layer 3 and/or layer 7 forwarding services are set up by relayd.

The relayctl program controls the relayd(8) daemon.

[1]: http://www.freebsd.org/
[2]: http://www.openbsd.org/
