# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)

# Out-of-tree board (boards/microchip/same51_cnano)
list(APPEND BOARD_ROOT ${CMAKE_CURRENT_LIST_DIR}/../..)

# Which mikroBUS slot holds the Two-Wire ETH click (found by apps/probe).
set(T1S_SLOT 3 CACHE STRING "mikroBUS slot (1-3) of the MIKROE-5543 click")
list(APPEND EXTRA_DTC_OVERLAY_FILE
     ${CMAKE_CURRENT_LIST_DIR}/../../dts/two-wire-eth-click-slot${T1S_SLOT}.overlay)

# zenoh-pico 1.10.1 as a Zephyr module (same tree the ESP T1S HAT firmware vendors).
set(ZENOH_PICO_DIR $ENV{HOME}/esp32-t1s-bridge/vendor/zenoh-pico CACHE PATH "zenoh-pico source")
list(APPEND ZEPHYR_EXTRA_MODULES ${ZENOH_PICO_DIR})

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(t1s_zone_node)

target_sources(app PRIVATE src/main.c src/t1s_diag.c)
if(CONFIG_APP_ZENOH)
  # the module does not run zenoh-pico's configure step; this is its config.h
  zephyr_include_directories(${CMAKE_CURRENT_LIST_DIR}/zenoh-config)
  zephyr_compile_definitions(ZENOH_LOG_WARN)
  target_sources(app PRIVATE src/zone_zenoh.c)
endif()
# t1s_diag.c reads PLCA status through the in-tree driver's OA TC6 helpers
target_include_directories(app PRIVATE
  ${ZEPHYR_BASE}/drivers/ethernet
  ${ZEPHYR_BASE}/subsys/net/l2)
