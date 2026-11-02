#!/usr/bin/env bash
# Make this project's keys OUTSIDE the repository (it is public): ~/.config/t1s-keys/ (mode 700, files 600).
#   mcuboot-rsa-2048.pem  MCUboot image signing key (private). Its public half is compiled into MCUboot.
#   bus.key               the bus shell's HMAC-SHA256 key, 64 hex digits (firmware build + console + smp_udp.py).
# Existing files are never overwritten: a new signing key needs every node's MCUboot reflashed over SWD, a new bus
# key every node's firmware rebuilt and updated (while the old key still opens their SMP window). Back both up.
set -euo pipefail
KD=${T1S_KEY_DIR:-$HOME/.config/t1s-keys}
IMGTOOL=${IMGTOOL:-$HOME/zephyrproject/bootloader/mcuboot/scripts/imgtool.py}
umask 077
mkdir -p "$KD"
chmod 700 "$KD"
if [ ! -f "$KD/mcuboot-rsa-2048.pem" ]; then
    python3 "$IMGTOOL" keygen -k "$KD/mcuboot-rsa-2048.pem" -t rsa-2048
    echo "made $KD/mcuboot-rsa-2048.pem"
fi
if [ ! -f "$KD/bus.key" ]; then
    python3 -c 'import secrets; print(secrets.token_hex(32))' > "$KD/bus.key"
    echo "made $KD/bus.key"
fi
ls -l "$KD"
echo "bus key id: $(tr -d '\n' < "$KD/bus.key" | sha256sum | cut -c1-8)  (heartbeat \"kid\")"
