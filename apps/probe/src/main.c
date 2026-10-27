/*
 * mikroBUS slot probe for the AC164162 base: finds the LAN865x
 * (Two-Wire ETH Click, MIKROE-5543) by reading its OPEN Alliance TC6
 * registers over the shared SPI with each slot's CS/RST in turn.
 * Also scans the shared I2C bus (the click carries an AT24MAC402).
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/printk.h>
#include <soc.h>

#define SPI_NODE DT_NODELABEL(sercom1)
#define I2C_NODE DT_NODELABEL(sercom2)


/* RST/INT of each slot come from the board mikroBUS nexus nodes, listed in
 * /zephyr,user by app.overlay. */
#define PROBE DT_PATH(zephyr_user)

struct slot {
	int num;
	struct gpio_dt_spec rst, intr;
	int cs_idx;
};

static const struct slot slots[] = {
	{ 1, GPIO_DT_SPEC_GET_BY_IDX(PROBE, rst_gpios, 0), GPIO_DT_SPEC_GET_BY_IDX(PROBE, int_gpios, 0), 0 },
	{ 2, GPIO_DT_SPEC_GET_BY_IDX(PROBE, rst_gpios, 1), GPIO_DT_SPEC_GET_BY_IDX(PROBE, int_gpios, 1), 1 },
	{ 3, GPIO_DT_SPEC_GET_BY_IDX(PROBE, rst_gpios, 2), GPIO_DT_SPEC_GET_BY_IDX(PROBE, int_gpios, 2), 2 },
};

static const struct device *spi = DEVICE_DT_GET(SPI_NODE);
static const struct device *i2c = DEVICE_DT_GET(I2C_NODE);

static const struct gpio_dt_spec cs[] = {
	GPIO_DT_SPEC_GET_BY_IDX(SPI_NODE, cs_gpios, 0),
	GPIO_DT_SPEC_GET_BY_IDX(SPI_NODE, cs_gpios, 1),
	GPIO_DT_SPEC_GET_BY_IDX(SPI_NODE, cs_gpios, 2),
};

static uint32_t parity_bit(uint32_t x)
{
	/* OA TC6: header carries odd parity -> P=1 when the rest has an even count */
	return (__builtin_popcount(x) & 1) ? 0 : 1;
}

static int tc6_read(int cs_idx, uint32_t reg, uint32_t *val, uint32_t *echo)
{
	uint8_t tx[12] = {0}, rx[12] = {0};
	uint32_t hdr = ((reg >> 16) & 0xF) << 24 | (reg & 0xFFFF) << 8;

	hdr |= parity_bit(hdr);
	sys_put_be32(hdr, tx);

	struct spi_cs_control csc = { .gpio = cs[cs_idx], .delay = 1 };
	struct spi_config cfg = {
		.frequency = 4000000,
		.operation = SPI_OP_MODE_MASTER | SPI_WORD_SET(8) | SPI_TRANSFER_MSB,
		.cs = csc,
	};
	struct spi_buf tb = { .buf = tx, .len = sizeof(tx) };
	struct spi_buf rb = { .buf = rx, .len = sizeof(rx) };
	struct spi_buf_set ts = { .buffers = &tb, .count = 1 };
	struct spi_buf_set rs = { .buffers = &rb, .count = 1 };
	int ret = spi_transceive(spi, &cfg, &ts, &rs);

	if (ret) {
		return ret;
	}
	*echo = sys_get_be32(&rx[4]);
	*val = sys_get_be32(&rx[8]);
	return (*echo == hdr) ? 0 : -EIO;
}

