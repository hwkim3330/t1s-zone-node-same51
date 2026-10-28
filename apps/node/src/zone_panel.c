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
#include "zone_lampgfx.h"
#include "t1s_diag.h"

LOG_MODULE_REGISTER(zone_panel, LOG_LEVEL_INF);

#define USER DT_PATH(zephyr_user)
#define HAS_PANEL DT_NODE_HAS_PROP(USER, panel_gpios)
#define HAS_STICK DT_NODE_EXISTS(DT_NODELABEL(thumbstick))

static const struct device *const i2c = DEVICE_DT_GET(DT_NODELABEL(mikrobus_i2c));
#define PROX_ADDR 0x51

static uint8_t map_bits = 0;        /* row-major, no zigzag: MIKROE-4115 per its library (index = 10 * y + x) */
static float bright = 0.03f;       /* start dim; `panel bright` raises it (saved) */
/* Hard current budget per frame: the sum of every channel value sent (0..76500 for 100 white LEDs at 255).
 * ~12 mA per channel at 255 on 5050 WS2812s -> 1500 = ~70 mA for the whole panel. A brighter frame is scaled
 * down whatever asked for it (2026-10-28: a panel image froze both boards, supply suspected). */
#define PANEL_BUDGET 1500
static uint32_t budget_hits;
static int half = -1;               /* 0 left, 1 right, -1 = from the PLCA id */
static volatile int test_led = -1;  /* `panel test`: the LED index being shown */
static volatile bool pattern;       /* `panel pattern on`: the orientation picture instead of the car */
static bool have_stick, have_prox;
static volatile uint16_t stick_x, stick_y, prox_raw;
static uint32_t frames;
static uint8_t lamp_mode = 1;      /* 1 = the zone's lamps (zone_lampgfx.h), 0 = the cluster picture */
static uint8_t beam_on;
static uint8_t view;               /* the stick node's panel view for every zone (left flick toggles it) */            /* the stick node's high beam (right flick toggles it) */

