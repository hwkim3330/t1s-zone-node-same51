/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdint.h>
#include <zephyr/device.h>

struct t1s_status {
	uint32_t devid;      /* 0x0A0094: chip id << 4 | silicon rev */
	uint32_t plca_idver; /* 0x04CA00 */
	uint32_t plca_ctrl0; /* 0x04CA01: bit15 EN */
	uint32_t plca_ctrl1; /* 0x04CA02: NCNT << 8 | ID */
	uint32_t plca_sts;   /* 0x04CA03: bit15 PST (PLCA active: beacons seen/sent) */
	uint32_t plca_totmr; /* 0x04CA04 */
	uint32_t plca_burst; /* 0x04CA05 */
	uint32_t oa_status0; /* 0x000008 */
	uint32_t phy_bmsr;   /* 0x00FF01: clause-22 BMSR, bit2 link */
};

/* Reads LAN865x registers via the in-tree driver's OA TC6 helpers,
 * serialised with the driver's own SPI traffic. */
int t1s_read_status(const struct device *dev, struct t1s_status *st);
