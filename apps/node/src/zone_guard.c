/*
 * Two guards of the zone node:
 *
 * 1. mcumgr SMP over UDP (port 1337, the firmware update) is filtered in the IPv4 input (Zephyr packet filter,
 *    IPv4 hook), before the SMP server sees anything:
 *      - only from CONFIG_APP_SMP_ALLOWED_SRC (the bench PC, 192.168.100.51), and
 *      - only while armed: `ota arm [s]` (CONFIG_APP_SMP_ARM), from the USB console or the authenticated bus shell
 *        (tools/smp_udp.py arms each node first). The window closes by itself and at every boot.
 *    The image itself is still checked by MCUboot (signature); this keeps everybody else on the bus from erasing
 *    slot 1, uploading, marking test boots or resetting the node.
 *
 * 2. IRQ latency probe (CONFIG_APP_IRQ_PROBE): TC4 counts at 7.5 MHz (GCLK0 / 16) and interrupts every 8 ms at the
 *    priority of the EIC (the LAN8651's IRQ_N); the ISR reads how far the counter has run past the match = how
 *    late the interrupt was taken. `irqlat [reset]`.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdlib.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/irq.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/net_pkt.h>
#include <zephyr/net/net_pkt_filter.h>
#include <zephyr/shell/shell.h>
#include <zephyr/logging/log.h>
#include <soc.h>
#include "zone.h"

LOG_MODULE_REGISTER(zone_guard, LOG_LEVEL_INF);

/* ------------------------------------------------------------------ SMP filter */
#ifdef CONFIG_APP_SMP_FILTER
#define SMP_PORT 1337

static struct in_addr smp_src;
static bool smp_src_any;
static volatile int64_t armed_until; /* uptime ms; 0 = closed */
static uint32_t n_smp_ok, n_smp_drop_src, n_smp_drop_closed;

static bool smp_armed(void)
{
#ifdef CONFIG_APP_SMP_ARM
	return armed_until && k_uptime_get() < armed_until;
#else
	return true;
#endif
}

/* true = drop: UDP to port 1337 from a source that is not allowed, or while closed */
static bool smp_reject(struct npf_test *t, struct net_pkt *pkt)
{
	struct net_pkt_cursor backup;
	uint8_t ip[20], udp[4];
	bool drop = false;

	ARG_UNUSED(t);
	net_pkt_cursor_backup(pkt, &backup);
	net_pkt_cursor_init(pkt);
	if (net_pkt_read(pkt, ip, sizeof(ip)) == 0 && (ip[0] >> 4) == 4 && ip[9] == IPPROTO_UDP &&
	    (ip[6] & 0x1f) == 0 && ip[7] == 0) { /* first fragment only carries the port */
		size_t ihl = (ip[0] & 0x0f) * 4;

		if (ihl >= 20 && net_pkt_skip(pkt, ihl - 20) == 0 && net_pkt_read(pkt, udp, sizeof(udp)) == 0 &&
		    (udp[2] << 8 | udp[3]) == SMP_PORT) {
			if (!smp_src_any && memcmp(&ip[12], &smp_src, 4)) {
				n_smp_drop_src++;
				drop = true;
			} else if (!smp_armed()) {
				n_smp_drop_closed++;
				drop = true;
			} else {
				n_smp_ok++;
			}
		}
	}
	net_pkt_cursor_restore(pkt, &backup);
	return drop;
}

static struct npf_test smp_test = {.fn = smp_reject};
static struct npf_rule smp_rule = {
	.result = NET_DROP,
	.nb_tests = 1,
	.tests = {&smp_test},
};

static int smp_filter_init(void)
{
	smp_src_any = !strlen(CONFIG_APP_SMP_ALLOWED_SRC) ||
		      net_addr_pton(AF_INET, CONFIG_APP_SMP_ALLOWED_SRC, &smp_src) != 0;
	/* a list with rules drops what no rule matched: the drop rule, then accept everything else */
	npf_append_ipv4_recv_rule(&smp_rule);
	npf_append_ipv4_recv_rule(&npf_default_ok);
	return 0;
}
SYS_INIT(smp_filter_init, APPLICATION, 0);

