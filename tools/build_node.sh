#!/usr/bin/env bash
# Build the zone node (MCUboot + app, sysbuild) with the keys from outside the repository.
#
#   tools/build_node.sh <build dir> <version> [extra args for the app image, e.g. -Dnode_CONFIG_APP_ROLE=\"x\"]
#
#   T1S_SIGNING_KEY  MCUboot signing key (PEM). Default ~/.config/t1s-keys/mcuboot-rsa-2048.pem if it exists.
#                    T1S_SIGNING_KEY=dev = MCUboot's public development key: what nodes 3 and 4 still have in their
#                    bootloader (their OTA images must be signed with it until their MCUboot is reflashed over SWD).
#   T1S_BUS_KEY_FILE the bus shell's HMAC key (64 hex digits). Default ~/.config/t1s-keys/bus.key.
#   ZEPHYR_WS        the west workspace (default: the one this repo sits in, else ~/zephyrproject).
# Keys: tools/keys.sh makes both (never in the repository).
set -euo pipefail
R=$(cd "$(dirname "$0")/.." && pwd)
[ $# -ge 2 ] || { sed -n '2,12p' "$0"; exit 2; }
B=$(realpath -m "$1"); V=$2; shift 2
KD=$HOME/.config/t1s-keys
if [ "${T1S_SIGNING_KEY:-}" = dev ]; then
    unset T1S_SIGNING_KEY
    echo "build_node: signing with MCUboot's DEVELOPMENT key (nodes whose MCUboot still has it)"
elif [ -z "${T1S_SIGNING_KEY:-}" ] && [ -f "$KD/mcuboot-rsa-2048.pem" ]; then
    export T1S_SIGNING_KEY=$KD/mcuboot-rsa-2048.pem
fi
[ -n "${T1S_SIGNING_KEY:-}" ] && echo "build_node: signing key $T1S_SIGNING_KEY"
export T1S_BUS_KEY_FILE=${T1S_BUS_KEY_FILE:-$KD/bus.key}
if [ -z "${ZEPHYR_WS:-}" ]; then
    if [ -d "$R/../zephyr" ] && [ -d "$R/../.west" ]; then ZEPHYR_WS=$(cd "$R/.." && pwd); else ZEPHYR_WS=$HOME/zephyrproject; fi
fi
export ZEPHYR_SDK_INSTALL_DIR=${ZEPHYR_SDK_INSTALL_DIR:-$HOME/zephyr-sdk-0.16.5}
cd "$ZEPHYR_WS"
# shellcheck disable=SC1091
source zephyr/zephyr-env.sh
nice west build -p --sysbuild -b same51_cnano -d "$B" "$R/apps/node" -- -DBOARD_ROOT="$R" \
    -Dnode_CONFIG_MCUBOOT_IMGTOOL_SIGN_VERSION=\""$V"\" "$@"
ls -l "$B/node/zephyr/zephyr.signed.bin" "$B/mcuboot/zephyr/zephyr.hex"
