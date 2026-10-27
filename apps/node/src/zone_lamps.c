/*
 * Zone lamps from the vehicle API: the ZoneLink VSS keys (`vehicle/<lower-case VSS path>`, one
 * little-endian scalar each, published by korea-autoware-sim/sdv/zonelink.py from the sim or from
 * Autoware through the PLEOS gateway) drive the lamp outputs of dts/zone-lamps-slot1.overlay.
 *
 *   body/lights/directionindicator/left|right/issignaling  u8  -> left / right indicator
 *   body/lights/hazard/issignaling                          u8  -> both indicators
 *   body/lights/brake/isactive                              u8  -> brake
 *   body/lights/backup/ison                                 u8  -> reverse
 *   speed                                                   f32 -> liveness (>= 5 Hz from the sidecar)
 *
 * Indicators blink at 1.5 Hz (UN R48: 90 +- 30 per minute), lit first, phase taken from the LAN8651
 * PTP clock: nodes whose clocks are locked (`ptp lock`, later gPTP) blink exactly together.
 * No `vehicle/speed` for 3 s = the car is gone: all lamps off.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/ptp_clock.h>
#include <zephyr/net/ethernet.h>
#include <zephyr/net/net_if.h>
#include <zephyr/shell/shell.h>
#include "zone.h"

#define BLINK_PERIOD_NS 666666667LL
#define STALE_MS 3000

#define LAMP(n) GPIO_DT_SPEC_GET_OR(DT_NODELABEL(n), gpios, {0})
static const struct gpio_dt_spec out_left = LAMP(lamp_left), out_right = LAMP(lamp_right),
				 out_brake = LAMP(lamp_brake), out_rev = LAMP(lamp_reverse);

static volatile uint8_t sig_left, sig_right, sig_hazard, sig_brake, sig_rev, gear = 126;
static volatile int64_t last_live;
static uint32_t n_keys;
volatile int zone_lamp_led = -1; /* LED0 override from the lamps: -1 none, 0/1 */

static bool lit_now(void)
{
	static const struct device *clk;
	struct net_ptp_time t;

	if (!clk) {
		clk = net_eth_get_ptp_clock(net_if_get_default());
	}
	int64_t ns = (clk && ptp_clock_get(clk, &t) == 0)
			     ? (int64_t)t.second * NSEC_PER_SEC + t.nanosecond
			     : k_ticks_to_ns_floor64(k_uptime_ticks());

	return (ns % BLINK_PERIOD_NS) < BLINK_PERIOD_NS / 2;
}

void zone_lamps_on_vss(const char *k, const uint8_t *b, size_t n)
{
	if (!n) {
		return;
	}
	n_keys++;
	if (!strcmp(k, "speed")) {
		last_live = k_uptime_get();
	} else if (!strcmp(k, "body/lights/directionindicator/left/issignaling")) {
		sig_left = b[0];
	} else if (!strcmp(k, "body/lights/directionindicator/right/issignaling")) {
		sig_right = b[0];
	} else if (!strcmp(k, "body/lights/hazard/issignaling")) {
		sig_hazard = b[0];
	} else if (!strcmp(k, "body/lights/brake/isactive")) {
		sig_brake = b[0];
	} else if (!strcmp(k, "body/lights/backup/ison")) {
		sig_rev = b[0];
	} else if (!strcmp(k, "powertrain/transmission/selectedgear")) {
		gear = b[0];
	}
}

static void set(const struct gpio_dt_spec *o, int v)
{
	if (o->port) {
		gpio_pin_set_dt(o, v);
	}
}

static void lamps_thread(void *a, void *b, void *c)
{
	const struct gpio_dt_spec *all[] = {&out_left, &out_right, &out_brake, &out_rev};

	for (size_t i = 0; i < ARRAY_SIZE(all); i++) {
		if (all[i]->port && device_is_ready(all[i]->port)) {
			gpio_pin_configure_dt(all[i], GPIO_OUTPUT_INACTIVE);
		}
	}
	for (;;) {
		bool live = last_live && k_uptime_get() - last_live < STALE_MS;
		bool l = live && (sig_left || sig_hazard), r = live && (sig_right || sig_hazard);
		bool on = (l || r) && lit_now();

		set(&out_left, l && on);
		set(&out_right, r && on);
		set(&out_brake, live && sig_brake);
		set(&out_rev, live && sig_rev);
		zone_lamp_led = (l || r) ? on : -1;
		k_msleep(20);
	}
}
K_THREAD_DEFINE(lamps_tid, 1024, lamps_thread, NULL, NULL, NULL, 8, 0, 500);

static int cmd_lamps(const struct shell *sh, size_t argc, char **argv)
{
	int64_t age = last_live ? k_uptime_get() - last_live : -1;

	shell_print(sh, "lamps: %s (speed %lld ms ago) | left %u right %u hazard %u brake %u reverse %u gear %d | "
		    "keys %u | outputs %s", age >= 0 && age < STALE_MS ? "live" : "NO CAR", (long long)age,
		    sig_left, sig_right, sig_hazard, sig_brake, sig_rev, (int8_t)gear, n_keys,
		    out_left.port ? "slot overlay" : "none (LAMP_SLOT=0)");
	return 0;
}
SHELL_CMD_REGISTER(lamps, NULL, "vehicle lamp state from the ZoneLink VSS keys", cmd_lamps);
