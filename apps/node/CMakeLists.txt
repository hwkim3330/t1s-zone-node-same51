# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)

# Out-of-tree board (boards/microchip/same51_cnano)
list(APPEND BOARD_ROOT ${CMAKE_CURRENT_LIST_DIR}/../..)

# In a sysbuild (MCUboot) an option for this image is given as -D<image>_<VAR> (here node_T1S_SLOT,
# node_LAMP_SLOT) and reaches the image only through the sysbuild cache file; zephyr_get() would read it,
# but it exists only after find_package(Zephyr), and the overlays must be chosen before that.
function(sysbuild_option var)
  if(DEFINED SYSBUILD_CACHE AND EXISTS "${SYSBUILD_CACHE}")
    file(STRINGS "${SYSBUILD_CACHE}" lines)
    set(name "")
    foreach(l IN LISTS lines)
      if(l MATCHES "^SYSBUILD_NAME:[^=]*=(.*)$")
        set(name "${CMAKE_MATCH_1}")
      endif()
    endforeach()
    foreach(l IN LISTS lines)
      if(name AND l MATCHES "^${name}_${var}:[^=]*=(.*)$")
        set(${var} "${CMAKE_MATCH_1}" CACHE STRING "" FORCE)
      endif()
    endforeach()
  endif()
endfunction()
sysbuild_option(T1S_SLOT)
sysbuild_option(LAMP_SLOT)
sysbuild_option(PANEL_SLOT)
sysbuild_option(SENSOR_SLOT)
sysbuild_option(BUS_KEY_FILE)

# The bus shell's HMAC key (Kconfig APP_BUS_SHELL_AUTH): a build-time secret, never in the repository.
# -Dnode_BUS_KEY_FILE= (sysbuild) / -DBUS_KEY_FILE=, else $T1S_BUS_KEY_FILE, else ~/.config/t1s-keys/bus.key.
if(NOT BUS_KEY_FILE)
  if(DEFINED ENV{T1S_BUS_KEY_FILE})
    set(BUS_KEY_FILE $ENV{T1S_BUS_KEY_FILE})
  else()
    set(BUS_KEY_FILE $ENV{HOME}/.config/t1s-keys/bus.key)
  endif()
endif()

# Which mikroBUS slot holds the Two-Wire ETH click (found by apps/probe).
set(T1S_SLOT 1 CACHE STRING "mikroBUS slot (1-3) of the MIKROE-5543 click")
list(APPEND EXTRA_DTC_OVERLAY_FILE
     ${CMAKE_CURRENT_LIST_DIR}/../../dts/two-wire-eth-click-slot${T1S_SLOT}.overlay)

# zenoh-pico 1.10.1 as a Zephyr module. In a workspace made from this repo's west.yml it is a west project
# (modules/lib/zenoh-pico) and Zephyr finds it by itself. Otherwise (the bench PC's older ~/zephyrproject,
# whose manifest is Zephyr's own) it is added from ZENOH_PICO_DIR: -DZENOH_PICO_DIR=... or the tree the ESP
# T1S HAT firmware vendors. Both are 1.10.1 + zenoh-pico-patches/.
if(NOT ZEPHYR_BASE AND DEFINED ENV{ZEPHYR_BASE})
  set(_zbase $ENV{ZEPHYR_BASE})
else()
  set(_zbase ${ZEPHYR_BASE})
endif()
if(EXISTS ${_zbase}/../modules/lib/zenoh-pico/zephyr/module.yml AND NOT ZENOH_PICO_DIR)
  message(STATUS "zenoh-pico: west project ${_zbase}/../modules/lib/zenoh-pico")
else()
  set(ZENOH_PICO_DIR $ENV{HOME}/esp32-t1s-bridge/vendor/zenoh-pico CACHE PATH "zenoh-pico source")
  if(NOT EXISTS ${ZENOH_PICO_DIR}/zephyr/module.yml)
    message(FATAL_ERROR "zenoh-pico not found: build in a workspace made from west.yml (README), or -DZENOH_PICO_DIR=")
  endif()
  list(APPEND ZEPHYR_EXTRA_MODULES ${ZENOH_PICO_DIR})
endif()

# Zone lamp outputs (left/right/brake/reverse) on a free mikroBUS slot; 0 = none
set(LAMP_SLOT 0 CACHE STRING "mikroBUS slot for plain GPIO lamp outputs (0 = none; the 10x10 panel shows the lamps)")
set(PANEL_SLOT 2 CACHE STRING "mikroBUS slot of the 10x10 RGB click (0 = none)")
set(SENSOR_SLOT 3 CACHE STRING "mikroBUS slot of the thumbstick / proximity click (0 = none)")
foreach(s PANEL_SLOT SENSOR_SLOT)
  if(NOT ${s} STREQUAL "0")
    if(${s} STREQUAL T1S_SLOT)
      message(FATAL_ERROR "${s} ${${s}} is the T1S click's slot")
    endif()
  endif()
