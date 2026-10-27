#!/bin/sh
# Flash one SAM E51 Curiosity Nano when several are plugged in.
# openocd 0.12's cmsis-dap HID backend aborts on serial selection, so the other nEDBG probes
# are de-authorised on USB for the duration and restored afterwards (needs sudo).
#   tools/flash_one.sh 000469 build/node0/zephyr/zephyr.hex
set -e
want=$1 hex=$2
cd "$(dirname "$0")/.."
hidden=""
for d in /sys/bus/usb/devices/*; do
	[ -f "$d/serial" ] && [ "$(cat "$d/idVendor"):$(cat "$d/idProduct")" = "03eb:2175" ] || continue
	case "$(cat "$d/serial")" in *"$want") ;; *) hidden="$hidden $d" ;; esac
done
restore() { for d in $hidden; do sudo sh -c "echo 1 > $d/authorized"; done; }
trap restore EXIT
for d in $hidden; do sudo sh -c "echo 0 > $d/authorized"; done
sleep 1
openocd -f boards/microchip/same51_cnano/support/openocd.cfg \
	-c init -c 'reset init' -c "program $hex verify reset" -c shutdown
