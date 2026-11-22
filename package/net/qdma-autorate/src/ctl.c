// SPDX-License-Identifier: GPL-2.0
#include <stdlib.h>
#include "ctl.h"

const char *ctl_action_name[] = { "hold", "dec", "inc", "nosig", "holdoff", "cooldown", "inc_fast", "nobind",
				  "probewait", "noload", "stale", "excess" };

void bind_push(struct bind_hist *b, unsigned int window, bool backlog)
{
	if (window > BIND_MAX)
		window = BIND_MAX;
	if (window != b->n) {		/* (re)size: start over */
		b->n = window;
		b->head = b->count = 0;
	}
	b->v[b->head] = backlog;
	b->head = (b->head + 1) % b->n;
	if (b->count < b->n)
		b->count++;
}

bool bind_binding(const struct bind_hist *b, double frac)
{
	unsigned int i, k = 0;

	if (!b->n || b->count < b->n)	/* not enough history yet */
		return false;
	for (i = 0; i < b->n; i++)
		k += b->v[i];
	return k >= frac * b->n - 1e-9;
}

double hw_quantum(double mbps)
{
	double q = 0.001;		/* MAN <= 127 at EXP 0 is 1 kbit/s steps */

	while (mbps >= 128 * q)
		q *= 10;
	return q;
}

double hw_rate(double mbps)
{
	double q = hw_quantum(mbps);

	return (double)(long long)(mbps / q + 1e-9) * q;
}

void flow_add(struct flow_win *f, uint64_t now, uint64_t rtt)
{
	uint64_t b = now / BUCKET_NS;
	unsigned int slot = b % MAX_BUCKETS;

	if (f->bucket_id[slot] != b || f->bucket_min[slot] == 0) {
		f->bucket_id[slot] = b;
		f->bucket_min[slot] = rtt;
	} else if (rtt < f->bucket_min[slot]) {
		f->bucket_min[slot] = rtt;
	}
	f->rt[f->rhead] = now;
	f->rv[f->rhead] = rtt;
	f->rhead = (f->rhead + 1) % RECENT;
	if (f->rcount < RECENT)
		f->rcount++;
	f->last_ns = now;
}

bool flow_delta(const struct flow_win *f, const struct ctl_cfg *c, uint64_t now, double *delta_ms, double *cur_ms)
{
	uint64_t cur = UINT64_MAX, base = UINT64_MAX, b_now = now / BUCKET_NS;
	uint64_t nb = c->base_ns / BUCKET_NS;
	unsigned int i, nq = 0;

	for (i = 0; i < f->rcount; i++) {
		unsigned int k = (f->rhead + RECENT - 1 - i) % RECENT;
		uint64_t age = now - f->rt[k];

		if (f->rt[k] > now)
			continue;
		if (age > c->qual_ns)
			break;		/* ring is in time order, newest first */
		nq++;
		if (age <= c->cur_ns && f->rv[k] < cur)
			cur = f->rv[k];
	}
	if (nq < c->min_samples || cur == UINT64_MAX)
		return false;
	if (nb > MAX_BUCKETS)
		nb = MAX_BUCKETS;
	for (i = 0; i < nb; i++) {
		uint64_t b = b_now - i;
		unsigned int slot = b % MAX_BUCKETS;

		if (f->bucket_id[slot] == b && f->bucket_min[slot] && f->bucket_min[slot] < base)
			base = f->bucket_min[slot];
	}
	if (base == UINT64_MAX || base > cur)
		base = cur;
	*delta_ms = (double)(cur - base) / NS_MS;
	if (cur_ms)
		*cur_ms = (double)cur / NS_MS;
	return true;
}

static int cmpd(const void *a, const void *b)
{
	double x = *(const double *)a, y = *(const double *)b;

	return (x > y) - (x < y);
}

double median(double *v, int n)
{
	qsort(v, n, sizeof(*v), cmpd);
	return n % 2 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2;
}

/* A further cut waits for the cooldown and for the current-RTT window to hold only post-cut samples. */
bool ctl_in_cooldown(const struct ctl_cfg *c, const struct ctl_state *s, uint64_t now)
{
	if (!s->last_cut || !c->cooldown_ns)	/* cooldown 0 = v1 behaviour: cut on every tick */
		return false;
	return now < s->last_cut + c->cooldown_ns || now < s->last_cut + c->cur_ns;
}

/* The ceiling in force: cfg ceil, raised by ceil creep; never below cfg ceil. */
double ctl_ceil(const struct ctl_cfg *c, const struct ctl_state *s)
{
	return s->ceil > c->ceil ? s->ceil : c->ceil;
}

/* Increase by the fast step below the recovery target, the slow probe above it. */
static enum ctl_action increase(const struct ctl_cfg *c, struct ctl_state *s)
{
	enum ctl_action a = ACT_INC;

	if (c->fast_frac > 0 && s->target > 0 && s->rate < s->target) {
		s->rate += c->fast_frac * c->ceil;
		if (s->rate >= s->target) {
			s->rate = s->target;
			s->target = 0;	/* reached: slow probing from here */
		}
		a = ACT_INC_FAST;
	} else {
		s->target = 0;
		s->rate += c->inc_frac * c->ceil;
	}
	if (s->rate > c->ceil)
		s->rate = c->ceil;
	return a;
}

/*
 * v4 increase rule (bind_backlog_frac > 0), applied after the unchanged cut logic. Nothing but a cut lowers
 * the rate. Increase only while the cap binds (LAN backlog) and the signal is below the increase
 * threshold: +fast_step per tick below the recovery target, else +probe_step at most every
 * probe_interval. Rates are kept on the hardware quantum.
 */
