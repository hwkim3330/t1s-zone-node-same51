/*
 * T1S zone node: SAM E51 Curiosity Nano + AC164162 + MIKROE-5543 (LAN8651).
 * Brings the LAN865x up with the in-tree driver, takes the MAC from the
 * click's AT24MAC402, applies PLCA from Kconfig (or the `t1s plca` shell
 * command), and prints PLCA/link status periodically.
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
#include <zephyr/logging/log.h>
#include "t1s_diag.h"

LOG_MODULE_REGISTER(zone_node, LOG_LEVEL_INF);

static const struct device *const t1s = DEVICE_DT_GET(DT_NODELABEL(t1s0));
static const struct device *const i2c = DEVICE_DT_GET(DT_NODELABEL(mikrobus_i2c));
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);

#define AT24MAC402_EUI_ADDR 0x58 /* A2..A0 = 0 on the click (found by apps/probe) */
#define AT24MAC402_EUI48_REG 0x9A

static struct net_if *iface;
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

static int apply_plca(void)
{
	int ret;

	net_if_down(iface);
	ret = net_mgmt(NET_REQUEST_ETHERNET_SET_T1S_PARAM, iface, &plca_cfg,
		       sizeof(plca_cfg));
	net_if_up(iface);
	return ret;
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

	shell_print(sh, "PLCA %s id %u count %u to %u -> %d (not saved: reboot restores Kconfig)",
		    plca_cfg.t1s_param.plca.enable ? "on" : "off (CSMA/CD)",
		    plca_cfg.t1s_param.plca.node_id, plca_cfg.t1s_param.plca.node_count,
		    plca_cfg.t1s_param.plca.to_timer, ret);
	k_msleep(50);
	print_status(sh);
	return ret;
}

SHELL_STATIC_SUBCMD_SET_CREATE(t1s_cmds,
	SHELL_CMD(status, NULL, "LAN865x PLCA / link / counters", cmd_status),
	SHELL_CMD(plca, NULL, "t1s plca <id|csma> [count] [to_timer]", cmd_plca),
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

	set_mac_from_eeprom();
	int ret = apply_plca();

	LOG_INF("PLCA node %u/%u applied (%d)", CONFIG_APP_PLCA_NODE_ID,
		CONFIG_APP_PLCA_NODE_COUNT, ret);
	k_msleep(100);

	int64_t next = 0;

	while (1) {
		struct t1s_status st;
		bool pst = (t1s_read_status(t1s, &st) == 0) && (st.plca_sts & BIT(15));

		/* LED0: solid = PLCA active (beacons), slow blink = waiting for a coordinator */
		if (pst) {
			gpio_pin_set_dt(&led, 1);
		} else {
			gpio_pin_toggle_dt(&led);
		}
		if (CONFIG_APP_STATUS_PERIOD_S && k_uptime_get() >= next) {
			print_status(NULL);
			next = k_uptime_get() + CONFIG_APP_STATUS_PERIOD_S * 1000;
		}
		k_msleep(500);
	}
	return 0;
}
