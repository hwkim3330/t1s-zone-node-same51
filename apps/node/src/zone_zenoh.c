/*
 * Zenoh on the zone node: a zenoh-pico client of the PC's zenohd, reached over T1S through the
 * ESP bridge. Keys: Vehicle API v1 (docs/VEHICLE_API.md; generated vehicle_api.h), and for one release also the
 * old ones (migration step 2: publish both, accept both). <role> = zone-left | zone-right (the panel half):
 *   ecu/<role>/status   pub, 1 Hz: {"id":3,"ip":"192.168.100.73","up":12,"pst":1,"cmds":0,"ptp_ns":5,"api":"<hash>"}
 *                       (old: t1s/sam<id>/hello)
 *   ecu/<role>/cmd      sub: "led on" | "led off" | "led blink" | "led auto" | "ping"   (old: t1s/sam<id>/cmd)
 *   ecu/all/cmd         sub: the same, to every node at once                         (old: t1s/all/cmd)
 *   ecu/<role>/cmd/ack  pub: what a command did                                      (old: t1s/sam<id>/ack)
 *   the zone-left node owns the lamp state keys (high / low beam, DRL, parking, the J3134 marker), 1 Hz
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
#include "vehicle_api.h"

LOG_MODULE_REGISTER(zone_zenoh, LOG_LEVEL_INF);

static char locator[64] = CONFIG_APP_ZENOH_LOCATOR;
static volatile bool connected;
static uint32_t n_hello, n_cmd, n_open_fail, n_reconnect;
/* zenoh-pico 1.10 on Zephyr takes recv() == 0 (peer closed) for "no data" and never closes the
 * session, so a dead router or link went unnoticed. Anything we subscribe to counts as proof of life:
 * the vehicle-API sidecar sends vehicle/speed at >= 5 Hz. Nothing for WATCHDOG_MS = reconnect. */
#define WATCHDOG_MS 15000
#define N_SUB 11 /* subscriptions: 4 command keys + the vehicle keys this zone uses (see sub_k) */
static volatile int64_t last_rx_ms;
static z_owned_publisher_t pub_ack[2];
static bool ack_ok;
/* the zone's sensors and outputs: [v1, old] per ZONE_PUB_* (the old key until migration step 5; NULL = none) */
static const char *const sensor_keys[ZONE_PUB_COUNT][2] = {
	{VAPI_P_CABIN_STICK_X, VAPI_P_CABIN_STICK_X_OLD},
	{VAPI_P_CABIN_STICK_Y, VAPI_P_CABIN_STICK_Y_OLD},
	{VAPI_ECU_DIAG_PROXIMITY_RAW, VAPI_ECU_DIAG_PROXIMITY_RAW_OLD},
	{VAPI_ADAS_OBST_REAR_CENTER_DISTANCE, VAPI_ADAS_OBST_REAR_CENTER_DISTANCE_OLD},
	/* this node's 10x10 picture, 300 B RGB row-major, as drawn (before `panel map`) */
	{VAPI_ECU_DISPLAY, VAPI_ECU_DISPLAY_OLD},
	/* the high beam: the stick asks, the headlamp zone (zone-left) owns the state key */
	{VAPI_LIGHT_BEAM_HIGH_ISON_TARGET, VAPI_LIGHT_BEAM_HIGH_ISON_OLD},
	/* the panel view (0 the zone lamps, 1 the cluster picture): the stick's left flick switches every zone */
	{VAPI_ECU_ALL_CONFIG_DISPLAY_VIEW, VAPI_ECU_ALL_CONFIG_DISPLAY_VIEW_OLD},
	{VAPI_ADAS_OBST_REAR_CENTER_ISWARNING, NULL}};
/* the lamp state the headlamp zone shows, as the owner of these keys */
static const char *const lamp_keys[] = {VAPI_LIGHT_BEAM_HIGH_ISON, VAPI_LIGHT_BEAM_LOW_ISON, VAPI_LIGHT_RUNNING_ISON,
					VAPI_LIGHT_PARKING_ISON, VAPI_P_LIGHT_AUTOMATEDDRIVING_ISON};