static enum ctl_action increase_v4(const struct ctl_cfg *c, struct ctl_state *s, uint64_t now,
				   bool have_signal, double signal_ms, bool binding)
{
	enum ctl_action a;

	if (!have_signal)
		return ACT_NOSIG;
	if (!binding)
		return ACT_NOBIND;
	if (signal_ms >= c->lo_ms)
		return ACT_HOLD;
	if (now < s->holdoff_until)
		return ACT_HOLDOFF;
	if (s->target > 0 && s->rate < s->target) {
		s->rate += c->fast_step;
		if (s->rate >= s->target) {
			s->rate = s->target;
			s->target = 0;
		}
		a = ACT_INC_FAST;
	} else {
		if (s->last_probe && now < s->last_probe + c->probe_interval_ns)
			return ACT_PROBEWAIT;
		s->target = 0;
		s->rate += c->probe_step;
		s->last_probe = now;
		a = ACT_INC;
	}
	if (s->rate > ctl_ceil(c, s))
		s->rate = ctl_ceil(c, s);
	s->rate = hw_rate(s->rate);
	return a;
}

/* Multiplicative cut from base, clamped to the floor; v4+ keeps rate and target on the hardware quantum. */
static enum ctl_action cut(const struct ctl_cfg *c, struct ctl_state *s, uint64_t now, double base, bool v4)
{
	s->target = s->rate * c->target_frac;	/* a cut during fast recovery sets a new target */
	s->rate = base * c->dec;
	if (s->rate < c->floor)
		s->rate = c->floor;
	if (v4) {	/* stay on the hardware quantum, not below the floor */
		s->target = hw_rate(s->target);
		s->rate = hw_rate(s->rate);
		if (s->rate < c->floor)
			s->rate = c->floor;
	}
	s->last_cut = now;
	s->holdoff_until = now + c->holdoff_ns;
	return ACT_DEC;
}

/*
 * v4/v5 rules (bind_backlog_frac > 0):
 * - load gate: below load_gate * rate (1 s WAN rx) hold, whatever the signal, so light traffic never moves the rate;
 * - stale: loaded but no ePPing sample for stale_ns -> floor, until samples return;
 * - cut only when the excess-delay integral reaches cut_integral (a single splash above hi_ms does not cut),
 *   from the current rate (the gate already guarantees real load), then the post-cut cooldown;
 * - otherwise the v4 increase rule.
 */
static enum ctl_action step_v4(const struct ctl_cfg *c, struct ctl_state *s, uint64_t now, bool have_signal,
			       double signal_ms, double achieved, bool binding, double load, uint64_t last_sample)
{
	double dt = s->last_step && now > s->last_step ? (double)(now - s->last_step) / 1e9 : 0;

	s->last_step = now;
	if (c->load_gate > 0 && load < c->load_gate * s->rate) {
		s->integ = 0;
		return ACT_NOLOAD;
	}
	if (c->stale_ns && now >= last_sample + c->stale_ns) {
		s->integ = 0;
		s->target = 0;
		s->rate = c->floor;
		s->stale = true;
		return ACT_STALE;
	}
	s->stale = false;
	/* ceil creep: pinned at the ceiling (on the quantum) with the signal under the cut threshold */
	if (c->ceil_creep) {
		if (have_signal && signal_ms < c->hi_ms && s->rate >= hw_rate(ctl_ceil(c, s))) {
			if (!s->ceil_since)
				s->ceil_since = now;
			if (now - s->ceil_since >= CREEP_NS && (!s->ceil_raised || now - s->ceil_raised >= CREEP_NS)) {
				s->ceil = ctl_ceil(c, s) * CREEP_FRAC;
				s->ceil_raised = now;
				s->ceil_since = now;	/* another 5 continuous minutes for the next raise */
			}
		} else {
			s->ceil_since = 0;
		}
	}
	if (have_signal) {	/* grows above hi_ms, decays toward 0 below it */
		s->integ += (signal_ms - c->hi_ms) * dt;
		if (s->integ < 0)
			s->integ = 0;
		if (c->cut_integral > 0 && s->integ > 2 * c->cut_integral)
			s->integ = 2 * c->cut_integral;	/* bounded wind-up during the cooldown */
	}
	if (have_signal && signal_ms > c->hi_ms) {
		if (c->cut_integral > 0 && s->integ < c->cut_integral)
			return ACT_EXCESS;
		if (ctl_in_cooldown(c, s, now))
			return ACT_COOLDOWN;
		s->integ = 0;
		return cut(c, s, now, c->load_gate > 0 || s->rate < achieved ? s->rate : achieved, true);
	}
	return increase_v4(c, s, now, have_signal, signal_ms, binding);
}

enum ctl_action ctl_step(const struct ctl_cfg *c, struct ctl_state *s, uint64_t now,
			 bool have_signal, double signal_ms, double achieved, bool binding,
			 double load, uint64_t last_sample)
{
	if (c->bind_backlog_frac > 0)
		return step_v4(c, s, now, have_signal, signal_ms, achieved, binding, load, last_sample);
	if (!have_signal)
		return ACT_NOSIG;	/* hold, whatever the utilisation */
	if (signal_ms > c->hi_ms) {
		if (ctl_in_cooldown(c, s, now))
			return ACT_COOLDOWN;
		return cut(c, s, now, s->rate < achieved ? s->rate : achieved, false);
	}
	if (signal_ms < c->lo_ms && achieved >= c->util * s->rate) {
		if (now < s->holdoff_until)
			return ACT_HOLDOFF;
		return increase(c, s);
	}
	return ACT_HOLD;
}
