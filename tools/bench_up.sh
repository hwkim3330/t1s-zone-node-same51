#!/bin/bash
# Bring up the bench PC side of the T1S zone bus, idempotently (run it again any time):
#   zenohd    tcp/192.168.100.51:7448   (the nodes' router; zenohd.service on 7447 is tied to the EVB's .70)
#   zonelink  Autoware (HMI gateway :8780 on the Autoware PC) -> vehicle/** VSS keys, restarted if it dies
#   console   http://localhost:8830
#   zone->HMI the zone nodes to the HMI gateway (POST /api/zone), thumbstick up/down -> max speed +-5 km/h
#   tools/bench_up.sh [stop|status]
# Logs: ~/.cache/t1s-bench/*.log
set -u
R=$(cd "$(dirname "$0")/.." && pwd)
L=$HOME/.cache/t1s-bench
mkdir -p "$L"
ZENOHD=/opt/zenoh/1.10.1/zenohd
ROUTER=tcp/192.168.100.51:7448
GATEWAY=${GATEWAY:-http://10.42.0.90:8780}
# the vehicle-API sidecar (zonelink.py, from the simulator project); SIDECAR=<path> to point at it
SIDECAR=${SIDECAR:-$(ls "$HOME"/*/sdv/zonelink.py 2>/dev/null | head -1)}

listening() { ss -ltn | grep -q ":$1 "; }
pidfile_alive() { [ -f "$L/$1.pid" ] && kill -0 "$(cat "$L/$1.pid")" 2>/dev/null; }

stop() {
	for n in zone_to_hmi zonelink console zenohd; do
		if pidfile_alive $n; then kill "$(cat "$L/$n.pid")" && echo "stopped $n"; fi
		rm -f "$L/$n.pid"
	done
}

status() {
	listening 7448 && echo "zenohd   up   $ROUTER" || echo "zenohd   DOWN"
	pidfile_alive zonelink && echo "zonelink up   $GATEWAY -> $ROUTER" || echo "zonelink DOWN"
	listening 8830 && echo "console  up   http://localhost:8830" || echo "console  DOWN"
	pidfile_alive zone_to_hmi && echo "zone->HMI up  $GATEWAY/api/zone (+ stick -> speed limit)" || echo "zone->HMI DOWN"
}

case "${1:-up}" in
stop) stop; exit 0 ;;
status) status; exit 0 ;;
esac

ip -br addr | grep -q "192.168.100.51/" || echo "warning: no interface has 192.168.100.51 (the USB NIC to the ESP gateway)"

if ! listening 7448; then
	nohup "$ZENOHD" --listen "$ROUTER" --no-multicast-scouting > "$L/zenohd.log" 2>&1 &
	echo $! > "$L/zenohd.pid"
	sleep 2
fi
if [ -z "$SIDECAR" ]; then
	echo "zonelink: no sidecar found (set SIDECAR=<path to zonelink.py>)"
elif ! pidfile_alive zonelink; then
	nohup bash -c "while true; do python3 -u $SIDECAR --api $GATEWAY --router $ROUTER; echo \"restart \$(date +%T)\"; sleep 2; done" \
		> "$L/zonelink.log" 2>&1 &
	echo $! > "$L/zonelink.pid"
fi
if ! listening 8830; then
	# note: opening the ESP's USB port resets the ESP (the gateway is back in ~10 s, nodes re-lock PTP in ~40 s)
	nohup python3 -u "$R/tools/console/server.py" > "$L/console.log" 2>&1 &
	echo $! > "$L/console.pid"
	sleep 3
fi
if ! pidfile_alive zone_to_hmi; then
	nohup bash -c "while true; do python3 -u $R/tools/zone_to_hmi.py --gateway $GATEWAY; echo \"restart \$(date +%T)\"; sleep 3; done" \
		> "$L/zone_to_hmi.log" 2>&1 &
	echo $! > "$L/zone_to_hmi.pid"
fi
status
