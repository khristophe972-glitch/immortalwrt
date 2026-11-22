// SPDX-License-Identifier: GPL-2.0
/*
 * qdma-autorate: closed-loop download rate controller for the GL-MT6000.
 *
 * Detection: the patched ePPing BPF object (pping_kern.o, tcx ingress+egress on the WAN netdev)
 * reports per-flow TCP RTT samples through a ringbuf, consumed in-process.
 * Enforcement: writes the rate to qdma_rl's rate_mbps (QDMA per-queue max rate on the LAN queues).
 * Every 100 ms: per-flow delta = min RTT over 100 ms - min RTT over 30 s, for flows with >= 3 samples
 * in 300 ms; the signal is the median delta. The control law is in ctl.c.
 */
#include <bpf/bpf.h>
#include <bpf/libbpf.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <net/if.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <linux/pkt_sched.h>
#include <linux/gen_stats.h>
#include "pping.h"
#include "ctl.h"

#define MAX_FLOWS 1024

struct flow {
	bool used;
	struct network_tuple key;
	struct flow_win w;
};

static struct flow flows[MAX_FLOWS];
static unsigned long n_rtt_events, n_map_full, n_flow_drop;
static uint64_t last_sample_ns;		/* newest ePPing RTT event (CLOCK_MONOTONIC) */
static volatile sig_atomic_t stop, toggle_epping;
#define LOAD_WIN_NS (1000 * NS_MS)	/* load gate averaging window */
#define LOAD_MAX 64

