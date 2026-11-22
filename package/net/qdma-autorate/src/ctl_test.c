// SPDX-License-Identifier: GPL-2.0
/* Host self-test for ctl.c: cc -O2 -o ctl_test ctl_test.c ctl.c && ./ctl_test */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "ctl.h"

static const struct ctl_cfg C = {
	.start = 300, .floor = 150, .ceil = 380, .hi_ms = 10, .lo_ms = 3, .dec = 0.95,
	.inc_frac = 0.01, .util = 0.8, .holdoff_ns = 300 * NS_MS, .base_ns = 30000 * NS_MS,
	.cur_ns = 200 * NS_MS, .qual_ns = 500 * NS_MS, .min_samples = 3,
	.fast_frac = 0, .target_frac = 0.97, .cooldown_ns = 0,	/* v1 behaviour */
};
/* v2 defaults */
static const struct ctl_cfg C2 = {
	.start = 300, .floor = 150, .ceil = 380, .hi_ms = 10, .lo_ms = 5, .dec = 0.95,
	.inc_frac = 0.002, .util = 0.8, .holdoff_ns = 300 * NS_MS, .base_ns = 30000 * NS_MS,
	.cur_ns = 100 * NS_MS, .qual_ns = 300 * NS_MS, .min_samples = 3,
	.fast_frac = 0.02, .target_frac = 0.97, .cooldown_ns = 400 * NS_MS,
	.bind_backlog_frac = 0,	/* v2 rules */
};

/* v1-v4 calls: the 1 s load equals the tick's achieved and a sample just arrived (v5 knobs are 0 there anyway) */
static enum ctl_action step(const struct ctl_cfg *c, struct ctl_state *s, uint64_t now, bool have_signal,
			    double signal_ms, double achieved, bool binding)
{
	return ctl_step(c, s, now, have_signal, signal_ms, achieved, binding, achieved, now);
}

/* v5 defaults as shipped (live rates 300/285/375) */
static const struct ctl_cfg C5 = {
	.start = 300, .floor = 285, .ceil = 375, .hi_ms = 10, .lo_ms = 5, .dec = 0.95,
	.inc_frac = 0.002, .util = 0.8, .holdoff_ns = 300 * NS_MS, .base_ns = 30000 * NS_MS,
	.cur_ns = 100 * NS_MS, .qual_ns = 300 * NS_MS, .min_samples = 3,
	.fast_frac = 0.02, .target_frac = 0.97, .cooldown_ns = 400 * NS_MS,
	.bind_backlog_frac = 0.8, .probe_step = 10, .fast_step = 20, .probe_interval_ns = 500 * NS_MS,
	.cut_integral = 1.0, .load_gate = 0.5, .stale_ns = 2000 * NS_MS,
};

