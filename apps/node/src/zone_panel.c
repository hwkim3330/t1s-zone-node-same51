/*
 * The zone node's face and hands: a MIKROE-4115 10x10 RGB click (WS2812-type, DI on slot 2's PWM pin) shows
 * this zone's half of the ESP zone controller's 20x10 car picture (zone_render.h: gear, speed, indicator sweep,
 * brake / reverse bar, pedals, wheel, proximity bar), and whichever input click sits in slot 3 is read at
 * 20 Hz and published:
 *   thumbstick (MCP3204, SPI ch 0/1)  vehicle/private/zone/stick/x|y            u16, 0..4095
 *   proximity  (VCNL4200, I2C 0x51)   vehicle/private/zone/proximity/raw        u16
 *                                     vehicle/private/external/proximity_rear   f32 m (zone_render's mapping)
 *
 * Which half: `panel half left|right` (saved; default: the PLCA id's parity, odd = left). The LEDs' order on
 * the click is not documented: `panel test` lights LED 0, 1, 2 ... in turn, `panel map <bits>` fixes it
 * (1 = serpentine rows, 2 = mirror x, 4 = mirror y, 8 = columns first; saved).
 * Brightness is capped (`panel bright 0.02..0.3`): 100 LEDs at full white would draw ~6 A from the base's 5 V.
 *
 * SPDX-License-Identifier: Apache-2.0 (zone_render.h / zone_glyphs.h: from elite-t1s-hat, MIT)
 */
#include <stdlib.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/settings/settings.h>
#include <zephyr/shell/shell.h>
#include <zephyr/logging/log.h>
#include <cmsis_core.h>
#include "zone.h"
#include "zone_render.h"
#include "t1s_diag.h"

LOG_MODULE_REGISTER(zone_panel, LOG_LEVEL_INF);

#define USER DT_PATH(zephyr_user)
#define HAS_PANEL DT_NODE_HAS_PROP(USER, panel_gpios)
#define HAS_STICK DT_NODE_EXISTS(DT_NODELABEL(thumbstick))

static const struct device *const i2c = DEVICE_DT_GET(DT_NODELABEL(mikrobus_i2c));
#define PROX_ADDR 0x51

static uint8_t map_bits = 1;        /* serpentine rows: the usual matrix wiring */
static float bright = 0.12f;
static int half = -1;               /* 0 left, 1 right, -1 = from the PLCA id */
static volatile int test_led = -1;  /* `panel test`: the LED index being shown */
static bool have_stick, have_prox;
static volatile uint16_t stick_x, stick_y, prox_raw;
static uint32_t frames;

static int panel_set(const char *key, size_t len, settings_read_cb read, void *arg)
{
	if (!strcmp(key, "map") && len == 1) {
		read(arg, &map_bits, 1);
	} else if (!strcmp(key, "bright") && len == sizeof(bright)) {
		read(arg, &bright, sizeof(bright));
	} else if (!strcmp(key, "half") && len == sizeof(half)) {
		read(arg, &half, sizeof(half));
	}
	return 0;
}
SETTINGS_STATIC_HANDLER_DEFINE(panel, "panel", NULL, panel_set, NULL, NULL);

/* ------------------------------------------------------------------ WS2812 out */
#if HAS_PANEL
#define PANEL_PORT DT_REG_ADDR(DT_GPIO_CTLR(USER, panel_gpios))
#define PANEL_PIN DT_GPIO_PIN(USER, panel_gpios)
static const struct gpio_dt_spec panel_pin = GPIO_DT_SPEC_GET(USER, panel_gpios);

/*
 * 800 kHz NRZ from the cycle counter at 120 MHz: bit period 150 cycles, a 0 is high for 48 (0.4 us), a 1 for
 * 96 (0.8 us); the WS2812 tolerance is +-150 ns, so a few cycles of loop jitter do not matter. Interrupts are
 * off for the whole frame (100 LEDs x 24 bits x 1.25 us = 3 ms); the LAN8651 IRQ waits that long at most.
 * SAM D5x PORT group: OUTCLR at +0x14, OUTSET at +0x18.
 */