endforeach()
if(NOT PANEL_SLOT STREQUAL "0")
  list(APPEND EXTRA_DTC_OVERLAY_FILE ${CMAKE_CURRENT_LIST_DIR}/../../dts/panel-slot${PANEL_SLOT}.overlay)
  if(PANEL_SLOT STREQUAL "2")
    # WS2812 data through SERCOM0 + DMA (the overlay's panel-spi); slot 2's PA11 is SERCOM0 PAD3
    list(APPEND EXTRA_CONF_FILE ${CMAKE_CURRENT_LIST_DIR}/panel-spi.conf)
  endif()
endif()
if(NOT SENSOR_SLOT STREQUAL "0")
  list(APPEND EXTRA_DTC_OVERLAY_FILE ${CMAKE_CURRENT_LIST_DIR}/../../dts/sensors-slot${SENSOR_SLOT}.overlay)
endif()
if(LAMP_SLOT STREQUAL T1S_SLOT)
  message(FATAL_ERROR "LAMP_SLOT ${LAMP_SLOT} is the T1S click's slot: pick another (README: Slot plan)")
endif()
if(NOT LAMP_SLOT STREQUAL "0")
  list(APPEND EXTRA_DTC_OVERLAY_FILE
       ${CMAKE_CURRENT_LIST_DIR}/../../dts/zone-lamps-slot${LAMP_SLOT}.overlay)
endif()

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(t1s_zone_node)

target_sources(app PRIVATE src/main.c src/t1s_diag.c src/ptp.c)
# LAN8651 frame timestamps: needs the LOCAL PATCH in zephyr-patches/ applied to the zephyr tree
zephyr_compile_definitions(LAN865X_FRAME_TIMESTAMPS)
if(CONFIG_APP_ZENOH)
  # the module does not run zenoh-pico's configure step; this is its config.h
  zephyr_include_directories(${CMAKE_CURRENT_LIST_DIR}/zenoh-config)
  zephyr_compile_definitions(ZENOH_LOG_WARN)
  target_sources(app PRIVATE src/zone_zenoh.c src/zone_lamps.c src/zone_panel.c)
endif()
if(CONFIG_APP_BUS_SHELL_AUTH)
  # -> <build>/bus_key/bus_key.h (the build directory is not committed). Missing key = every command refused.
  set(_kdir ${CMAKE_CURRENT_BINARY_DIR}/bus_key)
  if(EXISTS ${BUS_KEY_FILE})
    file(READ ${BUS_KEY_FILE} _k)
    string(STRIP "${_k}" _k)
    string(LENGTH "${_k}" _kl)
    if(NOT _k MATCHES "^[0-9a-fA-F]+$" OR NOT _kl EQUAL 64)
      message(FATAL_ERROR "${BUS_KEY_FILE}: expected 64 hex digits (32 bytes)")
    endif()
    string(TOLOWER "${_k}" _k)
    string(SHA256 _kid "${_k}")
    string(SUBSTRING "${_kid}" 0 8 _kid)
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," _kb "${_k}")
    set(_hdr "/* generated from ${BUS_KEY_FILE} -- secret, do not commit */\n#define BUS_KEY_ID \"${_kid}\"\nstatic const uint8_t bus_key[32] = {${_kb}};\n")
    message(STATUS "bus shell key: ${BUS_KEY_FILE} (id ${_kid})")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${BUS_KEY_FILE})
  else()
    message(WARNING "no bus shell key (${BUS_KEY_FILE}): this image refuses every bus shell command")
    set(_hdr "#define BUS_KEY_MISSING 1\n#define BUS_KEY_ID \"none\"\nstatic const uint8_t bus_key[32];\n")
  endif()
  file(CONFIGURE OUTPUT ${_kdir}/bus_key.h CONTENT "${_hdr}")
  target_include_directories(app PRIVATE ${_kdir})
endif()
if(CONFIG_APP_SMP_FILTER OR CONFIG_APP_IRQ_PROBE)
  target_sources(app PRIVATE src/zone_guard.c)
endif()
if(CONFIG_PTP)
  # ptp.c moves the experimental IEEE 1588 stack (ptp1588.conf) to domain 77 through its private header
  target_include_directories(app PRIVATE ${ZEPHYR_BASE})
endif()
# t1s_diag.c reads PLCA status through the in-tree driver's OA TC6 helpers
target_include_directories(app PRIVATE
  ${ZEPHYR_BASE}/drivers/ethernet
  ${ZEPHYR_BASE}/subsys/net/l2)