static int probe_slot(const struct slot *s)
{
	uint32_t id = 0, phy = 0, dev = 0, st0 = 0, echo = 0;
	int r1, r2, r3, r4;

	gpio_pin_configure_dt(&s->rst, GPIO_OUTPUT_INACTIVE);
	gpio_pin_configure_dt(&s->intr, GPIO_INPUT);
	/* RST_N low >= 5 us, then wait for IRQ_N (reset complete) */
	gpio_pin_set_dt(&s->rst, 1);
	k_busy_wait(20);
	gpio_pin_set_dt(&s->rst, 0);
	int irq_seen = 0;
	for (int i = 0; i < 100; i++) {
		if (gpio_pin_get_dt(&s->intr) == 1) { /* active-low INT asserted */
			irq_seen = 1;
			break;
		}
		k_msleep(1);
	}

	r1 = tc6_read(s->cs_idx, 0x00000000, &id, &echo);   /* OA_ID */
	r2 = tc6_read(s->cs_idx, 0x00000001, &phy, &echo);  /* OA_PHYID */
	r3 = tc6_read(s->cs_idx, 0x000A0094, &dev, &echo);  /* LAN865x DEVID */
	r4 = tc6_read(s->cs_idx, 0x00000008, &st0, &echo);  /* OA_STATUS0 */

	printk("slot %d: IRQ_N after reset=%s OA_ID=0x%08x(%d) PHYID=0x%08x(%d) "
	       "DEVID=0x%08x(%d) STATUS0=0x%08x(%d)\n",
	       s->num, irq_seen ? "low" : "high", id, r1, phy, r2, dev, r3, st0, r4);

	if (r1 == 0 && id == 0x11 && r3 == 0) {
		uint16_t chip = (dev >> 4) & 0xFFFF;
		printk("slot %d: LAN865x found: chip 0x%04x rev %u (%s)\n", s->num, chip,
		       dev & 0xF, (dev & 0xF) == 2 ? "B1" : ((dev & 0xF) == 1 ? "B0" : "?"));
		return 1;
	}
	return 0;
}

static void i2c_scan(void)
{
	static bool done;
	if (done) {
		return;
	}
	done = true;
	/* Line levels first: a bus without pull-ups (or held low) hangs the SERCOM */
	PORT->Group[0].PINCFG[12].bit.INEN = 1;
	PORT->Group[0].PINCFG[13].bit.INEN = 1;
	k_busy_wait(10);
	uint32_t in = PORT->Group[0].IN.reg;
	printk("I2C lines: SDA(PA12)=%d SCL(PA13)=%d\n", (int)((in >> 12) & 1),
	       (int)((in >> 13) & 1));
	if (!((in >> 12) & 1) || !((in >> 13) & 1)) {
		printk("I2C: a line is low (no pull-up?) - scan skipped\n");
		return;
	}
	if (!device_is_ready(i2c)) {
		printk("I2C: controller not ready\n");
		return;
	}
	printk("I2C (shared mikroBUS SCL/SDA) scan:");
	for (uint8_t a = 0x08; a < 0x78; a++) {
		uint8_t b;
		if (i2c_read(i2c, &b, 1, a) == 0) {
			printk(" 0x%02x", a);
		}
	}
	printk("\n");
	/* AT24MAC402: EUI-48 at word address 0x9A of the 0x58|A2..A0 device */
	for (uint8_t a = 0x58; a <= 0x5F; a++) {
		uint8_t reg = 0x9A, mac[6];
		if (i2c_write_read(i2c, a, &reg, 1, mac, 6) == 0) {
			printk("AT24MAC402 @0x%02x EUI-48 %02x:%02x:%02x:%02x:%02x:%02x\n", a,
			       mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
		}
	}
}

K_THREAD_DEFINE(i2c_tid, 1024, i2c_scan, NULL, NULL, NULL, 10, 0, 3000);

int main(void)
{
	int round = 0;

	k_msleep(500);
	while (1) {
		int found = 0;
		printk("\n=== mikroBUS slot probe, round %d ===\n", round++);
		for (size_t i = 0; i < ARRAY_SIZE(slots); i++) {
			if (probe_slot(&slots[i])) {
				printk("RESULT: Two-Wire ETH click (LAN865x) in mikroBUS slot %d\n",
				       slots[i].num);
				found++;
			}
		}
		if (!found) {
			printk("RESULT: no LAN865x answered on any slot\n");
		}
		k_msleep(5000);
	}
	return 0;
}