static uint64_t now_ns(clockid_t c)
{
	struct timespec ts;

	clock_gettime(c, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

static bool key_eq(const struct network_tuple *a, const struct network_tuple *b)
{
	return a->proto == b->proto && a->saddr.port == b->saddr.port && a->daddr.port == b->daddr.port &&
	       !memcmp(&a->saddr.ip, &b->saddr.ip, sizeof(a->saddr.ip)) &&
	       !memcmp(&a->daddr.ip, &b->daddr.ip, sizeof(a->daddr.ip));
}

static unsigned int key_hash(const struct network_tuple *k)
{
	const uint8_t *p = (const uint8_t *)&k->saddr.ip;
	unsigned int h = 2166136261u, i;

	for (i = 0; i < sizeof(k->saddr.ip); i++)
		h = (h ^ p[i]) * 16777619u;
	p = (const uint8_t *)&k->daddr.ip;
	for (i = 0; i < sizeof(k->daddr.ip); i++)
		h = (h ^ p[i]) * 16777619u;
	h = (h ^ k->saddr.port) * 16777619u;
	h = (h ^ k->daddr.port) * 16777619u;
	return h;
}

/* Linear probing; slots freed by expiry are reused, so lookups scan to an unused slot. */
static struct flow *flow_get(const struct network_tuple *k, bool create)
{
	unsigned int h = key_hash(k), i;
	struct flow *free_slot = NULL;

	for (i = 0; i < MAX_FLOWS; i++) {
		struct flow *f = &flows[(h + i) % MAX_FLOWS];

		if (!f->used) {
			if (!f->key.proto)
				break;		/* never used: end of chain */
			if (!free_slot)
				free_slot = f;
			continue;
		}
		if (key_eq(&f->key, k))
			return f;
	}
	if (!create)
		return NULL;
	if (!free_slot && i < MAX_FLOWS)
		free_slot = &flows[(h + i) % MAX_FLOWS];
	if (!free_slot)
		return NULL;
	memset(free_slot, 0, sizeof(*free_slot));
	free_slot->used = true;
	free_slot->key = *k;
	return free_slot;
}

static int handle_event(void *ctx, void *data, size_t size)
{
	const union pping_event *e = data;
	struct flow *f;

	if (size < sizeof(e->event_type))
		return 0;
	if (e->event_type == EVENT_TYPE_MAP_FULL) {
		n_map_full++;
		return 0;
	}
	if (e->event_type != EVENT_TYPE_RTT || size < sizeof(e->rtt_event))
		return 0;
	n_rtt_events++;
	last_sample_ns = e->rtt_event.timestamp;
	f = flow_get(&e->rtt_event.flow, true);
	if (!f) {
		n_flow_drop++;
		return 0;
	}
	/* ePPing timestamps are bpf_ktime_get_ns() = CLOCK_MONOTONIC */
	flow_add(&f->w, e->rtt_event.timestamp, e->rtt_event.rtt);
	return 0;
}

/*
 * Without kernel BTF the ePPing map-cleanup iterators cannot load. Timestamps whose TSval is never
 * echoed (e.g. pure ACKs dropped by an egress ack-filter) would stay in packet_ts forever, so drop
 * entries older than max_age from userspace. Returns the number deleted.
 */
static unsigned int clean_packet_ts(int map_fd, uint64_t now, uint64_t max_age)
{
	struct packet_id key, next, del[256];
	unsigned int ndel = 0, i, total = 0;
	uint64_t ts;
	void *prev = NULL;

	while (bpf_map_get_next_key(map_fd, prev, &next) == 0) {
		if (bpf_map_lookup_elem(map_fd, &next, &ts) == 0 && ts < now && now - ts > max_age) {
			del[ndel++] = next;
			if (ndel == 256) {
				for (i = 0; i < ndel; i++)
					bpf_map_delete_elem(map_fd, &del[i]);
				total += ndel;
				ndel = 0;
				prev = NULL;	/* restart: deleted keys invalidate the iteration */
				continue;
			}
		}
		key = next;
		prev = &key;
	}
	for (i = 0; i < ndel; i++)
		bpf_map_delete_elem(map_fd, &del[i]);
	return total + ndel;
}

/*
 * Total backlog (bytes) of the `kind` qdiscs whose parent major is `major` on ifindex, read with an
 * RTM_GETQDISC dump on a kept-open rtnetlink socket (no tc process). Returns -1 on error.
 */
static long long lan_backlog(int nl, int ifindex, uint32_t major, const char *kind)
{
	static unsigned int seq;
	static char buf[65536];
	struct {
		struct nlmsghdr n;
		struct tcmsg t;
	} req = { .n = { .nlmsg_len = NLMSG_LENGTH(sizeof(struct tcmsg)), .nlmsg_type = RTM_GETQDISC,
			 .nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP, .nlmsg_seq = ++seq },
		  .t = { .tcm_family = AF_UNSPEC, .tcm_ifindex = ifindex } };
	long long total = 0;

	if (send(nl, &req, req.n.nlmsg_len, 0) < 0)
		return -1;
	for (;;) {
		ssize_t len = recv(nl, buf, sizeof(buf), 0);
		struct nlmsghdr *h;

		if (len <= 0)
			return -1;
		for (h = (struct nlmsghdr *)buf; NLMSG_OK(h, (size_t)len); h = NLMSG_NEXT(h, len)) {
			struct tcmsg *t = NLMSG_DATA(h);
			struct rtattr *a;
			int alen;
			bool match = false;
			long long bl = 0;

			if (h->nlmsg_seq != seq)
				continue;
			if (h->nlmsg_type == NLMSG_DONE)
				return total;
			if (h->nlmsg_type == NLMSG_ERROR)
				return -1;
			if (h->nlmsg_type != RTM_NEWQDISC || t->tcm_ifindex != ifindex ||
			    TC_H_MAJ(t->tcm_parent) != (major << 16))
				continue;
			alen = h->nlmsg_len - NLMSG_LENGTH(sizeof(*t));
			for (a = (struct rtattr *)((char *)t + NLMSG_ALIGN(sizeof(*t))); RTA_OK(a, alen);
			     a = RTA_NEXT(a, alen)) {
				if (a->rta_type == TCA_KIND)
					match = !strcmp(RTA_DATA(a), kind);
				else if (a->rta_type == TCA_STATS2) {
					struct rtattr *x = RTA_DATA(a);
					int xl = RTA_PAYLOAD(a);

					for (; RTA_OK(x, xl); x = RTA_NEXT(x, xl))
						if (x->rta_type == TCA_STATS_QUEUE &&
						    RTA_PAYLOAD(x) >= sizeof(struct gnet_stats_queue))
							bl = ((struct gnet_stats_queue *)RTA_DATA(x))->backlog;
				}
			}
			if (match)
				total += bl;
		}
	}
}

static void on_signal(int sig)
{
	if (sig == SIGUSR1)
		toggle_epping = 1;	/* fault injection: detach / reattach ePPing */
	else
		stop = 1;
}

static int read_u64(int fd, uint64_t *v)
{
	char buf[32];
	ssize_t n = pread(fd, buf, sizeof(buf) - 1, 0);

	if (n <= 0)
		return -1;
	buf[n] = 0;
	*v = strtoull(buf, NULL, 10);
	return 0;
}

/* Once-per-second rate-vs-RTT log, rotated at LOG_MAX to PATH.1 (one old file kept). */
#define LOG_MAX (1024 * 1024)
static FILE *log_open(const char *path)
{
	FILE *f = fopen(path, "a");

	if (f)
		setvbuf(f, NULL, _IOLBF, 0);
	return f;
}

static FILE *log_line(FILE *f, const char *path, const char *line)
{
	char old[256];

	if (!f)
		return NULL;
	fputs(line, f);
	if (ftell(f) >= LOG_MAX) {
		fclose(f);
		snprintf(old, sizeof(old), "%s.1", path);
		rename(path, old);
		f = log_open(path);
	}
	return f;
}

static int write_rate(int fd, int mbps)
{
	char buf[16];
	int n = snprintf(buf, sizeof(buf), "%d\n", mbps);

	return pwrite(fd, buf, n, 0) == n ? 0 : -errno;
}

static void usage(const char *p)
{
	fprintf(stderr,
		"usage: %s [options]\n"
		"  -i IFACE        WAN interface (eth1)          -o OBJ       BPF object (pping_kern.o)\n"
		"  -p PATH         rate sysfs file (/sys/module/qdma_rl/parameters/rate_mbps), '-' = dry run\n"
		"  --csv PATH (-w) per-tick CSV for debugging (default: none)  -d SEC  stop after SEC seconds (0 = run)\n"
		"  -v              log start/exit info (default: warnings and errors only, on stderr -> syslog under procd)\n"
		"  --start 300 --floor 150 --ceil 380 --tick-ms 100 --cut-thresh 10 --inc-thresh 5 --dec 0.95\n"
		"  --inc-frac 0.002 --fast-frac 0.02 --target-frac 0.97 --cut-cooldown-ms 400 --util 0.8\n"
		"  --holdoff-ms 300 --base-s 30 --cur-win-ms 100 --qual-ms 300 --min-samples 3\n"
		"  --refresh-ms 300 --sample-ms 10 (ePPing per-flow rate limit)\n"
		"  v5: --cut-integral-ms 1.0 (cut when the excess delay above --cut-thresh, integrated over time, reaches\n"
		"      this many ms*s; 0 = cut on any tick above) --load-gate 0.5 (act only while the 1 s WAN rx rate is\n"
		"      >= this x the cap, else hold; 0 = off) --stale-ms 2000 (loaded but no ePPing sample: floor; 0 = off)\n"
		"  --cir MBPS      committed rate: floor = cir x --floor-pct (0.95), ceil = cir x --ceil-pct (1.25),\n"
		"                  start = cir unless given; explicit --floor/--ceil/--start win\n"
		"  --ceil-creep    +2%% ceiling after 5 min at it with delay under --cut-thresh, 5 min between raises (off)\n"
		"  --log PATH      1 s rate-vs-RTT log (/tmp/qdma-autorate.log, rotated at 1 MB to PATH.1; - = off)\n"
		"  SIGUSR1 detaches ePPing (no samples), a second SIGUSR1 reattaches it (stale-path test)\n"
		"  v4: --bind-backlog-frac 0.8 --bind-window-ms 500 (cap binding = LAN backlog > 0 on >= frac of ticks;\n"
		"      0 = v2 rules) --probe-step 10 --probe-interval-ms 500 --fast-step 20 (Mbit/s per tick)\n"
		"      --lan-dev eth0 --lan-parent 1 --lan-kind cake (qdiscs whose backlog is summed)\n"
		"  (aliases: --hi-ms = --cut-thresh, --lo-ms = --inc-thresh, --cur-ms = --cur-win-ms)\n"
		"  --no-pure-acks  stock ePPing sampling (pure ACKs not timestamped)\n"
		"  --ts-max-age-ms 2000  userspace cleanup age for unmatched timestamps\n", p);
}

int main(int argc, char **argv)
{
	/* v2 defaults; v1 behaviour: --inc-frac 0.01 --inc-thresh 3 --cur-win-ms 200 --qual-ms 500
	 * --fast-frac 0 --cut-cooldown-ms 0 */
	struct ctl_cfg c = { .start = 300, .floor = 150, .ceil = 380, .hi_ms = 10, .lo_ms = 5, .dec = 0.95,
			     .inc_frac = 0.002, .util = 0.8, .holdoff_ns = 300 * NS_MS, .base_ns = 30000 * NS_MS,
			     .cur_ns = 100 * NS_MS, .qual_ns = 300 * NS_MS, .min_samples = 3,
			     .fast_frac = 0.02, .target_frac = 0.97, .cooldown_ns = 400 * NS_MS,
			     .bind_backlog_frac = 0.8, .probe_step = 10, .fast_step = 20,
			     .probe_interval_ns = 500 * NS_MS,	/* v4; --bind-backlog-frac 0 = v2 rules */
			     .cut_integral = 1.0, .load_gate = 0.5, .stale_ns = 2000 * NS_MS };	/* v5 */
	const char *lan_dev = "eth0", *lan_kind = "cake";
	unsigned int lan_major = 1;
	double bind_window_ms = 500;
	struct bind_hist bh = { 0 };
	int nl = -1, lan_ifindex = 0;
	const char *ifname = "eth1", *obj_path = "pping_kern.o", *rate_path = "/sys/module/qdma_rl/parameters/rate_mbps";
	const char *csv_path = NULL;	/* --csv PATH: per-tick CSV for debugging (off by default) */
	bool verbose = false;
	uint64_t last_warn = 0;
	double tick_ms = 100, refresh_ms = 300, sample_ms = 10, duration_s = 0, ts_max_age_ms = 2000;
	bool pure_acks = true;
	int ts_map_fd;
	uint64_t next_clean;
	unsigned long n_ts_cleaned = 0;
	struct bpf_config bc = { 0 };
	struct bpf_object *obj;
	struct bpf_program *prog;
	struct ring_buffer *rb;
	struct ctl_state s;
	int ifindex, link_in = -1, link_eg = -1, rate_fd = -1, rx_fd, err, opt, last_written = -1;
	uint64_t t_start, next_tick, last_write_ns = 0, rx_prev, rx_now, t_prev;
	uint64_t load_rx[LOAD_MAX], load_t[LOAD_MAX];
	unsigned int load_n, load_head = 0, load_cnt = 0;
	int prog_in, prog_eg;
	bool was_stale = false, floor_set = false, ceil_set = false, start_set = false;
	double cir = 0, floor_pct = 0.95, ceil_pct = 1.25, last_ceil = 0;
	const char *log_path = "/tmp/qdma-autorate.log";
	FILE *logf = NULL;
	uint64_t next_log;
	static double curs[MAX_FLOWS];
	FILE *csv;
	char path[128];
	static double deltas[MAX_FLOWS];
	static const struct option lo[] = {
		{ "start", 1, 0, 1 }, { "floor", 1, 0, 2 }, { "ceil", 1, 0, 3 }, { "tick-ms", 1, 0, 4 },
		{ "hi-ms", 1, 0, 5 }, { "lo-ms", 1, 0, 6 }, { "dec", 1, 0, 7 }, { "inc-frac", 1, 0, 8 },
		{ "util", 1, 0, 9 }, { "holdoff-ms", 1, 0, 10 }, { "base-s", 1, 0, 11 }, { "cur-ms", 1, 0, 12 },
		{ "qual-ms", 1, 0, 13 }, { "min-samples", 1, 0, 14 }, { "refresh-ms", 1, 0, 15 },
		{ "sample-ms", 1, 0, 16 }, { "no-pure-acks", 0, 0, 17 }, { "ts-max-age-ms", 1, 0, 18 },
		{ "cut-cooldown-ms", 1, 0, 19 }, { "fast-frac", 1, 0, 20 }, { "target-frac", 1, 0, 21 },
		{ "cut-thresh", 1, 0, 5 }, { "inc-thresh", 1, 0, 6 }, { "cur-win-ms", 1, 0, 12 },
		{ "bind-backlog-frac", 1, 0, 24 }, { "probe-step", 1, 0, 25 }, { "probe-interval-ms", 1, 0, 26 },
		{ "fast-step", 1, 0, 27 }, { "bind-window-ms", 1, 0, 28 }, { "lan-dev", 1, 0, 29 },
		{ "lan-parent", 1, 0, 30 }, { "lan-kind", 1, 0, 31 },
		{ "cut-integral-ms", 1, 0, 32 }, { "load-gate", 1, 0, 33 }, { "stale-ms", 1, 0, 34 },
		{ "cir", 1, 0, 35 }, { "floor-pct", 1, 0, 36 }, { "ceil-pct", 1, 0, 37 }, { "ceil-creep", 0, 0, 38 },
		{ "log", 1, 0, 39 },
		{ "csv", 1, 0, 'w' }, { "help", 0, 0, 'h' }, { 0 }
	};

	while ((opt = getopt_long(argc, argv, "i:o:p:w:d:vh", lo, NULL)) != -1) {
		double v = optarg ? atof(optarg) : 0;

		switch (opt) {
		case 'i': ifname = optarg; break;
		case 'o': obj_path = optarg; break;
		case 'p': rate_path = optarg; break;
		case 'w': csv_path = optarg; break;
		case 'd': duration_s = v; break;
		case 'v': verbose = true; break;
		case 1: c.start = v; start_set = true; break;
		case 2: c.floor = v; floor_set = true; break;
		case 3: c.ceil = v; ceil_set = true; break;
		case 4: tick_ms = v; break;
		case 5: c.hi_ms = v; break;
		case 6: c.lo_ms = v; break;
		case 7: c.dec = v; break;
		case 8: c.inc_frac = v; break;
		case 9: c.util = v; break;
		case 10: c.holdoff_ns = v * NS_MS; break;
		case 11: c.base_ns = v * 1000 * NS_MS; break;
		case 12: c.cur_ns = v * NS_MS; break;
		case 13: c.qual_ns = v * NS_MS; break;
		case 14: c.min_samples = v; break;
		case 15: refresh_ms = v; break;
		case 16: sample_ms = v; break;
		case 17: pure_acks = false; break;
		case 18: ts_max_age_ms = v; break;
		case 19: c.cooldown_ns = v * NS_MS; break;
		case 20: c.fast_frac = v; break;
		case 21: c.target_frac = v; break;
		case 24: c.bind_backlog_frac = v; break;
		case 25: c.probe_step = v; break;
		case 26: c.probe_interval_ns = v * NS_MS; break;
		case 27: c.fast_step = v; break;
		case 28: bind_window_ms = v; break;
		case 29: lan_dev = optarg; break;
		case 30: lan_major = (unsigned int)v; break;
		case 31: lan_kind = optarg; break;
		case 32: c.cut_integral = v; break;
		case 33: c.load_gate = v; break;
		case 34: c.stale_ns = v * NS_MS; break;
		case 35: cir = v; break;
		case 36: floor_pct = v; break;
		case 37: ceil_pct = v; break;
		case 38: c.ceil_creep = true; break;
		case 39: log_path = optarg; break;
		default: usage(argv[0]); return opt == 'h' ? 0 : 1;
		}
	}
	if (cir < 0 || floor_pct <= 0 || ceil_pct < floor_pct) {
		fprintf(stderr, "invalid parameters\n");
		return 1;
	}
	if (cir > 0) {	/* explicit --floor/--ceil/--start override the CIR-derived values */
		if (!floor_set)
			c.floor = cir * floor_pct;
		if (!ceil_set)
			c.ceil = cir * ceil_pct;
		if (!start_set)
			c.start = cir;
	}
	if (c.floor <= 0 || c.ceil < c.floor || c.start < c.floor || c.start > c.ceil || tick_ms < 10 ||
	    c.base_ns / BUCKET_NS > MAX_BUCKETS || c.dec <= 0 || c.dec >= 1 || c.lo_ms > c.hi_ms ||
	    c.fast_frac < 0 || c.target_frac <= 0 || c.target_frac > 1 || c.bind_backlog_frac < 0 ||
	    c.bind_backlog_frac > 1 || c.probe_step <= 0 || c.fast_step <= 0 ||
	    bind_window_ms / tick_ms < 1 || bind_window_ms / tick_ms > BIND_MAX ||
	    c.cut_integral < 0 || c.load_gate < 0 || c.load_gate > 1 || 1000 / tick_ms > LOAD_MAX - 1) {
		fprintf(stderr, "invalid parameters\n");
		return 1;
	}
	ifindex = if_nametoindex(ifname);
	if (!ifindex) {
		fprintf(stderr, "no interface %s\n", ifname);
		return 1;
	}
	setrlimit(RLIMIT_MEMLOCK, &(struct rlimit){ RLIM_INFINITY, RLIM_INFINITY });

	/* Same BPF configuration as `pping -T -r <sample-ms>` (TCP only, local RTTs filtered, SYN skipped). */
	bc.rate_limit = sample_ms * NS_MS;
	bc.track_tcp = true;
	bc.localfilt = true;
	bc.skip_syn = true;
	bc.push_individual_events = true;
	bc.eth_header = true;
	bc.ts_pure_acks = pure_acks;

	obj = bpf_object__open(obj_path);
	if (!obj) {
		fprintf(stderr, "open %s: %s\n", obj_path, strerror(errno));
		return 1;
	}
	{
		struct bpf_map *m;

		bpf_object__for_each_map(m, obj) {
			if (strstr(bpf_map__name(m), ".rodata"))
				bpf_map__set_initial_value(m, &bc, sizeof(bc));
			/* RTT aggregation is off (agg_rtts = false): its four preallocated per-CPU maps would hold about
			 * 0.5 GB of kernel memory on the 1 GB router for nothing; one entry each keeps the programs valid. */
			if (!strncmp(bpf_map__name(m), "map_v4_agg", 10) || !strncmp(bpf_map__name(m), "map_v6_agg", 10))
				bpf_map__set_max_entries(m, 1);
		}
	}
	/* Only the tc programs: the XDP program is unused and the iterators need kernel BTF. */
	bpf_object__for_each_program(prog, obj)
		if (strcmp(bpf_program__name(prog), "pping_tc_ingress") &&
		    strcmp(bpf_program__name(prog), "pping_tc_egress"))
			bpf_program__set_autoload(prog, false);
	err = bpf_object__load(obj);
	if (err) {
		fprintf(stderr, "load %s: %d\n", obj_path, err);
		return 1;
	}
	err = 1;	/* until the main loop finishes cleanly */
	prog_in = bpf_program__fd(bpf_object__find_program_by_name(obj, "pping_tc_ingress"));
	prog_eg = bpf_program__fd(bpf_object__find_program_by_name(obj, "pping_tc_egress"));
	link_in = bpf_link_create(prog_in, ifindex, BPF_TCX_INGRESS, NULL);
	link_eg = bpf_link_create(prog_eg, ifindex, BPF_TCX_EGRESS, NULL);
	if (link_in < 0 || link_eg < 0) {
		fprintf(stderr, "tcx attach on %s failed: %d %d\n", ifname, link_in, link_eg);
		goto out;
	}
	rb = ring_buffer__new(bpf_object__find_map_fd_by_name(obj, "events"), handle_event, NULL, NULL);
	if (!rb) {
		fprintf(stderr, "ring buffer: %s\n", strerror(errno));
		goto out;
	}
	ts_map_fd = bpf_object__find_map_fd_by_name(obj, "packet_ts");
	snprintf(path, sizeof(path), "/sys/class/net/%s/statistics/rx_bytes", ifname);
	rx_fd = open(path, O_RDONLY | O_CLOEXEC);
	if (rx_fd < 0 || read_u64(rx_fd, &rx_prev)) {
		fprintf(stderr, "open %s: %s\n", path, strerror(errno));
		goto out;
	}
	if (c.bind_backlog_frac > 0) {
		lan_ifindex = if_nametoindex(lan_dev);
		nl = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
		if (!lan_ifindex || nl < 0 || lan_backlog(nl, lan_ifindex, lan_major, lan_kind) < 0) {
			fprintf(stderr, "LAN backlog via netlink on %s failed\n", lan_dev);
			goto out;
		}
		/* start at a quantum-aligned rate */
		c.start = hw_rate(c.start);
	}
	if (strcmp(rate_path, "-")) {
		rate_fd = open(rate_path, O_WRONLY | O_CLOEXEC);
		if (rate_fd < 0) {
			fprintf(stderr, "open %s: %s\n", rate_path, strerror(errno));
			goto out;
		}
	}
	csv = csv_path ? fopen(csv_path, "w") : NULL;
	if (csv_path && !csv) {
		fprintf(stderr, "open %s: %s\n", csv_path, strerror(errno));
		goto out;
	}
	if (!strcmp(log_path, "-"))
		log_path = "";
	if (*log_path && !(logf = log_open(log_path))) {
		fprintf(stderr, "open %s: %s\n", log_path, strerror(errno));
		goto out;
	}
	if (csv)
		fprintf(csv, "t_mono_s,t_epoch,rate,written,achieved,signal_ms,n_flows,action,rtt_events,map_full,flow_drop,write_err,ts_cleaned,target,cooldown,hw_rate,backlog,binding,load,integ,stale\n");

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
	signal(SIGUSR1, on_signal);
	memset(&s, 0, sizeof(s));
	s.rate = c.start;
	t_start = t_prev = now_ns(CLOCK_MONOTONIC);
	last_sample_ns = t_start;
	next_log = t_start + 1000 * NS_MS;
	load_n = (unsigned int)(1000 / tick_ms + 0.5) + 1;	/* samples spanning 1 s */
	next_tick = t_start + tick_ms * NS_MS;
	next_clean = t_start + 1000 * NS_MS;
	if (verbose)
		fprintf(stderr, "qdma-autorate: %s ifindex %d, tcx links %d/%d, start %.0f floor %.0f ceil %.0f, cut>%.1f inc<%.1f ms, "
		"cooldown %llu ms, cur %llu qual %llu ms, v4 %s (backlog %s %s 1:, frac %.2f/%.0f ms, probe %.0f/%llu ms, fast %.0f), "
		"pure ACKs %s, refresh %.0f ms, v5 cut-integral %.2f ms*s load-gate %.2f stale %llu ms cir %.0f "
		"ceil-creep %s, log %s, writing %s\n",
		ifname, ifindex, link_in, link_eg, c.start, c.floor, c.ceil, c.hi_ms, c.lo_ms,
		(unsigned long long)(c.cooldown_ns / NS_MS), (unsigned long long)(c.cur_ns / NS_MS),
		(unsigned long long)(c.qual_ns / NS_MS), c.bind_backlog_frac > 0 ? "on" : "off", lan_dev, lan_kind,
		c.bind_backlog_frac, bind_window_ms, c.probe_step, (unsigned long long)(c.probe_interval_ns / NS_MS),
		c.fast_step, pure_acks ? "on" : "off", refresh_ms, c.cut_integral, c.load_gate,
		(unsigned long long)(c.stale_ns / NS_MS), cir, c.ceil_creep ? "on" : "off", *log_path ? log_path : "off",
		rate_fd >= 0 ? rate_path : "(dry run)");

	while (!stop) {
		uint64_t now = now_ns(CLOCK_MONOTONIC);
		int wait = now >= next_tick ? 0 : (int)((next_tick - now) / NS_MS) + 1, n = 0, i, werr = 0, want;
		double achieved, load, signal_ms = 0;
		unsigned int oldest;
		long long backlog;
		bool binding;
		enum ctl_action act;

		err = ring_buffer__poll(rb, wait);
		if (err < 0 && err != -EINTR) {
			fprintf(stderr, "ring_buffer__poll: %d\n", err);
			break;
		}
		now = now_ns(CLOCK_MONOTONIC);
		if (toggle_epping) {
			toggle_epping = 0;
			if (link_in >= 0) {
				close(link_in);
				close(link_eg);
				link_in = link_eg = -1;
				fprintf(stderr, "qdma-autorate: SIGUSR1: ePPing detached\n");
			} else {
				link_in = bpf_link_create(prog_in, ifindex, BPF_TCX_INGRESS, NULL);
				link_eg = bpf_link_create(prog_eg, ifindex, BPF_TCX_EGRESS, NULL);
				fprintf(stderr, "qdma-autorate: SIGUSR1: ePPing reattached (%d %d)\n", link_in, link_eg);
			}
		}
		if (now < next_tick)
			continue;
		next_tick += tick_ms * NS_MS;
		if (next_tick < now)	/* fell behind: resync instead of bursting ticks */
			next_tick = now + tick_ms * NS_MS;

		if (now >= next_clean) {
			n_ts_cleaned += clean_packet_ts(ts_map_fd, now, ts_max_age_ms * NS_MS);
			next_clean = now + 1000 * NS_MS;
		}
		read_u64(rx_fd, &rx_now);
		achieved = (double)(rx_now - rx_prev) * 8 * 1000 / (now - t_prev);	/* Mbit/s */
		rx_prev = rx_now;
		t_prev = now;
		/* load: WAN rx over the last ~1 s (shorter until the history fills) */
		load_rx[load_head] = rx_now;
		load_t[load_head] = now;
		load_head = (load_head + 1) % load_n;
		if (load_cnt < load_n)
			load_cnt++;
		oldest = load_cnt < load_n ? 0 : load_head;
		load = now > load_t[oldest] ? (double)(rx_now - load_rx[oldest]) * 8 * 1000 / (now - load_t[oldest]) : achieved;

		for (i = 0; i < MAX_FLOWS; i++) {
			struct flow *f = &flows[i];

			if (!f->used)
				continue;
			if (now - f->w.last_ns > c.base_ns) {	/* idle longer than the baseline window */
				f->used = false;
				continue;
			}
			if (flow_delta(&f->w, &c, now, &deltas[n], &curs[n]))
				n++;
		}
		if (n)
			signal_ms = median(deltas, n);	/* median() sorts: deltas and curs are not paired afterwards */
		backlog = nl >= 0 ? lan_backlog(nl, lan_ifindex, lan_major, lan_kind) : 0;
		if (nl >= 0)
			bind_push(&bh, (unsigned int)(bind_window_ms / tick_ms + 0.5), backlog > 0);
		binding = nl >= 0 && bind_binding(&bh, c.bind_backlog_frac);
		act = ctl_step(&c, &s, now, n > 0, signal_ms, achieved, binding, load, last_sample_ns);
		if (ctl_ceil(&c, &s) != last_ceil) {
			if (last_ceil)
				fprintf(stderr, "qdma-autorate: ceil creep: ceiling %.1f -> %.1f Mbit/s\n", last_ceil, ctl_ceil(&c, &s));
			last_ceil = ctl_ceil(&c, &s);
		}
		if (logf && now >= next_log) {
			char line[256], ts[32];
			time_t wall = time(NULL);

			strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%S", localtime(&wall));
			if (n)
				snprintf(line, sizeof(line), "%s rate=%d rtt_ms=%.1f delay_ms=%.1f rx_mbps=%.1f flows=%d ceil=%.1f action=%s\n",
					 ts, last_written, median(curs, n), signal_ms, load, n, ctl_ceil(&c, &s), ctl_action_name[act]);
			else
				snprintf(line, sizeof(line), "%s rate=%d rtt_ms=- delay_ms=- rx_mbps=%.1f flows=0 ceil=%.1f action=%s\n",
					 ts, last_written, load, ctl_ceil(&c, &s), ctl_action_name[act]);
			logf = log_line(logf, log_path, line);
			next_log += 1000 * NS_MS;
			if (next_log < now)
				next_log = now + 1000 * NS_MS;
		}
		if (s.stale != was_stale) {
			fprintf(stderr, s.stale ? "qdma-autorate: stale-samples -> floor (%.0f Mbit/s)\n"
						: "qdma-autorate: samples back, normal control from %.0f Mbit/s\n", s.rate);
			was_stale = s.stale;
		}

		want = (int)s.rate;
		/* half a tick of slack: tick jitter must not push a 300 ms refresh to 400 ms */
		if (rate_fd >= 0 && (want != last_written || now - last_write_ns + tick_ms * NS_MS / 2 >= refresh_ms * NS_MS)) {
			werr = write_rate(rate_fd, want);
			if (!werr) {
				last_written = want;
				last_write_ns = now;
			}
		}
		/* warnings, at most one line per minute */
		if ((werr || backlog < 0 || n_map_full || n_flow_drop) && now - last_warn >= 60000 * NS_MS) {
			fprintf(stderr, "qdma-autorate: warning: rate write err %d, lan backlog %lld, map full %lu, "
				"flow table drops %lu\n", werr, backlog, n_map_full, n_flow_drop);
			last_warn = now;
		}
		if (!csv)
			goto tick_done;
		fprintf(csv, "%.3f,%.3f,%.2f,%d,%.1f,", (now - t_start) / 1e9, now_ns(CLOCK_REALTIME) / 1e9,
			s.rate, last_written, achieved);
		if (n)
			fprintf(csv, "%.2f", signal_ms);
		fprintf(csv, ",%d,%s,%lu,%lu,%lu,%d,%lu,%.2f,%d,%.0f,%lld,%d,%.1f,%.3f,%d\n", n, ctl_action_name[act],
			n_rtt_events, n_map_full, n_flow_drop, werr, n_ts_cleaned, s.target, ctl_in_cooldown(&c, &s, now),
			hw_rate(s.rate), backlog, binding, load, s.integ, s.stale);
tick_done:
		if (duration_s > 0 && now - t_start >= duration_s * 1e9)
			break;
	}
	if (csv)
		fclose(csv);
	if (logf)
		fclose(logf);
	err = 0;
out:
	/* Fail open: restore the original rates, then detach. */
	if (rate_fd >= 0 && write_rate(rate_fd, 0))
		fprintf(stderr, "writing 0 to %s failed\n", rate_path);
	if (link_in >= 0)
		close(link_in);
	if (link_eg >= 0)
		close(link_eg);
	bpf_object__close(obj);
	if (verbose || err)
		fprintf(stderr, "qdma-autorate: exit%s (rtt events %lu, map full %lu, flow table drops %lu)\n",
			err ? " on error" : "", n_rtt_events, n_map_full, n_flow_drop);
	return err ? 1 : 0;
}
