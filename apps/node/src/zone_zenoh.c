/*
 * Zenoh on the zone node: a zenoh-pico client of the PC's zenohd, reached over T1S through the
 * ESP bridge. Keys (same shape as the ESP T1S HAT's `t1s/<node>/...`):
 *   t1s/sam<id>/hello   pub, 1 Hz: {"id":3,"ip":"192.168.100.73","up":12,"pst":1,"cmds":0}
 *   t1s/sam<id>/cmd     sub: "led on" | "led off" | "led blink" | "led auto" | "ping"
 *   t1s/all/cmd         sub: the same, to every node at once
 *   t1s/sam<id>/ack     pub: what a command did
 * Router: CONFIG_APP_ZENOH_LOCATOR, or the shell `zenoh <locator>` (saved at once).
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdio.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/settings/settings.h>
#include <zephyr/logging/log.h>
#include <zenoh-pico.h>
#include "zone.h"

LOG_MODULE_REGISTER(zone_zenoh, LOG_LEVEL_INF);

static char locator[64] = CONFIG_APP_ZENOH_LOCATOR;
static volatile bool connected;
static uint32_t n_hello, n_cmd, n_open_fail, n_reconnect;
/* zenoh-pico 1.10 on Zephyr takes recv() == 0 (peer closed) for "no data" and never closes the
 * session, so a dead router or link went unnoticed. Anything we subscribe to counts as proof of life:
 * the vehicle-API sidecar sends vehicle/speed at >= 5 Hz. Nothing for WATCHDOG_MS = reconnect. */
#define WATCHDOG_MS 15000
static volatile int64_t last_rx_ms;
static z_owned_publisher_t pub_ack;
static bool ack_ok;
/* the zone's sensors, on the keys the ESP zone controller uses (the vehicle-API sidecar forwards external/...) */
static const char *const sensor_keys[ZONE_PUB_COUNT] = {
	"vehicle/private/zone/stick/x", "vehicle/private/zone/stick/y",
	"vehicle/private/zone/proximity/raw", "vehicle/private/external/proximity_rear",
	"t1s/sam%u/panel" /* this node's 10x10 picture, 300 B RGB row-major, as drawn (before `panel map`) */};
static z_owned_publisher_t pub_sensor[ZONE_PUB_COUNT];
static volatile bool sensors_ok;
static K_MUTEX_DEFINE(pub_lock);

void zone_publish(int which, const void *b, size_t n)
{
	if (!sensors_ok || which < 0 || which >= ZONE_PUB_COUNT || k_mutex_lock(&pub_lock, K_MSEC(5))) {
		return;
	}
	z_owned_bytes_t pl;

	z_bytes_copy_from_buf(&pl, b, n);
	z_publisher_put(z_publisher_loan(&pub_sensor[which]), z_bytes_move(&pl), NULL);
	k_mutex_unlock(&pub_lock);
}

static int zz_set(const char *key, size_t len, settings_read_cb read, void *arg)
{
	if (!strcmp(key, "locator") && len < sizeof(locator)) {
		int n = read(arg, locator, len);

		locator[n > 0 ? n : 0] = 0;
	}
	return 0;
}
SETTINGS_STATIC_HANDLER_DEFINE(zz, "zenoh", NULL, zz_set, NULL, NULL);

static void ack(const char *msg)
{
	/* runs in zenoh's read task: never wait for pub_lock (the teardown holds it while it stops this task) */
	if (!ack_ok || k_mutex_lock(&pub_lock, K_NO_WAIT)) {
		return;
	}
	if (ack_ok) {
		z_owned_bytes_t b;

		z_bytes_copy_from_str(&b, msg);
		z_publisher_put(z_publisher_loan(&pub_ack), z_bytes_move(&b), NULL);
	}
	k_mutex_unlock(&pub_lock);
}

