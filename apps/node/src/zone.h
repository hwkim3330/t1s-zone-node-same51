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

/* zone_lamps.c: the VSS keys (k = key below "vehicle/") and its LED0 request (-1 none) */
void zone_lamps_on_vss(const char *k, const uint8_t *b, size_t n);
extern volatile int zone_lamp_led;

/* ptp.c: last offset to the master (ns), INT64_MIN when not locked */
int64_t zone_ptp_offset_ns(void);

/* zone_lamps.c: the vehicle as the VSS keys describe it, for the 10x10 panel; false = no car (stale) */
struct zone_vehicle {
	bool left, right, lit, hazard, brake, reverse, high_beam;
	char gear;
	float speed_kmh, wheel_deg, accel, brake_pedal, prox_m;
	uint8_t mode;
};
bool zone_vehicle(struct zone_vehicle *v);
double zone_clock_s(void); /* the PTP clock in seconds (shared by locked zones), uptime without it */

/* zone_zenoh.c: publish one sensor value (key below "vehicle/", one little-endian scalar) if connected */
enum { ZONE_PUB_STICK_X, ZONE_PUB_STICK_Y, ZONE_PUB_PROX_RAW, ZONE_PUB_PROX_M, ZONE_PUB_PANEL, ZONE_PUB_BEAM, ZONE_PUB_VIEW, ZONE_PUB_PROX_WARN, ZONE_PUB_COUNT };
#define PROX_WARN_M 1.5f
const char *zone_role(void); /* "zone-left" | "zone-right" (the panel half) */
void zone_publish(int which, const void *b, size_t n);

/* main.c: RSTC.RCAUSE at boot (bit 5 watchdog, 6 software, 4 external, 0 power-on) */
extern uint8_t zone_reset_cause;

/* zone_lamps.c: the panel view every zone shows (0 lamps, 1 cluster), switched from the stick, -1 = none heard */
int zone_panel_view(void);
