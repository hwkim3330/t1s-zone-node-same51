/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* LED0: AUTO = bus status (solid = beacons, slow blink = waiting); the rest are set over Zenoh. */
enum { ZONE_LED_AUTO, ZONE_LED_ON, ZONE_LED_OFF, ZONE_LED_BLINK };
extern volatile int zone_led_mode;

bool zone_bus_up(void);              /* PLCA active: the coordinator's beacons are heard */
uint8_t zone_plca_id(void);
void zone_ip_str(char *buf, size_t len);

/* zone_lamps.c: the ZoneLink VSS keys (k = key below "vehicle/") and its LED0 request (-1 none) */
void zone_lamps_on_vss(const char *k, const uint8_t *b, size_t n);
extern volatile int zone_lamp_led;

/* ptp.c: last offset to the master (ns), INT64_MIN when not locked */
int64_t zone_ptp_offset_ns(void);