uint32_t zone_smp_accepted(void) { return n_smp_ok; }

int zone_ota_arm(int seconds)
{
	armed_until = seconds > 0 ? k_uptime_get() + (int64_t)seconds * 1000 : 0;
	LOG_INF("SMP/UDP %s", seconds > 0 ? "armed" : "closed");
	return 0;
}

int zone_ota_armed_s(void)
{
#ifdef CONFIG_APP_SMP_ARM
	return smp_armed() ? (int)((armed_until - k_uptime_get() + 999) / 1000) : 0;
#else
	return -1; /* always open */
#endif
}

static int cmd_ota(const struct shell *sh, size_t argc, char **argv)
{
	if (argc > 1 && !strcmp(argv[1], "arm")) {
		int s = argc > 2 ? CLAMP(atoi(argv[2]), 1, 3600) : CONFIG_APP_SMP_ARM_S;

		zone_ota_arm(s);
	} else if (argc > 1 && !strcmp(argv[1], "close")) {
		zone_ota_arm(0);
	} else if (argc > 2 && !strcmp(argv[1], "src")) {
		/* until reboot (then CONFIG_APP_SMP_ALLOWED_SRC again): another management host, or a test of the filter */
		struct in_addr a;

		if (!strcmp(argv[2], "any")) {
			smp_src_any = true;
		} else if (net_addr_pton(AF_INET, argv[2], &a) == 0) {
			smp_src = a;
			smp_src_any = false;
		} else {
			shell_print(sh, "ota src <a.b.c.d>|any");
			return -EINVAL;
		}
	} else if (argc > 1) {
		shell_print(sh, "ota [arm [s]|close]");
		return -EINVAL;
	}
	int a = zone_ota_armed_s();

	char src[NET_IPV4_ADDR_LEN];

	net_addr_ntop(AF_INET, &smp_src, src, sizeof(src));
	shell_print(sh, "ota: SMP/UDP %d from %s, %s | accepted %u, dropped: source %u, closed %u | image %s",
		    SMP_PORT, smp_src_any ? "any source" : src,
		    a < 0 ? "always open (no arming)" : a ? "ARMED" : "closed (`ota arm [s]`)",
		    n_smp_ok, n_smp_drop_src, n_smp_drop_closed, zone_health_str());
	if (a > 0) {
		shell_print(sh, "ota: armed for %d s more", a);
	}
	return 0;
}
SHELL_CMD_REGISTER(ota, NULL, "ota [arm [s]|close|src <ip>|any]: SMP/UDP firmware update window", cmd_ota);
#else
int zone_ota_armed_s(void) { return -1; }
uint32_t zone_smp_accepted(void) { return 0; }
int zone_ota_arm(int seconds) { ARG_UNUSED(seconds); return -ENOTSUP; }
#endif /* CONFIG_APP_SMP_FILTER */

/* ------------------------------------------------------------------ IRQ latency probe */
#ifdef CONFIG_APP_IRQ_PROBE
#define PROBE_DIV 16                         /* GCLK0 120 MHz / 16 = 7.5 MHz, 133 ns per count */
#define PROBE_TOP (8 * 7500 - 1)             /* 8 ms: longer than the WS2812 bit-bang's 3 ms with interrupts off */
#define TICK_NS 133.333

static volatile uint32_t lat_max, lat_n;
static volatile uint64_t lat_sum;
static volatile uint32_t lat_hist[6];        /* < 2 us, < 10 us, < 50 us, < 500 us, < 2 ms, >= 2 ms */
static const uint32_t hist_edge[5] = {15, 75, 375, 3750, 15000}; /* in counts */

