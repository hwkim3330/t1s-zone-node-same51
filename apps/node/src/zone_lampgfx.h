/*
 * The zone node as a vehicle lamp controller: lamp *functions* (DRL, low / high beam, sequential front and rear
 * indicators, tail, brake, reverse, the proximity warning) drawn on *segments*, each a list of pixels. Today a
 * segment is a set of pixels on the 10x10 RGB click; on the car, the same segments become addressable LED modules
 * (a module address + LED indices) and only the segment table changes, not the lamp logic.
 *
 * One panel = one side zone seen from that side of the car: top rows the front lamp, bottom rows the rear
 * combination lamp; x = 0 is the outer edge of the left zone (the right zone is drawn mirrored, its outer edge at
 * x = 19 of the 20-wide frame). Indicators sweep from the inner to the outer edge (one column per 25 ms after the
 * lamp lights); the lit phase comes from the LAN8651 PTP clock, so locked zones sweep together.
 *
 * Plain C over zone_render.h's 20x10 frame, like the ESP zone controller's renderer.
 */
#pragma once
#include <stdint.h>
#include <string.h>
#include "zone_render.h"

typedef struct {
	uint8_t live;        /* a car is there (else: the "no link" picture) */
	uint8_t turn_lit;    /* this zone's indicator is lit right now (indicator or hazard, PTP phase) */
	double turn_since;   /* seconds since it lit (the sweep) */
	uint8_t brake, reverse, high_beam;
	double prox_m;       /* < 0 or >= 1.5: no warning */
	uint8_t autonomous;  /* the automated driving system drives: the turquoise marker lamp (SAE J3134) */
	double clock_s;      /* a clock both zones share (PTP), for the marker's breathing */
	double welcome;      /* 0..1 while the welcome sweep runs (a car appeared), -1..0 goodbye, else 2 */
	uint8_t ess;         /* emergency stop signal: hard braking, the brake lamps flash */
	uint8_t mrm;         /* minimal risk manoeuvre: 1 stopping (rows 4-5 flash red/amber at 2 Hz), 2 stopped (breathing) */
} zl_state;

/* lamp levels before the panel brightness (0..255); brake and turn at full, tail and DRL dimmed */
#define ZL_DRL 70
#define ZL_LOW 110
#define ZL_HIGH 255
#define ZL_TAIL 45
#define ZL_BRAKE 255
#define ZL_REV 230
#define ZL_TURN 255

/* (x, y) of the left zone, mirrored for the right */
static inline void zl_px(zr_fb fb, int right, int x, int y, uint8_t r, uint8_t g, uint8_t b)
{
	const uint8_t c[3] = {r, g, b};
	int X = right ? 19 - x : x;

	if (x >= 0 && x < 10 && y >= 0 && y < 10) {
		zr_set(fb, y, X, c);
	}
}

/* one row segment from the inner edge (x = 9) outward, n columns of it lit */
static inline void zl_sweep(zr_fb fb, int right, int row, double since, uint8_t r, uint8_t g, uint8_t b)
{
	int n = (int)(since / 0.025) + 1;

	for (int k = 0; k < 10 && k < n; k++) {
		zl_px(fb, right, 9 - k, row, r, g, b);
	}
}

