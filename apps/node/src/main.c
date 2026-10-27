/*
 * T1S zone node: SAM E51 Curiosity Nano + AC164162 + MIKROE-5543 (LAN8651).
 * Brings the LAN865x up with the in-tree driver, takes the MAC from the
 * click's AT24MAC402, picks its PLCA id / count / IP (saved settings, else the
 * bench table below keyed by that MAC, else Kconfig), and prints PLCA/link
 * status periodically. `t1s plca`, `t1s ip` and `t1s save` change and keep them.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdlib.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/ethernet.h>
#include <zephyr/net/ethernet_mgmt.h>
#include <zephyr/net/net_stats.h>
#include <zephyr/shell/shell.h>
#include <zephyr/settings/settings.h>
#include <zephyr/dfu/mcuboot.h>
#include <zephyr/logging/log.h>
#include "t1s_diag.h"
#include "zone.h"

LOG_MODULE_REGISTER(zone_node, LOG_LEVEL_INF);

static const struct device *const t1s = DEVICE_DT_GET(DT_NODELABEL(t1s0));
static const struct device *const i2c = DEVICE_DT_GET(DT_NODELABEL(mikrobus_i2c));
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);

#define AT24MAC402_EUI_ADDR 0x58 /* A2..A0 = 0 on the click (found by apps/probe) */
#define AT24MAC402_EUI48_REG 0x9A

static struct net_if *iface;
static uint8_t mac[6];

/* Where this node's identity comes from, for the status line. */
static const char *id_src = "kconfig";

/*
 * Bench boards, keyed by the click's EUI-48, so one image serves every node and a
 * reboot never brings a node back as somebody else (or as a second coordinator).
 * Saved settings (`t1s save`) win over this table. The ESP T1S HAT is coordinator 0.
 */
static const struct {
	uint8_t mac[6];
	uint8_t id;
	uint8_t ip4; /* 192.168.100.<ip4> */
} bench[] = {
	{{0xfc, 0xc2, 0x3d, 0x39, 0xe9, 0xb8}, 3, 73},
	{{0xfc, 0xc2, 0x3d, 0x3a, 0x0f, 0x32}, 4, 74},
};

static struct in_addr my_ip;
static volatile bool bus_up;
volatile int zone_led_mode = ZONE_LED_AUTO;

static struct ethernet_req_params plca_cfg = {
	.t1s_param = {
		.type = ETHERNET_T1S_PARAM_TYPE_PLCA_CONFIG,
		.plca = {
			.enable = true,
			.node_id = CONFIG_APP_PLCA_NODE_ID,
			.node_count = CONFIG_APP_PLCA_NODE_COUNT,
			.burst_count = CONFIG_APP_PLCA_BURST_COUNT,
			.burst_timer = CONFIG_APP_PLCA_BURST_TIMER,
			.to_timer = CONFIG_APP_PLCA_TO_TIMER,
		},
	},
};

bool zone_bus_up(void) { return bus_up; }
uint8_t zone_plca_id(void) { return plca_cfg.t1s_param.plca.node_id; }
void zone_ip_str(char *buf, size_t len) { net_addr_ntop(AF_INET, &my_ip, buf, len); }

static int apply_plca(void)
{
	int ret;

	net_if_down(iface);
	ret = net_mgmt(NET_REQUEST_ETHERNET_SET_T1S_PARAM, iface, &plca_cfg,
		       sizeof(plca_cfg));
	net_if_up(iface);
	return ret;
}