static int panel_set(const char *key, size_t len, settings_read_cb read, void *arg)
{
	if (!strcmp(key, "map") && len == 1) {
		read(arg, &map_bits, 1);
	} else if (!strcmp(key, "bright") && len == sizeof(bright)) {
		read(arg, &bright, sizeof(bright));
	} else if (!strcmp(key, "mode") && len == 1) {
		read(arg, &lamp_mode, 1);
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

/* the cycle counter must be running or the loops above never end with interrupts off */
static bool cyccnt_runs(void)
{
	CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
	DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
	uint32_t a = DWT->CYCCNT;

	k_busy_wait(2);
	return DWT->CYCCNT != a;
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
	int64_t lit_l = 0, lit_r = 0, live_at = -10000, gone_at = -10000;
	bool was_l = false, was_r = false, was_live = false;
	float last_speed = 0;
	int64_t last_speed_at = 0, ess_until = 0;

	k_msleep(1500);
	probe_inputs();
#if HAS_PANEL
	gpio_pin_configure_dt(&panel_pin, GPIO_OUTPUT_INACTIVE);
	/* nothing on the LEDs until 10 s after boot, then one all-dark frame first: whatever the LEDs latched
	 * before (a crash, a power cut) is cleared, and a board that dies when it drives the panel can still
	 * be reflashed in those 10 s */
	k_msleep(8500);
	memset(grb, 0, sizeof(grb));
	LOG_INF("panel: first frame (dark) ... cycle counter %s", cyccnt_runs() ? "running" : "STOPPED");
	if (cyccnt_runs()) {
		ws2812_send(grb, sizeof(grb));
	}
	LOG_INF("panel: dark frame sent");
#endif
	for (uint32_t tick = 0;; tick++) {
		int64_t now = k_uptime_get();

		/* inputs at 20 Hz */
#if HAS_STICK
		if (have_stick) {
			int x = mcp3204(0), y = mcp3204(1);

			if (x >= 0 && y >= 0) {
				/* a flick right (past 90 % and back to the centre) toggles the high beam */
				static bool armed = true;

				if (x > 1000 && x < 3100) {
					armed = true;
				} else if (armed && x >= 3700) {
					armed = false;
					beam_on = !beam_on;
					LOG_INF("stick flick right: high beam %s", beam_on ? "on" : "off");
				} else if (armed && x <= 400) { /* a flick left: every zone between the lamps and the cluster */
					armed = false;
					view = !view;
					LOG_INF("stick flick left: panels show %s", view ? "the cluster" : "the zone lamps");
				}
				stick_x = x;
				stick_y = y;
				zone_publish(ZONE_PUB_STICK_X, (const void *)&stick_x, 2);
				zone_publish(ZONE_PUB_STICK_Y, (const void *)&stick_y, 2);
				zone_publish(ZONE_PUB_BEAM, &beam_on, 1); /* 20 Hz like the stick: a late joiner sees it at once */
				zone_publish(ZONE_PUB_VIEW, &view, 1);
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
		int h = half >= 0 ? half : (zone_plca_id() & 1) ? 0 : 1;
		bool l = live && v.left && v.lit, r = live && v.right && v.lit;

		/* the sweep restarts each time a lamp lights (one column per 25 ms) */
		if (l && !was_l) {
			lit_l = now;
		}
		if (r && !was_r) {
			lit_r = now;
		}
		was_l = l;
		was_r = r;
		double prox = have_prox ? zr_prox_m(prox_raw) : (v.prox_m >= 0 ? v.prox_m : -1);

		if (live && !was_live) {
			live_at = now;
		} else if (!live && was_live) {
			gone_at = now;
		}
		was_live = live;
		/* emergency stop signal: braking and losing more than 6 m/s^2 (21.6 km/h per s), held 1 s */
		if (live && now - last_speed_at >= 200) {
			float dv = (last_speed - v.speed_kmh) / 3.6f / ((now - last_speed_at) / 1000.0f);

			if (v.brake && last_speed_at && dv > 6.0f && v.speed_kmh > 10) {
				ess_until = now + 1000;
			}
			last_speed = v.speed_kmh;
			last_speed_at = now;
		}
		double welcome = now - live_at < 800 ? (now - live_at) / 800.0
			       : now - gone_at < 800 ? -(1.0 - (now - gone_at) / 800.0) : 2;

		/* the stick's view, when one is heard, wins over this node's saved `panel mode` */
		int sv = zone_panel_view();
		bool lamps = sv >= 0 ? sv == 0 : lamp_mode;

		if (lamps) {
			/* this zone's lamps: the left zone (h 0) takes the left indicator, the right zone the right one */
			zl_state z = {.live = live, .turn_lit = h ? r : l, .autonomous = live && v.mode == 2,
				      .clock_s = zone_clock_s(), .welcome = welcome, .ess = now < ess_until,
				      .turn_since = (now - (h ? lit_r : lit_l)) / 1000.0,
				      .brake = live && v.brake, .reverse = live && v.reverse,
				      .high_beam = live && v.high_beam, .prox_m = prox};

			memset(fb, 0, sizeof(zr_fb));
			zl_lamps(fb, h, &z);
		} else if (live) {
			zr_state s = {0};

			s.lamps = (l ? ZR_LEFT : 0) | (r ? ZR_RIGHT : 0) | (v.brake ? ZR_BRAKE : 0) |
				  (v.reverse ? ZR_REVERSE : 0) | (v.hazard ? ZR_HAZARD : 0) | (v.high_beam ? ZR_HIGH_BEAM : 0);
			s.gear = v.gear;
			s.speed = v.speed_kmh;
			s.mode = v.mode;
			s.prox_m = prox;
			s.wheel_deg = v.wheel_deg;
			s.accel = v.accel;
			s.brake = v.brake_pedal;
			zr_car(fb, &s, (now - lit_l) / 1000.0, (now - lit_r) / 1000.0);
		} else {
			zr_nolink(fb, tick, prox);
		}


		if (pattern) {
			/*
			 * An orientation picture nothing symmetric can fake: red top-left corner, a green top row
			 * pointing right (brighter to the right), a blue left column pointing down, a white dot at
			 * (6, 3). Seen right on the LEDs = `panel map` is right.
			 */
			static const uint8_t R[3] = {255, 0, 0}, W[3] = {255, 255, 255};

			memset(fb, 0, sizeof(zr_fb));
			for (int x = 1; x < 10; x++) {
				const uint8_t g[3] = {0, (uint8_t)(40 + 24 * x), 0};

				zr_set(fb, 0, x + 10 * h, g);
			}
			for (int y = 1; y < 10; y++) {
				const uint8_t b[3] = {0, 0, (uint8_t)(40 + 24 * y)};

				zr_set(fb, y, 10 * h, b);
			}
			zr_set(fb, 0, 10 * h, R);
			zr_set(fb, 3, 6 + 10 * h, W);
		}
		zr_scale(out, fb, bright);

		/* this node's half, as drawn (full brightness, before the LED order), to the PC at 5 Hz */
		if (tick % 4 == 0) {
			static uint8_t pic[300];

			for (int y = 0; y < 10; y++) {
				memcpy(&pic[y * 30], fb[y][10 * h], 30);
			}
			zone_publish(ZONE_PUB_PANEL, pic, sizeof(pic));
		}

		memset(grb, 0, sizeof(grb));
		uint32_t sum = 0;

		for (int y = 0; y < 10; y++) {
			for (int x = 0; x < 10; x++) {
				const uint8_t *p = out[y][x + 10 * h];

				sum += p[0] + p[1] + p[2];
			}
		}
		/* over the budget: scale the whole frame down (`out` is already at `bright`) */
		if (sum > PANEL_BUDGET) {
			budget_hits++;
			for (int y = 0; y < 10; y++) {
				for (int x = 0; x < 10; x++) {
					for (int k = 0; k < 3; k++) {
						out[y][x + 10 * h][k] = (uint8_t)((uint32_t)out[y][x + 10 * h][k] * PANEL_BUDGET / sum);
					}
				}
			}
		}
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
		if (!t1s_is_sleeping() && cyccnt_runs()) {
			if (frames < 3) {
				LOG_INF("panel: frame %u (sum %u) ...", frames, sum);
			}
			ws2812_send(grb, sizeof(grb));
			if (frames < 3) {
				LOG_INF("panel: frame %u sent", frames);
			}
		}
#endif
		frames++;
		k_msleep(50);
	}
}
K_THREAD_DEFINE(panel_tid, 4096, panel_thread, NULL, NULL, NULL, 8, 0, 0);

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
	} else if (argc > 2 && !strcmp(argv[1], "mode")) {
		lamp_mode = strcmp(argv[2], "cluster") != 0;
		settings_save_one("panel/mode", &lamp_mode, 1);
	} else if (argc > 2 && !strcmp(argv[1], "pattern")) {
		pattern = !strcmp(argv[2], "on");
	} else if (argc > 1 && !strcmp(argv[1], "test")) {
		/* light chain index 0, 1, 2 ... 99, 0.15 s each, so the wiring can be read off the board */
		for (int i = 0; i < 100; i++) {
			test_led = i;
			k_msleep(150);
		}
		test_led = -1;
	}
	shell_print(sh, "panel: %s mode, %s%s, half %s, map %u (serpentine %u mirror-x %u mirror-y %u columns %u), bright %.2f, "
		    "%u frames | slot 3: %s stick %u/%u prox %u",
		    lamp_mode ? "zone lamps" : "cluster", HAS_PANEL ? "10x10 on slot 2" : "none built in", pattern ? " (orientation pattern)" : "",
		    half == 0 ? "left" : half == 1 ? "right" : "auto (PLCA id odd = left)", map_bits, map_bits & 1,
		    (map_bits >> 1) & 1, (map_bits >> 2) & 1, (map_bits >> 3) & 1, (double)bright, frames,
		    have_stick ? "thumbstick" : have_prox ? "proximity" : "none", stick_x, stick_y, prox_raw);
	shell_print(sh, "panel: current budget %u (sum of channel values per frame), frames scaled down %u", PANEL_BUDGET,
		    budget_hits);
	return 0;
}
SHELL_CMD_REGISTER(panel, NULL, "panel [mode lamps|cluster|map <bits>|bright <0.02..0.3>|half left|right|auto|pattern on|off|test]", cmd_panel);