static void test_v5(void)
{
	uint64_t t0 = 1000000 * NS_MS, t;
	struct ctl_state s;
	int k;

	/* single spike (one 100 ms tick at 15 ms, loaded) -> no cut; the integral then decays */
	s = (struct ctl_state){ .rate = 350, .last_step = t0 - 100 * NS_MS };
	assert(ctl_step(&C5, &s, t0, true, 15, 340, true, 340, t0) == ACT_EXCESS && s.rate == 350);
	assert(fabs(s.integ - 0.5) < 1e-9);
	assert(ctl_step(&C5, &s, t0 + 100 * NS_MS, true, 9, 340, false, 340, t0) == ACT_NOBIND && fabs(s.integ - 0.4) < 1e-9);
	assert(ctl_step(&C5, &s, t0 + 200 * NS_MS, true, 4, 340, false, 340, t0) == ACT_NOBIND && s.integ == 0 && s.rate == 350);
	/* sustained excess: 15 ms (5 over) for 2 ticks = 1.0 ms*s -> cut from the rate, then the cooldown */
	s = (struct ctl_state){ .rate = 350, .last_step = t0 - 100 * NS_MS };
	assert(ctl_step(&C5, &s, t0, true, 15, 340, true, 340, t0) == ACT_EXCESS);
	assert(ctl_step(&C5, &s, t0 + 100 * NS_MS, true, 15, 340, true, 340, t0) == ACT_DEC);
	assert(s.rate == 330 && s.target == 330 && s.integ == 0);	/* 350*0.95 = 332.5 -> 330; 350*0.97 -> 330 */
	for (k = 2; k < 5; k++)		/* still 15 ms: integral refills, but the cooldown holds the next cut */
		assert(ctl_step(&C5, &s, t0 + k * 100 * NS_MS, true, 15, 330, true, 330, t0) == (k < 3 ? ACT_EXCESS : ACT_COOLDOWN));
	assert(ctl_step(&C5, &s, t0 + 500 * NS_MS, true, 15, 330, true, 330, t0) == ACT_DEC && s.rate == 310);	/* cut + 400 ms */
	/* a big splash still cuts at once: one tick at 25 ms is 1.5 ms*s */
	s = (struct ctl_state){ .rate = 350, .last_step = t0 - 100 * NS_MS };
	assert(ctl_step(&C5, &s, t0, true, 25, 340, true, 340, t0) == ACT_DEC);
	/* cut base ignores a low per-tick achieved while the gate is open (no cut straight to the floor) */
	s = (struct ctl_state){ .rate = 370, .last_step = t0 - 100 * NS_MS };
	assert(ctl_step(&C5, &s, t0, true, 30, 50, true, 300, t0) == ACT_DEC && s.rate == 350);
	/* idle: below 50 % of the cap -> hold whatever the signal, for 5 min */
	s = (struct ctl_state){ .rate = 370 };
	for (t = t0; t < t0 + 300000 * NS_MS; t += 100 * NS_MS)
		assert(ctl_step(&C5, &s, t, true, 40, 20, false, 20, t) == ACT_NOLOAD);
	assert(s.rate == 370 && s.integ == 0);
	/* idle with no samples at all (ePPing dead or no TCP) -> still hold, not floor */
	assert(ctl_step(&C5, &s, t + 10000 * NS_MS, false, 0, 1, false, 1, t0) == ACT_NOLOAD && s.rate == 370);
	/* loaded, last sample 1.9 s ago -> normal; 2.0 s -> floor */
	s = (struct ctl_state){ .rate = 370 };
	assert(ctl_step(&C5, &s, t0 + 1900 * NS_MS, false, 0, 300, true, 300, t0) == ACT_NOSIG && s.rate == 370);
	assert(ctl_step(&C5, &s, t0 + 2000 * NS_MS, false, 0, 300, true, 300, t0) == ACT_STALE);
	assert(s.rate == 285 && s.stale && s.target == 0);
	/* the floor itself keeps the gate open (300 >= 0.5 * 285); samples return -> normal control, probes up */
	assert(ctl_step(&C5, &s, t0 + 2100 * NS_MS, false, 0, 280, true, 280, t0) == ACT_STALE);
	assert(ctl_step(&C5, &s, t0 + 2200 * NS_MS, true, 1, 280, true, 280, t0 + 2150 * NS_MS) == ACT_INC);
	assert(!s.stale && s.rate == 290);	/* 285 + 10 -> 295 -> quantum 290 */
	/* knobs at 0 = v4 behaviour: a single tick above 10 ms cuts, from min(rate, achieved) */
	{
		struct ctl_cfg c4 = C5;

		c4.cut_integral = 0;
		c4.load_gate = 0;
		c4.stale_ns = 0;
		c4.floor = 150;
		s = (struct ctl_state){ .rate = 350 };
		assert(ctl_step(&c4, &s, t0, true, 10.5, 200, true, 0, 0) == ACT_DEC && s.rate == 190);
	}
	/* ceil creep: off by default -> the ceiling never moves */
	s = (struct ctl_state){ .rate = 370 };
	for (t = t0; t < t0 + 700000 * NS_MS; t += 100 * NS_MS)
		ctl_step(&C5, &s, t, true, 2, 370, true, 370, t);
	assert(s.rate == 370 && ctl_ceil(&C5, &s) == 375);
	/* on: 5 continuous min pinned with low delay -> +2 %; then 5 more min before the next raise */
	{
		struct ctl_cfg cc = C5;
		int raises = 0;
		double last = 375;

		cc.ceil_creep = true;
		s = (struct ctl_state){ .rate = 370 };
		for (t = t0; t <= t0 + 299900 * NS_MS; t += 100 * NS_MS)
			ctl_step(&cc, &s, t, true, 2, 370, true, 370, t);
		assert(ctl_ceil(&cc, &s) == 375);			/* 4:59.9 pinned: not yet */
		ctl_step(&cc, &s, t, true, 2, 370, true, 370, t);	/* 5:00 */
		assert(fabs(ctl_ceil(&cc, &s) - 382.5) < 1e-9);
		/* probes reach the new ceiling on the quantum (380); the next raise needs 5 more minutes there */
		last = ctl_ceil(&cc, &s);
		for (t += 100 * NS_MS; t < t0 + 600000 * NS_MS; t += 100 * NS_MS) {
			ctl_step(&cc, &s, t, true, 2, 380, true, 380, t);
			if (ctl_ceil(&cc, &s) != last) {
				raises++;
				last = ctl_ceil(&cc, &s);
			}
		}
		assert(s.rate == 380 && raises == 0);
		ctl_step(&cc, &s, t, true, 2, 380, true, 380, t);	/* 10:00 */
		assert(fabs(ctl_ceil(&cc, &s) - 382.5 * 1.02) < 1e-9);
		/* one tick above the cut threshold restarts the 5 min run */
		t += 100 * NS_MS;
		ctl_step(&cc, &s, t, true, 12, 380, true, 380, t);
		assert(s.ceil_since == 0);
		/* a cut lowers the rate, never the ceiling below cfg ceil (cir x 1.25) */
		s = (struct ctl_state){ .rate = 370, .ceil = 300 };
		assert(ctl_ceil(&cc, &s) == 375);
	}
}