static inline void zl_lamps(zr_fb fb, int right, const zl_state *s)
{
	for (int y = 0; y < 10; y++) { /* this zone's half only */
		for (int x = 0; x < 10; x++) {
			zl_px(fb, right, x, y, 0, 0, 0);
		}
	}
	if (s->welcome >= -1 && s->welcome < 1) {
		/* welcome (0..1): light opens from the inner edge outward; goodbye (-1..0): it closes again */
		double f = s->welcome >= 0 ? s->welcome : -s->welcome;
		int n = (int)(f * 10.0 + 0.5);

		for (int y = 0; y < 10; y++) {
			for (int x = 9; x >= 10 - n; x--) {
				uint8_t w = (uint8_t)(40 + 20 * (9 - x) / 9);

				zl_px(fb, right, x, y, w, w, w);
			}
		}
		return;
	}
	if (!s->live) {
		/* no car: a dim dashed line through the middle, so a stale picture never looks live */
		for (int x = 0; x < 10; x += 2) {
			zl_px(fb, right, x, 4, 18, 18, 18);
			zl_px(fb, right, x, 5, 18, 18, 18);
		}
		return;
	}
	/* front lamp: DRL on top, beam below (low = inner half, high = wider and full white) */
	for (int x = 0; x < 10; x++) {
		zl_px(fb, right, x, 0, ZL_DRL, ZL_DRL, ZL_DRL);
	}
	for (int y = 1; y <= 2; y++) {
		for (int x = s->high_beam ? 1 : 5; x < 10; x++) {
			uint8_t v = s->high_beam ? ZL_HIGH : ZL_LOW;

			zl_px(fb, right, x, y, v, v, (uint8_t)(s->high_beam ? 255 : v));
		}
	}
	/* rear combination lamp: tail always, brake bright over it (flashing at 4 Hz as the emergency stop signal),
	 * reverse on the inner side of the bottom row */
	const int brake_on = s->brake && !(s->ess && ((int)(s->clock_s * 8) & 1));

	for (int y = 7; y <= 8; y++) {
		for (int x = 0; x < 10; x++) {
			uint8_t v = brake_on ? ZL_BRAKE : ZL_TAIL;

			zl_px(fb, right, x, y, v, 0, 0);
		}
	}
	for (int x = 0; x < 10; x++) {
		if (s->reverse && x >= 6) {
			zl_px(fb, right, x, 9, ZL_REV, ZL_REV, ZL_REV);
		} else {
			zl_px(fb, right, x, 9, brake_on ? ZL_BRAKE / 2 : ZL_TAIL / 2, 0, 0);
		}
	}
	/* sequential indicators, front (row 3) and rear (row 6), amber */
	if (s->turn_lit) {
		zl_sweep(fb, right, 3, s->turn_since, ZL_TURN, 120, 0);
		zl_sweep(fb, right, 6, s->turn_since, ZL_TURN, 120, 0);
	}
	/* automated driving marker (rows 4-5, inner half): turquoise, breathing every 2 s on the shared clock */
	if (s->autonomous && !(s->prox_m >= 0 && s->prox_m < 1.5)) {
		double ph = s->clock_s / 2.0 - (int64_t)(s->clock_s / 2.0);
		double b = 0.35 + 0.65 * (ph < 0.5 ? ph * 2 : (1 - ph) * 2);

		for (int x = 3; x < 10; x++) {
			zl_px(fb, right, x, 4, 0, (uint8_t)(200 * b), (uint8_t)(180 * b));
			zl_px(fb, right, x, 5, 0, (uint8_t)(200 * b), (uint8_t)(180 * b));
		}
	}
	/* MRM (rows 4-5, full width, over the marker): the ADS is gone and the vehicle stops itself -- a light nobody
	 * mistakes for an indicator: red / amber alternating at 2 Hz while stopping, red breathing once stopped */
	if (s->mrm) {
		double ph = s->clock_s * 2.0 - (int64_t)(s->clock_s * 2.0);

		for (int x = 0; x < 10; x++) {
			uint8_t r = 255, g = 0;

			if (s->mrm == 1) {
				g = ((x / 2 + (ph < 0.5)) & 1) ? 110 : 0;
			} else {
				double b = 0.3 + 0.7 * (ph < 0.5 ? ph * 2 : (1 - ph) * 2);

				r = (uint8_t)(255 * b);
			}
			zl_px(fb, right, x, 4, r, g, 0);
			zl_px(fb, right, x, 5, r, g, 0);
		}
		return;
	}
	/* proximity warning (rows 4-5): from 1.5 m, longer and redder the nearer, from the outer edge */
	if (s->prox_m >= 0 && s->prox_m < 1.5) {
		double near = 1.0 - s->prox_m / 1.5;
		int n = 1 + (int)(near * 9);
		uint8_t r = 255, g = (uint8_t)(160 * (1.0 - near));

		for (int x = 0; x < n; x++) {
			zl_px(fb, right, x, 4, r, g, 0);
			zl_px(fb, right, x, 5, r, g, 0);
		}
	}
}