static void on_cmd(z_loaned_sample_t *sample, void *arg)
{
	z_owned_string_t s;
	char cmd[48] = {0};

	z_bytes_to_string(z_sample_payload(sample), &s);
	size_t n = MIN(z_string_len(z_string_loan(&s)), sizeof(cmd) - 1);

	memcpy(cmd, z_string_data(z_string_loan(&s)), n);
	z_string_drop(z_string_move(&s));
	n_cmd++;
	last_rx_ms = k_uptime_get();

	if (!strncmp(cmd, "led ", 4)) {
		const char *m = cmd + 4;
		int mode = !strcmp(m, "on") ? ZONE_LED_ON : !strcmp(m, "off") ? ZONE_LED_OFF
			 : !strcmp(m, "blink") ? ZONE_LED_BLINK : ZONE_LED_AUTO;

		zone_led_mode = mode;
		LOG_INF("cmd '%s' -> led mode %d", cmd, mode);
		ack(cmd);
	} else if (!strcmp(cmd, "ping")) {
		ack("pong");
	} else {
		LOG_WRN("cmd '%s' not understood", cmd);
		ack("unknown");
	}
}

static void on_vehicle(z_loaned_sample_t *sample, void *arg)
{
	z_view_string_t ks;
	char key[96];
	uint8_t v[8];

	z_keyexpr_as_view_string(z_sample_keyexpr(sample), &ks);
	size_t kl = MIN(z_string_len(z_view_string_loan(&ks)), sizeof(key) - 1);

	memcpy(key, z_string_data(z_view_string_loan(&ks)), kl);
	key[kl] = 0;
	if (kl <= 8) {
		return;
	}
	z_bytes_reader_t r = z_bytes_get_reader(z_sample_payload(sample));
	size_t n = z_bytes_reader_read(&r, v, sizeof(v));

	last_rx_ms = k_uptime_get();
	zone_lamps_on_vss(key + 8, v, n); /* after "vehicle/" */
}