static void ws2812_send(const uint8_t *grb, size_t n)
{
	volatile uint32_t *const clr = (volatile uint32_t *)(PANEL_PORT + 0x14);
	volatile uint32_t *const set = (volatile uint32_t *)(PANEL_PORT + 0x18);
	const uint32_t m = BIT(PANEL_PIN);

	CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
	DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
	unsigned int key = irq_lock();
	uint32_t t = DWT->CYCCNT;

	for (size_t i = 0; i < n; i++) {
		for (uint8_t b = 0x80; b; b >>= 1) {
			const uint32_t hi = (grb[i] & b) ? 96 : 48;

			while (DWT->CYCCNT - t < 150) {
			}
			t = DWT->CYCCNT;
			*set = m;
			while (DWT->CYCCNT - t < hi) {
			}
			*clr = m;
		}
	}
	irq_unlock(key);
	k_busy_wait(300); /* reset / latch: > 280 us low for the newer parts */
}
#endif

/* panel pixel (x, y) in 0..9 -> the LED's index on the chain, per `panel map` */
static int led_index(int x, int y)
{
	if (map_bits & 2) {
		x = 9 - x;
	}
	if (map_bits & 4) {
		y = 9 - y;
	}
	if (map_bits & 8) { /* columns first */
		int t = x;

		x = y;
		y = t;
	}
	if ((map_bits & 1) && (y & 1)) {
		x = 9 - x;
	}
	return y * 10 + x;
}

/* ------------------------------------------------------------------ input clicks */
#if HAS_STICK
static const struct spi_dt_spec stick = SPI_DT_SPEC_GET(DT_NODELABEL(thumbstick), SPI_WORD_SET(8) | SPI_TRANSFER_MSB, 0);

static int mcp3204(int ch)
{
	/* single-ended: start bit, SGL, D2 (0 for MCP3204), then D1 D0; 12 bits come back in rx[1]:rx[2] */
	uint8_t tx[3] = {0x06, (uint8_t)((ch & 3) << 6), 0}, rx[3] = {0};
	const struct spi_buf tb = {.buf = tx, .len = 3}, rb = {.buf = rx, .len = 3};
	const struct spi_buf_set ts = {.buffers = &tb, .count = 1}, rs = {.buffers = &rb, .count = 1};

	if (spi_transceive_dt(&stick, &ts, &rs)) {
		return -1;
	}
	return (rx[1] & 0x0f) << 8 | rx[2];
}
#endif

static int vcnl4200_init(void)
{
	/* as the LAN8661 kit's firmware sets it up: PS_CONF1/2 (16-bit, 200 mA LED), PS_CONF3, ALS on */
	static const uint8_t init[][3] = {{0x03, 0x02, 0x08}, {0x04, 0x01, 0x07}, {0x00, 0x00, 0x00}};

	for (size_t i = 0; i < ARRAY_SIZE(init); i++) {
		if (i2c_write(i2c, init[i], 3, PROX_ADDR)) {
			return -EIO;
		}
	}
	return 0;
}

static int vcnl4200_read(void)
{
	uint8_t reg = 0x08, v[2];

	if (i2c_write_read(i2c, PROX_ADDR, &reg, 1, v, 2)) {
		return -1;
	}
	return v[0] | v[1] << 8;
}

static void probe_inputs(void)
{
#if HAS_STICK
	if (spi_is_ready_dt(&stick)) {
		int x = mcp3204(0), y = mcp3204(1);

		/* nothing on the bus reads all ones or all zeros; a centred stick reads ~2048 */
		have_stick = x > 64 && x < 4031 && y > 64 && y < 4031;
	}
#endif
	have_prox = device_is_ready(i2c) && vcnl4200_init() == 0 && vcnl4200_read() >= 0;
	LOG_INF("slot 3 input: %s", have_stick ? "thumbstick (MCP3204)" : have_prox ? "proximity (VCNL4200)" : "none");
}

