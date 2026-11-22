/* SPDX-License-Identifier: GPL-2.0 */
/* Per-flow RTT windows and the rate control law. No BPF/libbpf dependency, so it is host-testable. */
#ifndef CTL_H
#define CTL_H
#include <stdint.h>
#include <stdbool.h>

#define NS_MS 1000000ULL
#define BUCKET_NS (100 * NS_MS)		/* baseline buckets: min RTT per 100 ms */
#define MAX_BUCKETS 600			/* up to 60 s baseline window */
#define RECENT 128			/* recent raw samples per flow (>= 500 ms at 10 ms sampling) */

struct flow_win {
	uint64_t bucket_id[MAX_BUCKETS];	/* absolute bucket number held in each slot */
	uint64_t bucket_min[MAX_BUCKETS];
	uint64_t rt[RECENT], rv[RECENT];	/* recent sample times and RTTs (ns) */
	unsigned int rhead, rcount;
	uint64_t last_ns;
};

struct ctl_cfg {
	double start, floor, ceil;		/* Mbit/s */
	double hi_ms, lo_ms;			/* decrease above hi, increase below lo */
	double dec, inc_frac, util;		/* rate*dec on bloat, +inc_frac*ceil when util ok */
	double fast_frac, target_frac;		/* fast recovery step (x ceil) up to target = pre-cut rate * target_frac */
	uint64_t holdoff_ns, base_ns, cur_ns, qual_ns;
	uint64_t cooldown_ns;			/* min time between cuts (also: cur window must be post-cut) */
	double bind_backlog_frac;		/* v4: cap binding if LAN backlog > 0 on >= frac of recent ticks (0 = v2 rules) */
	double probe_step, fast_step;		/* v4: Mbit/s per probe / per fast-recovery tick */
	uint64_t probe_interval_ns;		/* v4: min time between slow probes */
	double cut_integral;			/* v5: cut when the excess-delay integral (ms*s above hi) reaches this; 0 = any tick */
	double load_gate;			/* v5: act only while the 1 s WAN rx rate >= load_gate * rate; 0 = off */
	uint64_t stale_ns;			/* v5: loaded but no ePPing sample for this long -> floor; 0 = off */
	bool ceil_creep;			/* v5: +2% ceiling after 5 min pinned at it with low delay */
	unsigned int min_samples;
};

struct ctl_state {
	double rate;
	uint64_t holdoff_until;
	uint64_t last_cut;			/* 0 = never */
	double target;				/* fast-recovery target, 0 = none */
	uint64_t last_probe;			/* v4: time of the last slow probe */
	double integ;				/* v5: excess-delay integral, ms*s */
	uint64_t last_step;			/* v5: time of the previous ctl_step (integration dt) */
	bool stale;				/* v5: rate forced to the floor by missing samples */
	double ceil;				/* v5: current ceiling (ceil creep); 0 = cfg ceil */
	uint64_t ceil_since, ceil_raised;	/* v5: start of the current at-ceiling run, last raise */
};

/* v4 binding detector: did the LAN qdiscs hold a backlog on >= frac of the last n ticks? */
#define BIND_MAX 64
struct bind_hist {
	unsigned char v[BIND_MAX];
	unsigned int n, head, count;
};
void bind_push(struct bind_hist *b, unsigned int window, bool backlog);
bool bind_binding(const struct bind_hist *b, double frac);

/* QDMA encodes MAN (7 bits) * 10^EXP kbit/s: the step is 1 Mbit/s below 128, 10 below 1280, ... */
double hw_quantum(double mbps);
double hw_rate(double mbps);		/* what the hardware actually enforces (rounded down) */

enum ctl_action { ACT_HOLD, ACT_DEC, ACT_INC, ACT_NOSIG, ACT_HOLDOFF, ACT_COOLDOWN, ACT_INC_FAST, ACT_NOBIND,
		  ACT_PROBEWAIT, ACT_NOLOAD, ACT_STALE, ACT_EXCESS };
bool ctl_in_cooldown(const struct ctl_cfg *c, const struct ctl_state *s, uint64_t now);
extern const char *ctl_action_name[];

void flow_add(struct flow_win *f, uint64_t now, uint64_t rtt);
/* Returns true and the flow's delta (ms) if it qualifies: >= min_samples in qual window and a sample in cur window. */
bool flow_delta(const struct flow_win *f, const struct ctl_cfg *c, uint64_t now, double *delta_ms, double *cur_ms);
#define CREEP_NS (300000 * NS_MS)		/* ceil creep: 5 min at the ceiling, 5 min between raises */
#define CREEP_FRAC 1.02
double ctl_ceil(const struct ctl_cfg *c, const struct ctl_state *s);
double median(double *v, int n);
/*
 * achieved: WAN rx Mbit/s over the last tick (v2 rules); load: WAN rx Mbit/s over the last 1 s (v5 load gate);
 * last_sample: time of the newest ePPing RTT event (v5 stale timeout).
 */
enum ctl_action ctl_step(const struct ctl_cfg *c, struct ctl_state *s, uint64_t now,
			 bool have_signal, double signal_ms, double achieved, bool binding,
			 double load, uint64_t last_sample);
#endif
