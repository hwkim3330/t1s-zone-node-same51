/*
 * Time sync on the LAN8651's own 1588 clock (TSU, 25 MHz, 40 ns ticks), with hardware stamps at
 * the SFD on the wire. Same exchange and wire format as the ESP T1S HAT firmware
 * (elite-t1s-hat/firmware, `sync` / `ptp on` / `ptp lock`), so either board can be master:
 *
 *   asker  -> SYNC {magic, seq, t1 (us, asker's uptime), 0, 0}            UDP 5007
 *   answer <- SYNC {magic, seq, t1, t2, t3 (us, answerer's uptime)}
 *   answer <- SYNF {magic, seq, t1, t2, t3 (ns, answerer's LAN8651 clock)}  hardware follow-up
 *
 * `ptp on` makes the stamps (TSU increment, packet matcher on every frame, the driver hooks);
 * the answering side runs always. `ptp lock <ip> [sec] [kp] [ki]` disciplines this node's clock to
 * <ip>'s: 16 exchanges a second, the min-delay one, one step, then PI on the TSU frequency.
 * Offset printed = master - node (ns).
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdlib.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/net_pkt.h>
#include <zephyr/random/random.h>
#include <zephyr/shell/shell.h>
#include <zephyr/logging/log.h>
#include "t1s_diag.h"
#include "oa_tc6.h"

LOG_MODULE_REGISTER(zone_ptp, LOG_LEVEL_INF);

#define SYNC_PORT 5007
#define SYNC_MAGIC 0x53594e43u  /* "SYNC" */
#define SYNC_FOLLOW 0x53594e46u /* "SYNF" */

struct sync_msg {
	uint32_t magic, seq;
	int64_t t1, t2, t3;
};

static const struct device *const t1s = DEVICE_DT_GET(DT_NODELABEL(t1s0));
static bool ptp_on;
static uint32_t n_rx_ts, n_sync_rx, n_tx_ask, n_tx_got, n_tx_miss, n_fu;

struct hw_stamp {
	volatile uint32_t seq;
	volatile int64_t ns;
};
static struct hw_stamp rx_stamp[64];

static int64_t ts_to_ns(uint64_t ts)
{
	return (int64_t)(ts >> 32) * 1000000000LL + (int64_t)(ts & 0xffffffffu);
}

static int64_t now_us(void)
{
	return k_ticks_to_us_floor64(k_uptime_ticks());
}

/* The UDP payload of an IPv4 frame on port 5007, copied out of the packet: its magic and seq. */
static bool sync_of(struct net_pkt *pkt, uint32_t *magic, uint32_t *seq)
{
	uint8_t f[14 + 60 + 8 + 8];
	size_t len = MIN(net_pkt_get_len(pkt), sizeof(f));

	net_pkt_cursor_init(pkt);
	if (net_pkt_read(pkt, f, len)) {
		net_pkt_cursor_init(pkt);
		return false;
	}
	net_pkt_cursor_init(pkt);
	if (len < 14 + 20 + 8 + 8 || f[12] != 0x08 || f[13] != 0x00) {
		return false;
	}
	const uint8_t *ip = f + 14;
	size_t ihl = (ip[0] & 0x0f) * 4;

	if (ip[9] != 17 || len < 14 + ihl + 8 + 8) {
		return false;
	}
	const uint8_t *u = ip + ihl;
	uint16_t sp = u[0] << 8 | u[1], dp = u[2] << 8 | u[3];

	if (sp != SYNC_PORT && dp != SYNC_PORT) {
		return false;
	}
	memcpy(magic, u + 8, 4);
	memcpy(seq, u + 12, 4);
	return true;
}

static void on_rx_ts(struct net_pkt *pkt, uint64_t ts)
{
	uint32_t magic, seq;

	n_rx_ts++;
	if (sync_of(pkt, &magic, &seq) && magic == SYNC_MAGIC) {
		rx_stamp[seq & 63].ns = ts_to_ns(ts);
		rx_stamp[seq & 63].seq = seq;
		n_sync_rx++;
	}
}

static bool want_tx_ts(struct net_pkt *pkt)
{
	uint32_t magic, seq;

	if (sync_of(pkt, &magic, &seq) && magic == SYNC_MAGIC) { /* not the follow-up */
		n_tx_ask++;
		return true;
	}
	return false;
}

static int64_t take_tx_stamp(void)
{
	uint64_t ts;

	if (t1s_take_tx_stamp(t1s, &ts, 5) == 0) {
		n_tx_got++;
		return ts_to_ns(ts);
	}
	n_tx_miss++;
	return -1;
}

#define MMS1(a) (0x10000u | (a))
#define MMS4(a) (0x40000u | (a))

static int64_t tsu_now(void) /* TN first, then TSL: the order the datasheet latches */
{
	uint32_t ns = 0, sl = 0;

	t1s_reg_read(t1s, MMS1(0x75), &ns);
	t1s_reg_read(t1s, MMS1(0x74), &sl);
	return (int64_t)sl * 1000000000LL + ns;
}

