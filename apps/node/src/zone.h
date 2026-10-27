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
