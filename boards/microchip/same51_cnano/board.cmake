# SPDX-License-Identifier: Apache-2.0
# Flash through the on-board nEDBG (CMSIS-DAP, USB 03eb:2175) with OpenOCD.
board_runner_args(openocd "--config=${BOARD_DIR}/support/openocd.cfg")
include(${ZEPHYR_BASE}/boards/common/openocd.board.cmake)