/* ------------------------------------------------------------------ the loop */
static void panel_thread(void *a, void *b, void *c)
{
	static zr_fb fb, out;
	static uint8_t grb[300];
	int64_t lit_l = 0, lit_r = 0;
	bool was_l = false, was_r = false;

	k_msleep(1500);
	probe_inputs();
#if HAS_PANEL
	gpio_pin_configure_dt(&panel_pin, GPIO_OUTPUT_INACTIVE);
#endif
	for (uint32_t tick = 0;; tick++) {
		int64_t now = k_uptime_get();

		/* inputs at 20 Hz */
#if HAS_STICK
		if (have_stick) {
			int x = mcp3204(0), y = mcp3204(1);

			if (x >= 0 && y >= 0) {
				stick_x = x;
				stick_y = y;
				zone_publish(ZONE_PUB_STICK_X, (const void *)&stick_x, 2);
				zone_publish(ZONE_PUB_STICK_Y, (const void *)&stick_y, 2);
			}
		}
#endif
		if (have_prox) {
			int r = vcnl4200_read();

			if (r >= 0) {
				float m = (float)zr_prox_m(r);

				prox_raw = r;
				zone_publish(ZONE_PUB_PROX_RAW, (const void *)&prox_raw, 2);
				zone_publish(ZONE_PUB_PROX_M, &m, 4);
			}
		}

		/* the picture */
		struct zone_vehicle v;
		bool live = zone_vehicle(&v);

		if (live) {
			zr_state s = {0};

			/* the sweep restarts each time a lamp lights (zone_render: one column per 25 ms) */
			bool l = v.left && v.lit, r = v.right && v.lit;

			if (l && !was_l) {
				lit_l = now;
			}
			if (r && !was_r) {
				lit_r = now;
			}
			was_l = l;
			was_r = r;
			s.lamps = (l ? ZR_LEFT : 0) | (r ? ZR_RIGHT : 0) | (v.brake ? ZR_BRAKE : 0) |
				  (v.reverse ? ZR_REVERSE : 0) | (v.hazard ? ZR_HAZARD : 0);
			s.gear = v.gear;
			s.speed = v.speed_kmh;
			s.mode = v.mode;
			s.prox_m = have_prox ? zr_prox_m(prox_raw) : (v.prox_m >= 0 ? v.prox_m : -1);
			s.wheel_deg = v.wheel_deg;
			s.accel = v.accel;
			s.brake = v.brake_pedal;
			zr_car(fb, &s, (now - lit_l) / 1000.0, (now - lit_r) / 1000.0);
		} else {
			zr_nolink(fb, tick, have_prox ? zr_prox_m(prox_raw) : -1);
		}
		zr_scale(out, fb, bright);

		int h = half >= 0 ? half : (zone_plca_id() & 1) ? 0 : 1;

		memset(grb, 0, sizeof(grb));
		for (int y = 0; y < 10; y++) {
			for (int x = 0; x < 10; x++) {
				int i = led_index(x, y) * 3;
				const uint8_t *p = out[y][x + 10 * h];

				if (test_led >= 0) { /* `panel test`: only the LED with that chain index, white */
					p = (i / 3 == test_led) ? (const uint8_t[]){40, 40, 40} : (const uint8_t[]){0, 0, 0};
				}
				grb[i] = p[1];
				grb[i + 1] = p[0];
				grb[i + 2] = p[2];
			}
		}
#if HAS_PANEL
		if (!t1s_is_sleeping()) {
			ws2812_send(grb, sizeof(grb));
		}
#endif
		frames++;
		k_msleep(50);
	}
}
K_THREAD_DEFINE(panel_tid, 2048, panel_thread, NULL, NULL, NULL, 8, 0, 0);

static int cmd_panel(const struct shell *sh, size_t argc, char **argv)
{
	if (argc > 2 && !strcmp(argv[1], "map")) {
		map_bits = strtoul(argv[2], NULL, 0) & 0xf;
		settings_save_one("panel/map", &map_bits, 1);
	} else if (argc > 2 && !strcmp(argv[1], "bright")) {
		bright = CLAMP(strtof(argv[2], NULL), 0.02f, 0.3f);
		settings_save_one("panel/bright", &bright, sizeof(bright));
	} else if (argc > 2 && !strcmp(argv[1], "half")) {
		half = !strcmp(argv[2], "left") ? 0 : !strcmp(argv[2], "right") ? 1 : -1;
		settings_save_one("panel/half", &half, sizeof(half));
	} else if (argc > 1 && !strcmp(argv[1], "test")) {
		/* light chain index 0, 1, 2 ... 99, 0.15 s each, so the wiring can be read off the board */
		for (int i = 0; i < 100; i++) {
			test_led = i;
			k_msleep(150);
		}
		test_led = -1;
	}
	shell_print(sh, "panel: %s, half %s, map %u (serpentine %u mirror-x %u mirror-y %u columns %u), bright %.2f, "
		    "%u frames | slot 3: %s stick %u/%u prox %u",
		    HAS_PANEL ? "10x10 on slot 2" : "none built in",
		    half == 0 ? "left" : half == 1 ? "right" : "auto (PLCA id odd = left)", map_bits, map_bits & 1,
		    (map_bits >> 1) & 1, (map_bits >> 2) & 1, (map_bits >> 3) & 1, (double)bright, frames,
		    have_stick ? "thumbstick" : have_prox ? "proximity" : "none", stick_x, stick_y, prox_raw);
	return 0;
}
SHELL_CMD_REGISTER(panel, NULL, "panel [map <bits>|bright <0.02..0.3>|half left|right|auto|test]", cmd_panel);