/* a key from the table: "%s" = this ECU's role, "%u" = its PLCA id */
static void key_fmt(char *out, size_t n, const char *fmt, uint8_t id)
{
	if (strstr(fmt, "%s")) {
		snprintf(out, n, fmt, zone_role());
	} else if (strstr(fmt, "%u")) {
		snprintf(out, n, fmt, id);
	} else {
		snprintf(out, n, "%s", fmt);
	}
}

static z_owned_publisher_t pub_sensor[ZONE_PUB_COUNT][2];
static bool pub_has[ZONE_PUB_COUNT][2];
static volatile bool sensors_ok;
static K_MUTEX_DEFINE(pub_lock);

void zone_publish(int which, const void *b, size_t n)
{
	if (!sensors_ok || which < 0 || which >= ZONE_PUB_COUNT || k_mutex_lock(&pub_lock, K_MSEC(5))) {
		return;
	}
	z_owned_bytes_t pl;

	for (int v = 0; v < 2; v++) {
		if (pub_has[which][v]) {
			z_bytes_copy_from_buf(&pl, b, n);
			z_publisher_put(z_publisher_loan(&pub_sensor[which][v]), z_bytes_move(&pl), NULL);
		}
	}
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
	/* runs in zenoh's read task: wait for pub_lock only briefly (the teardown holds it while it stops this task,
	 * so an unbounded wait could deadlock; K_NO_WAIT lost ~10 % of the acks to the 20 Hz sensor sends) */
	if (!ack_ok || k_mutex_lock(&pub_lock, K_MSEC(10))) {
		return;
	}
	if (ack_ok) {
		for (int v = 0; v < 2; v++) {
			z_owned_bytes_t b;

			z_bytes_copy_from_str(&b, msg);
			z_publisher_put(z_publisher_loan(&pub_ack[v]), z_bytes_move(&b), NULL);
		}
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
	z_bytes_reader_t r = z_bytes_get_reader(z_sample_payload(sample));
	size_t n = z_bytes_reader_read(&r, v, sizeof(v));

	last_rx_ms = k_uptime_get();
	zone_lamps_on_vss(key, v, n);
}

static void zenoh_thread(void *a, void *b, void *c)
{
	char ke[64], msg[160];

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
		z_owned_publisher_t hello[2];
		z_owned_subscriber_t sub[N_SUB];
		z_view_keyexpr_t k;
		/* [v1, old] */
		static const char *const hello_k[2] = {VAPI_ECU_STATUS, VAPI_ECU_STATUS_OLD};
		static const char *const ack_k[2] = {VAPI_ECU_CMD_ACK, VAPI_ECU_CMD_ACK_OLD};
		/*
		 * commands (on_cmd), then the vehicle / config keys this zone uses (on_vehicle) -- not the whole vehicle/ tree: that
		 * brought every other zone's inputs (sticks, sensors) onto this node's receive pool as well
		 */
		static const char *const sub_k[N_SUB] = {
			VAPI_ECU_CMD, VAPI_ECU_CMD_OLD, VAPI_ECU_ALL_CMD, VAPI_ECU_ALL_CMD_OLD,
			VAPI_SPEED, "vehicle/body/lights/**", "vehicle/chassis/**", VAPI_SELECTEDGEAR, VAPI_P_DRIVINGMODE,
			"vehicle/adas/**", VAPI_P_MRM_STATE};
		static char sk[N_SUB][64];
		bool ok = true;

		for (int v = 0; v < 2; v++) {
			key_fmt(ke, sizeof(ke), hello_k[v], id);
			z_view_keyexpr_from_str_unchecked(&k, ke);
			z_declare_publisher(z_session_loan(&s), &hello[v], z_view_keyexpr_loan(&k), NULL);
			key_fmt(ke, sizeof(ke), ack_k[v], id);
			z_view_keyexpr_from_str_unchecked(&k, ke);
			ok &= z_declare_publisher(z_session_loan(&s), &pub_ack[v], z_view_keyexpr_loan(&k), NULL) == 0;
		}
		ack_ok = ok;
		for (int i = 0; i < N_SUB; i++) {
			z_owned_closure_sample_t c;

			key_fmt(sk[i], sizeof(sk[i]), sub_k[i], id);
			z_view_keyexpr_from_str_unchecked(&k, sk[i]);
			z_closure(&c, i < 4 ? on_cmd : on_vehicle, NULL, NULL);
			z_declare_subscriber(z_session_loan(&s), &sub[i], z_view_keyexpr_loan(&k), z_closure_sample_move(&c),
					     NULL);
		}
		/* the shared panel view lives under ecu/all/config: on_vehicle takes it as well */
		z_owned_subscriber_t sub_view;
		z_owned_closure_sample_t cv;

		z_view_keyexpr_from_str_unchecked(&k, VAPI_ECU_ALL_CONFIG_DISPLAY_VIEW);
		z_closure(&cv, on_vehicle, NULL, NULL);
		z_declare_subscriber(z_session_loan(&s), &sub_view, z_view_keyexpr_loan(&k), z_closure_sample_move(&cv), NULL);

		for (int i = 0; i < ZONE_PUB_COUNT; i++) {
			static char pk[ZONE_PUB_COUNT][2][64];

			for (int v = 0; v < 2; v++) {
				pub_has[i][v] = false;
				if (!sensor_keys[i][v]) {
					continue;
				}
				key_fmt(pk[i][v], sizeof(pk[i][v]), sensor_keys[i][v], id);
				z_view_keyexpr_from_str_unchecked(&k, pk[i][v]);
				pub_has[i][v] = z_declare_publisher(z_session_loan(&s), &pub_sensor[i][v],
								    z_view_keyexpr_loan(&k), NULL) == 0;
			}
		}
		sensors_ok = true;
		connected = true;
		last_rx_ms = k_uptime_get();
		LOG_INF("zenoh: connected to %s as ecu/%s (t1s/sam%u), api %s", locator, zone_role(), id, VAPI_HASH);

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
				 "{\"id\":%u,\"ip\":\"%s\",\"up\":%u,\"pst\":%d,\"cmds\":%u,\"ptp_ns\":%s,\"rst\":%u,"
				 "\"role\":\"%s\",\"api\":\"%s\"}", id, ip, (unsigned)(k_uptime_get() / 1000), zone_bus_up(),
				 n_cmd, ptp, zone_reset_cause, zone_role(), VAPI_HASH);
			/*
			 * A put that fails is not a lost session: with the transmit lock busy (the panel thread's
			 * sensors, an ack) zenoh-pico drops the message and returns EBUSY ("System error: 16").
			 * Treating that as a loss made the node reconnect every few seconds under load, and the
			 * teardown raced the other senders (bus fault in _z_wbuf_write, 2026-10-28).
			 */
			if (!k_mutex_lock(&pub_lock, K_MSEC(20))) {
				for (int v = 0; v < 2; v++) {
					z_bytes_copy_from_str(&pl, msg);
					z_publisher_put(z_publisher_loan(&hello[v]), z_bytes_move(&pl), NULL);
				}
				if (!strcmp(zone_role(), "zone-left")) {
					/* the lamp state this zone shows, as the owner of the keys (1 Hz, max_age 2 s) */
					struct zone_vehicle zv;
					bool live = zone_vehicle(&zv);
					const uint8_t lv[] = {live && zv.high_beam, live, live, live, live && zv.mode == 2};

					for (size_t j = 0; j < ARRAY_SIZE(lamp_keys); j++) {
						z_view_keyexpr_t lk;

						z_view_keyexpr_from_str_unchecked(&lk, lamp_keys[j]);
						z_bytes_copy_from_buf(&pl, &lv[j], 1);
						z_put(z_session_loan(&s), z_view_keyexpr_loan(&lk), z_bytes_move(&pl), NULL);
					}
				}
				k_mutex_unlock(&pub_lock);
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
		for (int i = 0; i < N_SUB; i++) {
			z_undeclare_subscriber(z_subscriber_move(&sub[i]));
		}
		z_undeclare_subscriber(z_subscriber_move(&sub_view));
		for (int v = 0; v < 2; v++) {
			z_undeclare_publisher(z_publisher_move(&hello[v]));
			z_undeclare_publisher(z_publisher_move(&pub_ack[v]));
		}
		for (int i = 0; i < ZONE_PUB_COUNT; i++) {
			for (int v = 0; v < 2; v++) {
				if (pub_has[i][v]) {
					z_undeclare_publisher(z_publisher_move(&pub_sensor[i][v]));
					pub_has[i][v] = false;
				}
			}
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
