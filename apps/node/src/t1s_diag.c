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