static void tsu_step(int64_t d) /* add d ns: TA below 1 s, else set seconds / ns */
{
	if (d > -1000000000LL && d < 1000000000LL) {
		uint32_t v = (d < 0 ? 0x80000000u : 0) | (uint32_t)((d < 0 ? -d : d) & 0x3fffffff);

		t1s_reg_write(t1s, MMS1(0x76), v);
	} else {
		int64_t t = tsu_now() + d;

		t1s_reg_write(t1s, MMS1(0x75), (uint32_t)(t % 1000000000LL));
		t1s_reg_write(t1s, MMS1(0x74), (uint32_t)(t / 1000000000LL));
	}
}

static void tsu_freq(double ppb) /* increment = 40 ns x (1 + ppb e-9), 2^-24 ns resolution */
{
	uint64_t inc = (uint64_t)(40.0 * 16777216.0 * (1.0 + ppb * 1e-9) + 0.5);
	uint32_t ti = inc >> 24, sub = inc & 0xffffff;

	t1s_reg_write(t1s, MMS1(0x6f), (sub & 0xff) << 24 | ((sub >> 8) & 0xffff)); /* TISUBN first */
	t1s_reg_write(t1s, MMS1(0x77), ti);
}

static void ptp_enable(bool on)
{
	/* The stamp is triggered by the PHY's packet matcher at the SFD, which out of reset is off
	 * and set for gPTP Sync only: match every frame (mask 0xFFFFFF, location 0), TX and RX. */
	static const uint16_t mm[][2] = {{0x43, 0x00ff}, {0x44, 0xffff}, {0x45, 0},
					 {0x53, 0x00ff}, {0x54, 0xffff}, {0x55, 0}};

	t1s_reg_write(t1s, MMS1(0x77), 40);
	for (size_t i = 0; i < ARRAY_SIZE(mm); i++) {
		t1s_reg_write(t1s, MMS4(mm[i][0]), mm[i][1]);
	}
	t1s_reg_write(t1s, MMS4(0x40), on ? 2 : 0); /* TXMCTL.TXME */
	t1s_reg_write(t1s, MMS4(0x50), on ? 2 : 0); /* RXMCTL.RXME */
	oa_tc6_rx_ts_cb = on ? on_rx_ts : NULL;
	oa_tc6_tx_ts_want = on ? want_tx_ts : NULL;
	ptp_on = on;
}

/* ------------------------------------------------------------------ answering side */
static void sync_server(void *a, void *b, void *c)
{
	int s = zsock_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	struct sockaddr_in me = {.sin_family = AF_INET, .sin_port = htons(SYNC_PORT)};
	struct sync_msg m;

	zsock_bind(s, (struct sockaddr *)&me, sizeof(me));
	for (;;) {
		struct sockaddr_in from;
		socklen_t fl = sizeof(from);

		if (zsock_recvfrom(s, &m, sizeof(m), 0, (struct sockaddr *)&from, &fl) != sizeof(m) ||
		    m.magic != SYNC_MAGIC) {
			continue;
		}
		m.t2 = now_us();
		m.t3 = now_us();
		zsock_sendto(s, &m, sizeof(m), 0, (struct sockaddr *)&from, fl);
		if (ptp_on) { /* like PTP's Follow_Up: the precise times after the message */
			int64_t t3h = take_tx_stamp();
			struct hw_stamp r = rx_stamp[m.seq & 63];
			struct sync_msg fu = {SYNC_FOLLOW, m.seq, m.t1, r.seq == m.seq ? r.ns : -1, t3h};

			n_fu++;
			zsock_sendto(s, &fu, sizeof(fu), 0, (struct sockaddr *)&from, fl);
		}
	}
}
K_THREAD_DEFINE(sync_tid, 2048, sync_server, NULL, NULL, NULL, 6, 0, 3000);

/* ------------------------------------------------------------------ servo (`ptp lock`) */
static volatile bool lock_run;
static char lock_host[16];
static int lock_secs = 60;
static double lock_kp = 0.2, lock_ki = 0.02; /* the ESP bench's measured best; ptp4l uses 0.7 / 0.3 */
volatile int64_t ptp_last_offset_ns = INT64_MIN;
volatile int64_t ptp_last_delay_ns;