static void probe_isr(const void *arg)
{
	Tc *tc = TC4;

	ARG_UNUSED(arg);
	tc->COUNT16.CTRLBSET.reg = TC_CTRLBSET_CMD_READSYNC;
	while (tc->COUNT16.SYNCBUSY.bit.CTRLB || tc->COUNT16.SYNCBUSY.bit.COUNT) {
	}
	uint32_t c = tc->COUNT16.COUNT.reg;

	tc->COUNT16.INTFLAG.reg = TC_INTFLAG_MC0;
	lat_n++;
	lat_sum += c;
	if (c > lat_max) {
		lat_max = c;
	}
	int b = 0;

	while (b < 5 && c >= hist_edge[b]) {
		b++;
	}
	lat_hist[b]++;
}

static int probe_init(void)
{
	Tc *tc = TC4;

	MCLK->APBCMASK.reg |= MCLK_APBCMASK_TC4;
	GCLK->PCHCTRL[TC4_GCLK_ID].reg = GCLK_PCHCTRL_GEN_GCLK0 | GCLK_PCHCTRL_CHEN;
	while (!(GCLK->PCHCTRL[TC4_GCLK_ID].reg & GCLK_PCHCTRL_CHEN)) {
	}
	tc->COUNT16.CTRLA.reg = TC_CTRLA_SWRST;
	while (tc->COUNT16.SYNCBUSY.bit.SWRST) {
	}
	tc->COUNT16.CTRLA.reg = TC_CTRLA_MODE_COUNT16 | TC_CTRLA_PRESCALER_DIV16 | TC_CTRLA_PRESCSYNC_PRESC;
	tc->COUNT16.WAVE.reg = TC_WAVE_WAVEGEN_MFRQ;
	tc->COUNT16.CC[0].reg = PROBE_TOP;
	while (tc->COUNT16.SYNCBUSY.reg) {
	}
	tc->COUNT16.INTENSET.reg = TC_INTENSET_MC0;
	/* the EIC's priority (devicetree: 0), so it sees what the LAN8651's IRQ_N sees */
	IRQ_CONNECT(TC4_IRQn, 0, probe_isr, NULL, 0);
	irq_enable(TC4_IRQn);
	tc->COUNT16.CTRLA.reg |= TC_CTRLA_ENABLE;
	while (tc->COUNT16.SYNCBUSY.bit.ENABLE) {
	}
	return 0;
}
SYS_INIT(probe_init, APPLICATION, 1);

uint32_t zone_irq_lat_max_ns(void) { return (uint32_t)(lat_max * TICK_NS); }

static int cmd_irqlat(const struct shell *sh, size_t argc, char **argv)
{
	unsigned int key = irq_lock();
	uint32_t n = lat_n, mx = lat_max, h[6];
	uint64_t sum = lat_sum;

	memcpy(h, (const void *)lat_hist, sizeof(h));
	if (argc > 1 && !strcmp(argv[1], "reset")) {
		lat_n = 0;
		lat_sum = 0;
		lat_max = 0;
		memset((void *)lat_hist, 0, sizeof(lat_hist));
	}
	irq_unlock(key);
	shell_print(sh, "irqlat: %u samples (TC4 every 8 ms, prio 0), mean %u ns, max %u ns | <2us %u, <10us %u, <50us %u, "
		    "<500us %u, <2ms %u, >=2ms %u", n, n ? (uint32_t)(sum * TICK_NS / n) : 0, (uint32_t)(mx * TICK_NS),
		    h[0], h[1], h[2], h[3], h[4], h[5]);
	return 0;
}
SHELL_CMD_REGISTER(irqlat, NULL, "irqlat [reset]: interrupt latency seen by a periodic TC4 interrupt", cmd_irqlat);
#else
uint32_t zone_irq_lat_max_ns(void) { return 0; }
#endif /* CONFIG_APP_IRQ_PROBE */
