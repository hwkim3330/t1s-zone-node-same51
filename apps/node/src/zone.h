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
	uint8_t mrm; /* vehicle/private/mrm/state: 0 none, 1 stopping, 2 stopped */
	char gear;
	float speed_kmh, wheel_deg, accel, brake_pedal, prox_m;
	uint8_t mode;
};
bool zone_vehicle(struct zone_vehicle *v);
double zone_clock_s(void); /* the PTP clock in seconds (shared by locked zones), uptime without it */

/* zone_zenoh.c: publish one sensor value (key below "vehicle/", one little-endian scalar) if connected */
enum { ZONE_PUB_STICK_X, ZONE_PUB_STICK_Y, ZONE_PUB_PROX_RAW, ZONE_PUB_PROX_M, ZONE_PUB_PANEL, ZONE_PUB_BEAM, ZONE_PUB_VIEW, ZONE_PUB_PROX_WARN, ZONE_PUB_COUNT };
#define PROX_WARN_M 1.5f
/* main.c: the ECU's role on the Vehicle API (ecu/<role>/...): saved (`zone role`) > MAC table > Kconfig APP_ROLE >
 * the panel half / PLCA id parity (odd zone-left, even zone-right). Unique per board: two nodes with one role
 * collide on ecu/<role>/status and the console cannot address them (2026-11-02, nodes 3 and 5). */
const char *zone_role(void);
bool zone_role_valid(const char *r);
int zone_panel_half_cfg(void); /* zone_panel.c: the saved `panel half` (0 left, 1 right), -1 = none */
/* zone_zenoh.c: true once connected and the first heartbeat is out (the OTA health check); re-declare the keys */
bool zone_zenoh_healthy(void);
void zone_zenoh_rekey(void);
uint32_t zone_vss_keys(void); /* zone_lamps.c: vehicle keys received since boot */
/* main.c: the running image is confirmed (MCUboot keeps it); what the health check still waits for */
bool zone_img_confirmed(void);
const char *zone_health_str(void);
/* zone_guard.c: SMP/UDP arming (seconds left, -1 = no arming in this build), IRQ latency probe */
int zone_ota_arm(int seconds);
int zone_ota_armed_s(void);
uint32_t zone_smp_accepted(void); /* SMP datagrams let through since boot */
uint32_t zone_irq_lat_max_ns(void);
void zone_publish(int which, const void *b, size_t n);

/* main.c: RSTC.RCAUSE at boot (bit 5 watchdog, 6 software, 4 external, 0 power-on) */
extern uint8_t zone_reset_cause;

/* zone_lamps.c: the panel view every zone shows (0 lamps, 1 cluster), switched from the stick, -1 = none heard */
int zone_panel_view(void);
int zone_high_beam(void); /* the high beam as requested / as the headlamp zone reports it */