int main(void)
{
	static struct flow_win f;
	struct ctl_state s = { .rate = 300 };
	uint64_t t0 = 1000000 * NS_MS, t;
	double d, v[] = { 5, 1, 9, 3 };

	/* window: 20 ms baseline for 10 s, then 45 ms for 300 ms -> delta 25 */
	for (t = t0; t < t0 + 10000 * NS_MS; t += 10 * NS_MS)
		flow_add(&f, t, 20 * NS_MS);
	assert(flow_delta(&f, &C, t, &d, NULL) && fabs(d) < 1e-9);
	for (; t < t0 + 10300 * NS_MS; t += 10 * NS_MS)
		flow_add(&f, t, 45 * NS_MS);
	{
		double cur;

		assert(flow_delta(&f, &C, t, &d, &cur) && fabs(d - 25) < 1e-9 && fabs(cur - 45) < 1e-9);
	}
	/* baseline ages out after 30 s of 45 ms only */
	for (; t < t0 + 41000 * NS_MS; t += 10 * NS_MS)
		flow_add(&f, t, 45 * NS_MS);
	assert(flow_delta(&f, &C, t, &d, NULL) && fabs(d) < 1e-9);
	/* no qualification after silence > qual window */
	assert(!flow_delta(&f, &C, t + 600 * NS_MS, &d, NULL));
	/* only 2 samples in qual window -> not qualified */
	memset(&f, 0, sizeof(f));
	flow_add(&f, t0, 20 * NS_MS);
	flow_add(&f, t0 + 10 * NS_MS, 20 * NS_MS);
	assert(!flow_delta(&f, &C, t0 + 20 * NS_MS, &d, NULL));

	assert(median(v, 4) == 4 && median(v, 3) == 3);

	/* control law */
	assert(step(&C, &s, t0, false, 0, 0, false) == ACT_NOSIG && s.rate == 300);
	assert(step(&C, &s, t0, true, 12, 280, false) == ACT_DEC && fabs(s.rate - 266) < 1e-9);
	assert(step(&C, &s, t0 + 100 * NS_MS, true, 1, 266, false) == ACT_HOLDOFF && fabs(s.rate - 266) < 1e-9);
	assert(step(&C, &s, t0 + 300 * NS_MS, true, 1, 266, false) == ACT_INC && fabs(s.rate - 269.8) < 1e-9);
	assert(step(&C, &s, t0 + 400 * NS_MS, true, 1, 100, false) == ACT_HOLD);	/* under-utilised */
	assert(step(&C, &s, t0 + 500 * NS_MS, true, 5, 269, false) == ACT_HOLD);	/* between lo and hi */
	s.rate = 155;
	assert(step(&C, &s, t0, true, 50, 300, false) == ACT_DEC && s.rate == 150);	/* floor */
	s.rate = 379;
	s.holdoff_until = 0;
	assert(step(&C, &s, t0, true, 0, 379, false) == ACT_INC && s.rate == 380);	/* ceiling */

	/* v2: cut cooldown */
	s = (struct ctl_state){ .rate = 300 };
	assert(step(&C2, &s, t0, true, 20, 320, false) == ACT_DEC && fabs(s.rate - 285) < 1e-9);
	assert(fabs(s.target - 291) < 1e-9);					/* 300 * 0.97 */
	assert(step(&C2, &s, t0 + 100 * NS_MS, true, 20, 300, false) == ACT_COOLDOWN && fabs(s.rate - 285) < 1e-9);
	assert(step(&C2, &s, t0 + 399 * NS_MS, true, 20, 300, false) == ACT_COOLDOWN);
	assert(step(&C2, &s, t0 + 400 * NS_MS, true, 20, 300, false) == ACT_DEC);	/* cooldown over: cut again */
	assert(fabs(s.rate - 270.75) < 1e-9 && fabs(s.target - 276.45) < 1e-9);	/* new target from 285 */
	/* cur window longer than the cooldown also blocks */
	{
		struct ctl_cfg cw = C2;
		struct ctl_state s2 = { .rate = 300 };

		cw.cur_ns = 600 * NS_MS;
		step(&cw, &s2, t0, true, 20, 300, false);
		assert(step(&cw, &s2, t0 + 500 * NS_MS, true, 20, 300, false) == ACT_COOLDOWN);
		assert(step(&cw, &s2, t0 + 600 * NS_MS, true, 20, 300, false) == ACT_DEC);
	}
	/* v2: fast recovery to target, then slow probe */
	s = (struct ctl_state){ .rate = 300 };
	step(&C2, &s, t0, true, 20, 300, false);					/* 285, target 291 */
	assert(step(&C2, &s, t0 + 100 * NS_MS, true, 1, 285, false) == ACT_HOLDOFF);
	assert(step(&C2, &s, t0 + 300 * NS_MS, true, 1, 285, false) == ACT_INC_FAST);
	assert(fabs(s.rate - 291) < 1e-9 && s.target == 0);			/* 285 + 7.6 clamped to 291 */
	assert(step(&C2, &s, t0 + 400 * NS_MS, true, 1, 291, false) == ACT_INC && fabs(s.rate - 291.76) < 1e-9);
	/* v2: deep cut then several fast steps; a cut during the ramp resets the target */
	s = (struct ctl_state){ .rate = 300 };
	s.rate = 200;
	s.target = 291;
	assert(step(&C2, &s, t0, true, 1, 200, false) == ACT_INC_FAST && fabs(s.rate - 207.6) < 1e-9);
	assert(step(&C2, &s, t0 + 100 * NS_MS, true, 1, 207, false) == ACT_INC_FAST && fabs(s.rate - 215.2) < 1e-9);
	assert(step(&C2, &s, t0 + 200 * NS_MS, true, 15, 215, false) == ACT_DEC);
	assert(fabs(s.target - 215.2 * 0.97) < 1e-9 && fabs(s.rate - 215 * 0.95) < 1e-9);
	/* v2: signal between thresholds (single flow at 3-5 ms) now increases */
	s = (struct ctl_state){ .rate = 200 };
	assert(step(&C2, &s, t0, true, 4, 200, false) == ACT_INC);
	/* no signal: hold regardless of utilisation */
	assert(step(&C2, &s, t0, false, 0, 400, false) == ACT_NOSIG);


	/* v4: binding detector */
	{
		struct bind_hist b = { 0 };
		int i;

		for (i = 0; i < 4; i++)
			bind_push(&b, 5, true);
		assert(!bind_binding(&b, 0.8));			/* only 4 of 5 ticks seen yet */
		bind_push(&b, 5, false);
		assert(bind_binding(&b, 0.8));			/* 4/5 = 0.8 */
		bind_push(&b, 5, false);
		assert(!bind_binding(&b, 0.8));			/* 3/5 */
		for (i = 0; i < 5; i++)
			bind_push(&b, 5, true);
		assert(bind_binding(&b, 0.8));
	}
	/* v4: hardware quantum */
	assert(hw_quantum(100) == 1 && hw_quantum(127.9) == 1 && hw_quantum(128) == 10 && hw_quantum(380) == 10);
	assert(hw_quantum(1300) == 100);
	assert(hw_rate(159) == 150 && hw_rate(300) == 300 && hw_rate(99.6) == 99);
	{
		struct ctl_cfg C4 = C2;

		C4.bind_backlog_frac = 0.8;
		C4.probe_step = 10;
		C4.fast_step = 20;
		C4.probe_interval_ns = 500 * NS_MS;
		C4.ceil = 380;
		/* quantum probe while binding, then wait the probe interval */
		s = (struct ctl_state){ .rate = 300 };
		assert(step(&C4, &s, t0, true, 2, 300, true) == ACT_INC && s.rate == 310);
		assert(step(&C4, &s, t0 + 100 * NS_MS, true, 2, 310, true) == ACT_PROBEWAIT && s.rate == 310);
		assert(step(&C4, &s, t0 + 499 * NS_MS, true, 2, 310, true) == ACT_PROBEWAIT);
		assert(step(&C4, &s, t0 + 500 * NS_MS, true, 2, 310, true) == ACT_INC && s.rate == 320);
		/* signal in the old hold zone (6 ms) while binding: no increase (inc threshold 5 unchanged) */
		assert(step(&C4, &s, t0 + 2000 * NS_MS, true, 6, 320, true) == ACT_HOLD && s.rate == 320);
		/* no increase without backlog, whatever the signal or achieved */
		assert(step(&C4, &s, t0 + 3000 * NS_MS, true, 0.5, 400, false) == ACT_NOBIND && s.rate == 320);
		/* ramp-up must not lower the rate: achieved climbing from 0, no backlog yet, low signal */
		s = (struct ctl_state){ .rate = 300 };
		{
			int k;

			for (k = 0; k < 30; k++)
				step(&C4, &s, t0 + k * 100 * NS_MS, k > 3, 1, k * 10.0, false);
			assert(s.rate == 300);
			/* ... and also not with backlog but a 5-10 ms signal */
			for (k = 30; k < 40; k++)
				step(&C4, &s, t0 + k * 100 * NS_MS, true, 7, 290, true);
			assert(s.rate == 300);
		}
		/* cut: unchanged rule, result on the quantum; target on the quantum */
		s = (struct ctl_state){ .rate = 330 };
		assert(step(&C4, &s, t0, true, 15, 330, true) == ACT_DEC);
		assert(s.rate == 310 && s.target == 320);		/* 330*0.95 = 313.5 -> 310; 330*0.97 = 320.1 -> 320 */
		assert(step(&C4, &s, t0 + 100 * NS_MS, true, 15, 330, true) == ACT_COOLDOWN && s.rate == 310);
		/* fast recovery: +20 per tick up to the target, then quantum probes */
		assert(step(&C4, &s, t0 + 300 * NS_MS, true, 1, 310, true) == ACT_INC_FAST && s.rate == 320 && s.target == 0);
		assert(step(&C4, &s, t0 + 400 * NS_MS, true, 1, 320, true) == ACT_INC && s.rate == 330);
		/* deep cut: floor stays on the quantum and fast steps are 20 */
		s = (struct ctl_state){ .rate = 160 };
		assert(step(&C4, &s, t0, true, 30, 100, true) == ACT_DEC && s.rate == 150 && s.target == 150);
		s = (struct ctl_state){ .rate = 200, .target = 290 };
		assert(step(&C4, &s, t0, true, 1, 200, true) == ACT_INC_FAST && s.rate == 220);
		/* ceiling is only a bound */
		s = (struct ctl_state){ .rate = 380 };
		assert(step(&C4, &s, t0, true, 1, 380, true) == ACT_INC && s.rate == 380);
	}
	test_v5();
	puts("ctl_test: all passed");
	return 0;
}
