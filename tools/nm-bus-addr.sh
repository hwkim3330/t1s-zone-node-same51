#!/bin/sh
# NetworkManager dispatcher hook: keep the T1S bus address on the port to the D10s.
# The port's NM profile is "shared" (10.42.0.1, the Autoware PC's link) and shared mode drops any
# second address, so every time the Autoware PC powers up (link down/up) the nodes' Zenoh router
# address 192.168.100.51 vanished and every node went silent on the console.
# Install: sudo install -m 755 tools/nm-bus-addr.sh /etc/NetworkManager/dispatcher.d/90-t1s-bus-addr
IFACE=${BUS_IFACE:-enp4s0}
ADDR=${BUS_ADDR:-192.168.100.51/24}
[ "$1" = "$IFACE" ] || exit 0
case "$2" in
up|dhcp4-change|reapply) ip addr replace "$ADDR" dev "$IFACE" ;;
esac