static void zenoh_thread(void *a, void *b, void *c)
{
	char ke[40], msg[128];

	while (!zone_bus_up()) {
		k_msleep(500);
	}
	for (;;) {
		z_owned_config_t cfg;
		z_owned_session_t s;

		z_config_default(&cfg);
		zp_config_insert(z_config_loan_mut(&cfg), Z_CONFIG_MODE_KEY, "client");
		zp_config_insert(z_config_loan_mut(&cfg), Z_CONFIG_CONNECT_KEY, locator);
		if (z_open(&s, z_config_move(&cfg), NULL) < 0) {
			if (n_open_fail++ % 10 == 0) {
				LOG_WRN("zenoh: cannot reach %s (try %u)", locator, n_open_fail);
			}
			k_msleep(3000);
			continue;
		}
		zp_start_read_task(z_session_loan_mut(&s), NULL);
		zp_start_lease_task(z_session_loan_mut(&s), NULL);

		uint8_t id = zone_plca_id();
		z_owned_publisher_t hello;
		z_owned_subscriber_t sub1, sub2, sub3;
		z_owned_closure_sample_t c1, c2, c3;
		z_view_keyexpr_t k;

		snprintf(ke, sizeof(ke), "t1s/sam%u/hello", id);
		z_view_keyexpr_from_str_unchecked(&k, ke);
		z_declare_publisher(z_session_loan(&s), &hello, z_view_keyexpr_loan(&k), NULL);
		snprintf(ke, sizeof(ke), "t1s/sam%u/ack", id);
		z_view_keyexpr_from_str_unchecked(&k, ke);
		ack_ok = z_declare_publisher(z_session_loan(&s), &pub_ack, z_view_keyexpr_loan(&k), NULL) == 0;
		snprintf(ke, sizeof(ke), "t1s/sam%u/cmd", id);
		z_view_keyexpr_from_str_unchecked(&k, ke);
		z_closure(&c1, on_cmd, NULL, NULL);
		z_declare_subscriber(z_session_loan(&s), &sub1, z_view_keyexpr_loan(&k), z_closure_sample_move(&c1), NULL);
		z_view_keyexpr_from_str_unchecked(&k, "t1s/all/cmd");
		z_closure(&c2, on_cmd, NULL, NULL);
		z_declare_subscriber(z_session_loan(&s), &sub2, z_view_keyexpr_loan(&k), z_closure_sample_move(&c2), NULL);

		/* the vehicle API: body lights, gear, speed (liveness) */
		z_view_keyexpr_from_str_unchecked(&k, "vehicle/**");
		z_closure(&c3, on_vehicle, NULL, NULL);
		z_declare_subscriber(z_session_loan(&s), &sub3, z_view_keyexpr_loan(&k), z_closure_sample_move(&c3), NULL);

		for (int i = 0; i < ZONE_PUB_COUNT; i++) {
			static char pk[ZONE_PUB_COUNT][40];

			snprintf(pk[i], sizeof(pk[i]), sensor_keys[i], id);
			z_view_keyexpr_from_str_unchecked(&k, pk[i]);
			z_declare_publisher(z_session_loan(&s), &pub_sensor[i], z_view_keyexpr_loan(&k), NULL);
		}
		sensors_ok = true;
		connected = true;
		last_rx_ms = k_uptime_get();
		LOG_INF("zenoh: connected to %s as t1s/sam%u", locator, id);

		/* z_publisher_put fails once the lease task has closed a dead session */
		for (;;) {
			char ip[16];
			z_owned_bytes_t pl;

			zone_ip_str(ip, sizeof(ip));
			int64_t po = zone_ptp_offset_ns();
			char ptp[24] = "null";

			if (po != INT64_MIN) {
				snprintf(ptp, sizeof(ptp), "%lld", (long long)po);
			}
			snprintf(msg, sizeof(msg),
				 "{\"id\":%u,\"ip\":\"%s\",\"up\":%u,\"pst\":%d,\"cmds\":%u,\"ptp_ns\":%s,\"rst\":%u}", id, ip,
				 (unsigned)(k_uptime_get() / 1000), zone_bus_up(), n_cmd, ptp, zone_reset_cause);
			z_bytes_copy_from_str(&pl, msg);
			/*
			 * A put that fails is not a lost session: with the transmit lock busy (the panel thread's
			 * sensors, an ack) zenoh-pico drops the message and returns EBUSY ("System error: 16").
			 * Treating that as a loss made the node reconnect every few seconds under load, and the
			 * teardown raced the other senders (bus fault in _z_wbuf_write, 2026-10-28).
			 */
			if (!k_mutex_lock(&pub_lock, K_MSEC(20))) {
				z_publisher_put(z_publisher_loan(&hello), z_bytes_move(&pl), NULL);
				k_mutex_unlock(&pub_lock);
			} else {
				z_bytes_drop(z_bytes_move(&pl));
			}
			if (z_session_is_closed(z_session_loan(&s))) {
				break;
			}
			if (k_uptime_get() - last_rx_ms > WATCHDOG_MS) {
				LOG_WRN("zenoh: nothing received for %d s, reconnecting", WATCHDOG_MS / 1000);
				n_reconnect++;
				break;
			}
			n_hello++;
			k_msleep(1000);
		}
		LOG_WRN("zenoh: session lost, reconnecting");
		/* no sender may touch the session from here: then the tasks stop, then everything is freed */
		k_mutex_lock(&pub_lock, K_FOREVER);
		connected = false;
		ack_ok = false;
		sensors_ok = false;
		zp_stop_read_task(z_session_loan_mut(&s));
		zp_stop_lease_task(z_session_loan_mut(&s));
		z_undeclare_subscriber(z_subscriber_move(&sub1));
		z_undeclare_subscriber(z_subscriber_move(&sub2));
		z_undeclare_subscriber(z_subscriber_move(&sub3));
		z_undeclare_publisher(z_publisher_move(&hello));
		z_undeclare_publisher(z_publisher_move(&pub_ack));
		for (int i = 0; i < ZONE_PUB_COUNT; i++) {
			z_undeclare_publisher(z_publisher_move(&pub_sensor[i]));
		}
		z_session_drop(z_session_move(&s));
		k_mutex_unlock(&pub_lock);
		k_msleep(1000);
	}
}

K_THREAD_DEFINE(zenoh_tid, 6144, zenoh_thread, NULL, NULL, NULL, 7, 0, 2000);

static int cmd_zenoh(const struct shell *sh, size_t argc, char **argv)
{
	if (argc > 1) {
		strncpy(locator, argv[1], sizeof(locator) - 1);
		settings_save_one("zenoh/locator", locator, strlen(locator));
		shell_print(sh, "zenoh: router %s saved (takes effect on reconnect / reboot)", locator);
		return 0;
	}
	shell_print(sh, "zenoh: %s %s | hello sent %u | cmds %u | open failures %u | watchdog reconnects %u | "
		    "last rx %lld ms ago", connected ? "connected to" : "not connected, router", locator, n_hello,
		    n_cmd, n_open_fail, n_reconnect, (long long)(k_uptime_get() - last_rx_ms));
	return 0;
}
SHELL_CMD_REGISTER(zenoh, NULL, "zenoh [locator]: status, or set the router (saved)", cmd_zenoh);