static int zone_set(const char *key, size_t len, settings_read_cb read, void *arg)
{
	uint32_t v;

	if (len != sizeof(v) || read(arg, &v, sizeof(v)) != sizeof(v)) {
		return -EINVAL;
	}
	if (!strcmp(key, "id")) {
		plca_cfg.t1s_param.plca.node_id = v & 0xff;
		plca_cfg.t1s_param.plca.enable = v != 0xffff;
		id_src = "saved";
	} else if (!strcmp(key, "count")) {
		plca_cfg.t1s_param.plca.node_count = v;
	} else if (!strcmp(key, "ip")) {
		my_ip.s_addr = v;
	}
	return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(zone, "zone", NULL, zone_set, NULL, NULL);

static void apply_ip(struct in_addr ip)
{
	struct net_if_ipv4 *v4 = iface->config.ip.ipv4;

	if (v4 && v4->unicast[0].ipv4.is_used) {
		net_if_ipv4_addr_rm(iface, &v4->unicast[0].ipv4.address.in_addr);
	}
	net_if_ipv4_addr_add(iface, &ip, NET_ADDR_MANUAL, 0);
	struct in_addr mask = {.s4_addr = {255, 255, 255, 0}};

	net_if_ipv4_set_netmask_by_addr(iface, &ip, &mask);
}

/* Saved settings first, then the bench table, then Kconfig. */
static void pick_identity(void)
{
	bool saved_ip = false;

	net_addr_pton(AF_INET, CONFIG_NET_CONFIG_MY_IPV4_ADDR, &my_ip);
	struct in_addr kc = my_ip;

	settings_subsys_init();
	settings_load(); /* "zone/..." here, "zenoh/..." in zone_zenoh.c */
	saved_ip = my_ip.s_addr != kc.s_addr;

	if (strcmp(id_src, "saved")) {
		for (size_t i = 0; i < ARRAY_SIZE(bench); i++) {
			if (!memcmp(bench[i].mac, mac, 6)) {
				plca_cfg.t1s_param.plca.node_id = bench[i].id;
				if (!saved_ip) {
					my_ip = (struct in_addr){.s4_addr = {192, 168, 100, bench[i].ip4}};
				}
				id_src = "mac table";
			}
		}
	}
	apply_ip(my_ip);
	LOG_INF("identity (%s): PLCA %u/%u, ip %d.%d.%d.%d", id_src,
		plca_cfg.t1s_param.plca.node_id, plca_cfg.t1s_param.plca.node_count,
		my_ip.s4_addr[0], my_ip.s4_addr[1], my_ip.s4_addr[2], my_ip.s4_addr[3]);
}

static void set_mac_from_eeprom(void)
{
	uint8_t reg = AT24MAC402_EUI48_REG;
	struct ethernet_req_params p = {0};

	if (!device_is_ready(i2c) ||
	    i2c_write_read(i2c, AT24MAC402_EUI_ADDR, &reg, 1, p.mac_address.addr, 6) != 0) {
		LOG_WRN("AT24MAC402 not readable, keeping devicetree MAC");
		return;
	}
	memcpy(mac, p.mac_address.addr, 6);
	net_if_down(iface);
	int ret = net_mgmt(NET_REQUEST_ETHERNET_SET_MAC_ADDRESS, iface, &p, sizeof(p));
	net_if_up(iface);
	LOG_INF("MAC from AT24MAC402: %02x:%02x:%02x:%02x:%02x:%02x (%d)",
		p.mac_address.addr[0], p.mac_address.addr[1], p.mac_address.addr[2],
		p.mac_address.addr[3], p.mac_address.addr[4], p.mac_address.addr[5], ret);
}

static void print_status(const struct shell *sh)
{
	struct t1s_status st;
	struct net_stats ns = {0};
	int ret = t1s_read_status(t1s, &st);

#define OUT(...) do { if (sh) shell_print(sh, __VA_ARGS__); else printk(__VA_ARGS__), printk("\n"); } while (0)
	if (ret) {
		OUT("t1s: register read failed (%d)", ret);
		return;
	}
	bool en = st.plca_ctrl0 & BIT(15);
	bool pst = st.plca_sts & BIT(15);
	uint8_t id = st.plca_ctrl1 & 0xff, ncnt = (st.plca_ctrl1 >> 8) & 0xff;
	char ip[NET_IPV4_ADDR_LEN] = "-";
	struct net_if_ipv4 *v4 = iface->config.ip.ipv4;

	if (v4) {
		net_addr_ntop(AF_INET, &v4->unicast[0].ipv4.address.in_addr, ip, sizeof(ip));
	}
	/* The LAN865x driver keeps no packet counters; these come from the IP stack. */
	net_mgmt(NET_REQUEST_STATS_GET_ALL, iface, &ns, sizeof(ns));
	OUT("t1s: chip %04x rev %u | PLCA %s id %u count %u to %u burst %u/%u | "
	    "PST=%u (%s) | BMSR=%04x link=%u | STATUS0=%08x",
	    (unsigned)((st.devid >> 4) & 0xffff), (unsigned)(st.devid & 0xf),
	    en ? "on" : "off", id, ncnt, (unsigned)(st.plca_totmr & 0xff),
	    (unsigned)((st.plca_burst >> 8) & 0xff), (unsigned)(st.plca_burst & 0xff),
	    pst, pst ? "beacons: bus is running" : "no beacon: coordinator absent or no peer",
	    (unsigned)(st.phy_bmsr & 0xffff), (unsigned)((st.phy_bmsr >> 2) & 1),
	    st.oa_status0);
	OUT("net: %s up=%d carrier=%d | bytes rx %u tx %u | ipv4 rx %u tx %u drop %u | icmp rx %u tx %u",
	    ip, net_if_is_up(iface), net_if_is_carrier_ok(iface),
	    ns.bytes.received, ns.bytes.sent, ns.ipv4.recv, ns.ipv4.sent, ns.ipv4.drop,
	    ns.icmp.recv, ns.icmp.sent);
#undef OUT
}

static int cmd_status(const struct shell *sh, size_t argc, char **argv)
{
	print_status(sh);
	return 0;
}

static int cmd_plca(const struct shell *sh, size_t argc, char **argv)
{
	if (argc < 2) {
		shell_print(sh, "usage: t1s plca <id|csma> [count] [to_timer]");
		return -EINVAL;
	}
	if (!strcmp(argv[1], "csma")) {
		plca_cfg.t1s_param.plca.enable = false;
	} else {
		plca_cfg.t1s_param.plca.enable = true;
		plca_cfg.t1s_param.plca.node_id = strtoul(argv[1], NULL, 0);
	}
	if (argc > 2) {
		plca_cfg.t1s_param.plca.node_count = strtoul(argv[2], NULL, 0);
	}
	if (argc > 3) {
		plca_cfg.t1s_param.plca.to_timer = strtoul(argv[3], NULL, 0);
	}
	int ret = apply_plca();

	shell_print(sh, "PLCA %s id %u count %u to %u -> %d (`t1s save` keeps it across reboots)",
		    plca_cfg.t1s_param.plca.enable ? "on" : "off (CSMA/CD)",
		    plca_cfg.t1s_param.plca.node_id, plca_cfg.t1s_param.plca.node_count,
		    plca_cfg.t1s_param.plca.to_timer, ret);
	k_msleep(50);
	print_status(sh);
	return ret;
}

static int cmd_ip(const struct shell *sh, size_t argc, char **argv)
{
	struct in_addr ip;

	if (argc < 2 || net_addr_pton(AF_INET, argv[1], &ip)) {
		shell_print(sh, "usage: t1s ip <a.b.c.d>   (/24; `t1s save` keeps it)");
		return -EINVAL;
	}
	my_ip = ip;
	apply_ip(ip);
	shell_print(sh, "ip %s", argv[1]);
	return 0;
}

static int cmd_save(const struct shell *sh, size_t argc, char **argv)
{
	const __typeof__(plca_cfg.t1s_param.plca) *p = &plca_cfg.t1s_param.plca;
	uint32_t id = p->enable ? p->node_id : 0xffff, cnt = p->node_count, ip = my_ip.s_addr;
	int ret = settings_save_one("zone/id", &id, sizeof(id)) ||
		  settings_save_one("zone/count", &cnt, sizeof(cnt)) ||
		  settings_save_one("zone/ip", &ip, sizeof(ip));

	shell_print(sh, "saved PLCA %s %u/%u ip %d.%d.%d.%d -> %s", p->enable ? "id" : "csma",
		    p->node_id, p->node_count, my_ip.s4_addr[0], my_ip.s4_addr[1],
		    my_ip.s4_addr[2], my_ip.s4_addr[3], ret ? "FAILED" : "ok");
	return ret ? -EIO : 0;
}

static int cmd_forget(const struct shell *sh, size_t argc, char **argv)
{
	settings_delete("zone/id");
	settings_delete("zone/count");
	settings_delete("zone/ip");
	shell_print(sh, "saved identity erased: next boot uses the MAC table / Kconfig");
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(t1s_cmds,
	SHELL_CMD(status, NULL, "LAN865x PLCA / link / counters", cmd_status),
	SHELL_CMD(plca, NULL, "t1s plca <id|csma> [count] [to_timer]", cmd_plca),
	SHELL_CMD(ip, NULL, "t1s ip <a.b.c.d>", cmd_ip),
	SHELL_CMD(save, NULL, "keep PLCA id/count and IP across reboots", cmd_save),
	SHELL_CMD(forget, NULL, "erase the saved identity", cmd_forget),
	SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(t1s, &t1s_cmds, "10BASE-T1S node", NULL);

int main(void)
{
	gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);

	if (!device_is_ready(t1s)) {
		LOG_ERR("LAN865x not ready (driver probe failed: check slot / power)");
		while (1) {
			gpio_pin_toggle_dt(&led);
			k_msleep(100);
		}
	}
	iface = net_if_lookup_by_dev(t1s);
	LOG_INF("LAN865x bound: %s, iface %d", t1s->name, net_if_get_by_iface(iface));

#ifdef CONFIG_BOOTLOADER_MCUBOOT
	/* Got this far with the network driver up: keep this image. An update that never
	 * reaches here is swapped back out by MCUboot on the next reset. */
	if (!boot_is_img_confirmed()) {
		LOG_INF("new image confirmed (%d)", boot_write_img_confirmed());
	}
#endif
	set_mac_from_eeprom();
	pick_identity();
	int ret = apply_plca();

	LOG_INF("PLCA node %u/%u applied (%d)", plca_cfg.t1s_param.plca.node_id,
		plca_cfg.t1s_param.plca.node_count, ret);
	k_msleep(100);

	int64_t next = 0;
	bool was_pst = true; /* so a node that boots with no coordinator counts as "lost" */

	while (1) {
		struct t1s_status st;
		bool pst = (t1s_read_status(t1s, &st) == 0) && (st.plca_sts & BIT(15));

		/*
		 * A follower that ran without beacons has frames stuck in the MAC-PHY and its
		 * TX never restarts by itself when a coordinator appears (seen on the bench,
		 * 2026-10-27: tx bytes frozen until PLCA was re-applied). Re-apply on the edge.
		 */
		if (pst && !was_pst && plca_cfg.t1s_param.plca.node_id != 0) {
			LOG_INF("beacons back: re-applying PLCA (%d)", apply_plca());
		}
		was_pst = pst;

		bus_up = pst;
		/* LED0 (AUTO): solid = PLCA active (beacons), slow blink = waiting for a coordinator */
		switch (zone_led_mode) {
		case ZONE_LED_ON:
			gpio_pin_set_dt(&led, 1);
			break;
		case ZONE_LED_OFF:
			gpio_pin_set_dt(&led, 0);
			break;
		case ZONE_LED_BLINK:
			gpio_pin_toggle_dt(&led);
			break;
		default:
			if (pst) {
				gpio_pin_set_dt(&led, 1);
			} else {
				gpio_pin_toggle_dt(&led);
			}
		}
		if (CONFIG_APP_STATUS_PERIOD_S && k_uptime_get() >= next) {
			print_status(NULL);
			next = k_uptime_get() + CONFIG_APP_STATUS_PERIOD_S * 1000;
		}
		k_msleep(500);
	}
	return 0;
}