static void lock_thread(void *a, void *b, void *c)
{
	for (;;) {
		while (!lock_run) {
			k_msleep(200);
		}
		int s = zsock_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
		struct sockaddr_in to = {.sin_family = AF_INET, .sin_port = htons(SYNC_PORT)};
		struct timeval tv = {0, 30000}, tf = {0, 5000};
		double integ = 0, freq = 0;
		bool stepped = false;
		uint32_t seq = 0x40000000u;

		zsock_inet_pton(AF_INET, lock_host, &to.sin_addr);
		zsock_setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
		tsu_freq(0);
		printk("ptpl: locking the LAN8651 clock to %s for %d s (kp %.2f ki %.3f)\n", lock_host,
		       lock_secs, lock_kp, lock_ki);
		for (int sec = 0; sec < lock_secs && lock_run; sec++) {
			int64_t best_off = 0, best_dly = INT64_MAX;
			int ok = 0;

			for (int k = 0; k < 16; k++) {
				struct sync_msg m = {SYNC_MAGIC, ++seq, now_us(), 0, 0}, in, fu;
				bool got = false;

				zsock_sendto(s, &m, sizeof(m), 0, (struct sockaddr *)&to, sizeof(to));
				int64_t t1 = take_tx_stamp();

				while (zsock_recv(s, &in, sizeof(in), 0) == sizeof(in)) {
					if (in.magic == SYNC_MAGIC && in.seq == m.seq) {
						got = true;
						break;
					}
				}
				int64_t t2 = in.t2 * 1000, t3 = in.t3 * 1000;
				bool hw = false;

				if (got) {
					zsock_setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tf, sizeof(tf));
					while (zsock_recv(s, &fu, sizeof(fu), 0) == sizeof(fu)) {
						if (fu.seq != m.seq) {
							continue;
						}
						if (fu.magic == SYNC_FOLLOW && fu.t2 >= 0 && fu.t3 >= 0) {
							t2 = fu.t2;
							t3 = fu.t3;
							hw = true;
						}
						break;
					}
					zsock_setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
				}
				struct hw_stamp r = rx_stamp[m.seq & 63];

				if (got && hw && t1 >= 0 && r.seq == m.seq) {
					int64_t t4 = r.ns;
					int64_t off = ((t2 - t1) + (t3 - t4)) / 2;
					int64_t dly = ((t4 - t1) - (t3 - t2)) / 2;

					if (dly < best_dly) {
						best_dly = dly;
						best_off = off;
					}
					ok++;
				}
				k_usleep(sys_rand32_get() % 1000);
				k_msleep(54);
			}
			if (!ok) {
				printk("ptpl: %d s no hardware-stamped answers\n", sec);
				continue;
			}
			if (!stepped) {
				tsu_step(best_off);
				stepped = true;
				printk("ptpl: %d step %lld ns\n", sec, (long long)best_off);
				continue;
			}
			integ += lock_ki * best_off;
			freq = lock_kp * best_off + integ; /* ppb, + = node slow -> speed up */
			freq = CLAMP(freq, -200000, 200000);
			tsu_freq(freq);
			ptp_last_offset_ns = best_off;
			ptp_last_delay_ns = best_dly;
			printk("ptpl: %d offset %lld ns delay %lld ns freq %+d ppb (%d/16)\n", sec,
			       (long long)best_off, (long long)best_dly, (int)freq, ok);
		}
		zsock_close(s);
		printk("ptpl: done, holding %+d ppb\n", (int)freq);
		lock_run = false;
	}
}
K_THREAD_DEFINE(lock_tid, 3072, lock_thread, NULL, NULL, NULL, 5, 0, 3000);

static int cmd_ptp(const struct shell *sh, size_t argc, char **argv)
{
	if (argc > 1 && (!strcmp(argv[1], "on") || !strcmp(argv[1], "off"))) {
		ptp_enable(!strcmp(argv[1], "on"));
	} else if (argc > 2 && !strcmp(argv[1], "lock")) {
		if (!ptp_on) {
			shell_print(sh, "ptpl: run `ptp on` first");
			return -EINVAL;
		}
		strncpy(lock_host, argv[2], sizeof(lock_host) - 1);
		lock_secs = argc > 3 ? CLAMP(atoi(argv[3]), 3, 86400) : 60;
		lock_kp = argc > 4 ? strtod(argv[4], NULL) : 0.2;
		lock_ki = argc > 5 ? strtod(argv[5], NULL) : 0.02;
		lock_run = true;
		return 0;
	} else if (argc > 1 && !strcmp(argv[1], "stop")) {
		lock_run = false;
		return 0;
	} else if (argc > 1) {
		shell_print(sh, "ptp [on|off] | ptp lock <master ip> [sec] [kp] [ki] | ptp stop");
		return -EINVAL;
	}
	uint32_t c0 = 0, ti = 0;
	int64_t now = tsu_now();

	t1s_reg_read(t1s, 0x00000004, &c0);
	t1s_reg_read(t1s, MMS1(0x77), &ti);
	shell_print(sh, "ptp: %s, OA_CONFIG0 0x%08x (FTSE %u FTSS %u), TSU increment %u ns, clock %lld.%09lld s",
		    ptp_on ? "hardware stamps on" : "off", c0, (c0 >> 7) & 1, (c0 >> 6) & 1, ti,
		    (long long)(now / 1000000000LL), (long long)(now % 1000000000LL));
	shell_print(sh, "ptp: ingress stamps %u (sync %u), egress asked %u got %u missed %u, follow-ups %u, "
		    "lock %s, last offset %lld ns delay %lld ns", n_rx_ts, n_sync_rx, n_tx_ask, n_tx_got,
		    n_tx_miss, n_fu, lock_run ? lock_host : "off",
		    (long long)(ptp_last_offset_ns == INT64_MIN ? 0 : ptp_last_offset_ns),
		    (long long)ptp_last_delay_ns);
	return 0;
}
SHELL_CMD_REGISTER(ptp, NULL, "ptp [on|off] | ptp lock <ip> [sec] [kp] [ki] | ptp stop", cmd_ptp);
