/* SPDX-License-Identifier: Apache-2.0 */
#include "t1s_diag.h"
#include <zephyr/kernel.h>
#include "eth_lan865x_priv.h"
#include "oa_tc6.h"

static int rd(struct lan865x_data *ctx, uint32_t reg, uint32_t *v)
{
	*v = 0;
	return oa_tc6_reg_read(ctx->tc6, reg, v);
}

int t1s_read_status(const struct device *dev, struct t1s_status *st)
{
	struct lan865x_data *ctx = dev->data;
	int err = 0;

	if (k_sem_take(&ctx->tx_rx_sem, K_MSEC(200)) != 0) {
		return -EBUSY;
	}
	err |= rd(ctx, 0x000A0094, &st->devid);
	err |= rd(ctx, 0x0004CA00, &st->plca_idver);
	err |= rd(ctx, 0x0004CA01, &st->plca_ctrl0);
	err |= rd(ctx, 0x0004CA02, &st->plca_ctrl1);
	err |= rd(ctx, 0x0004CA03, &st->plca_sts);
	err |= rd(ctx, 0x0004CA04, &st->plca_totmr);
	err |= rd(ctx, 0x0004CA05, &st->plca_burst);
	err |= rd(ctx, 0x00000008, &st->oa_status0);
	err |= rd(ctx, 0x0000FF01, &st->phy_bmsr);
	k_sem_give(&ctx->tx_rx_sem);
	return err ? -EIO : 0;
}

int t1s_reg_read(const struct device *dev, uint32_t reg, uint32_t *val)
{
	struct lan865x_data *ctx = dev->data;

	if (k_sem_take(&ctx->tx_rx_sem, K_MSEC(200)) != 0) {
		return -EBUSY;
	}
	int ret = rd(ctx, reg, val);

	k_sem_give(&ctx->tx_rx_sem);
	return ret;
}

int t1s_reg_write(const struct device *dev, uint32_t reg, uint32_t val)
{
	struct lan865x_data *ctx = dev->data;

	if (k_sem_take(&ctx->tx_rx_sem, K_MSEC(200)) != 0) {
		return -EBUSY;
	}
	int ret = oa_tc6_reg_write(ctx->tc6, reg, val);

	k_sem_give(&ctx->tx_rx_sem);
	return ret;
}

/*
 * STATUS1 TTSCOF{A,B,C} (21..23) and TTSCM{A,B,C} (24..26): a capture that overflowed (a new one
 * while TTSCAA was still set, e.g. a frame that left after the reader gave up) blocks every later
 * capture until cleared, and IMASK1 masks them so the driver never sees them. Write-1-to-clear.
 */
#define STATUS1_TTSC_ERR (0x3fu << 21)

static void clear_capture(const struct device *dev)
{
	t1s_reg_write(dev, 0x00000009, STATUS1_TTSC_ERR);
	t1s_reg_write(dev, 0x00000008, BIT(8));
}

int t1s_take_tx_stamp(const struct device *dev, uint64_t *ts, int timeout_ms)
{
	struct lan865x_data *ctx = dev->data;
	struct oa_tc6 *tc6 = ctx->tc6;
	int ret = -EAGAIN;

	for (int i = 0; i < timeout_ms * 10; i++) {
		if (tc6->tx_ts_valid) { /* the driver's status check got there first */
			*ts = tc6->tx_ts;
			tc6->tx_ts_valid = false;
			ret = 0;
			break;
		}
		uint32_t st = 0, hi = 0, lo = 0;

		if (t1s_reg_read(dev, 0x00000008, &st) == 0 && (st & BIT(8))) { /* STATUS0.TTSCAA */
			t1s_reg_read(dev, 0x00000010, &hi);
			t1s_reg_read(dev, 0x00000011, &lo);
			*ts = (uint64_t)hi << 32 | lo;
			ret = 0;
			break;
		}
		k_busy_wait(100);
	}
	clear_capture(dev); /* never leave a stamp or an overflow behind for the next request */
	return ret;
}
