// SPDX-License-Identifier: GPL-2.0
/*
 * qdma_rl v3: runtime knob for the MT7986 QDMA TX rate (Linux 6.18.56, ImmortalWrt fork):
 * per-queue max rate (mode=queue) or one hardware scheduler shared by the LAN queues (mode=scheduler).
 *
 * Ported from mindwolf80/mt7986-qdma-autorate @ 47fd09adc5 (written against OpenWrt
 * 25.12.6 / Linux 6.12.112). Verified against 6.18.56: every offset and bit position
 * below is unchanged (mt7986_reg_map.qdma.qtx_sch = 0x4404, .tx_sch_rate = 0x4798,
 * MTK_QTX_OFFSET = 0x10, MTK_QDMA_NUM_QUEUES = 16, TX_SEL_V2 = GENMASK(31,30),
 * MAX_RATE WEIGHT/EN/MAN/EXP = 15:12/11/10:4/3:0). The only delta is the page-select
 * register 0x45f0, which 6.18.56 does not define at all -- see page_ok() below.
 *
 * Register layout, offsets and rate encoding are taken from
 * drivers/net/ethernet/mediatek/mtk_eth_soc.{c,h}:
 *   mt7986_reg_map.qdma.qtx_sch = 0x4404, .tx_sch_rate = 0x4798,
 *   MTK_QTX_OFFSET = 0x10, MTK_QDMA_NUM_QUEUES = 16 (all on page 0), netsys v2 QTX_SCH layout:
 *   TX_SEL_V2[31:30] MAX_RATE_WEIGHT[15:12] MAX_RATE_EN[11] MAX_RATE_MAN[10:4] MAX_RATE_EXP[3:0],
 *   rate = MAN * 10^EXP kbit/s (mtk_set_queue_speed: 10M=1e4, 100M=1e5, 1G=1e6).
 * Like mtk_set_queue_speed(), writes use __raw_writel without page select, under RTNL.
 *
 * The 25.12.6 source defines no rate fields for the scheduler rate registers (only
 * MTK_QDMA_TX_SCH_MAX_WFQ). Scheduler mode uses the field definitions and the scheduler-id
 * mapping from MediaTek's mtk-openwrt-feeds @ c2b24b480,
 * 25.12/files/target/linux/mediatek/patches-6.12/999-eth-90-mtk_eth_soc-support-proprietary-debugfs.patch:
 *   MTK_QDMA_TX_SCH_RATE_EN BIT(11), _RATE_MAN GENMASK(10,4), _RATE_EXP GENMASK(3,0) per 16-bit half
 *   (patch lines 3899-3904); scheduler id -> tx_sch_rate + (id >> 1) * 4, upper half for odd ids
 *   (lines 154-161, 225-252). Only these register writes are used; the driver is not changed.
 *
 * mode=scheduler: listed LAN queues get TX_SEL_V2 = 1 and scheduler 1 (upper half of tx_sch_rate word 0)
 * gets the rate; scheduler 0 (WAN q1 and everything else) is never modified. Every rate write re-asserts
 * the whole configuration, so a driver rewrite (mtk_set_queue_speed on link change) is undone by the
 * next write.
 *
 * Every queue in `queues` gets the same max rate. Nothing is written on load. rate_mbps=0, the
 * watchdog (no rate write for watchdog_ms while capped) and rmmod restore every saved register.
 */
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/io.h>
#include <linux/bitfield.h>
#include <linux/rtnetlink.h>
#include <linux/workqueue.h>
#include <linux/jiffies.h>
#include <linux/string.h>
#include "qdma_enc.h"

#define QDMA_QTX_SCH		0x4404
#define QDMA_PAGE		0x45f0
#define QDMA_TX_SCH_RATE	0x4798
#define QDMA_TX_SCH_WORDS	2	/* netsys v2: tx_sch_rate and tx_sch_rate + 4 */
#define QTX_OFFSET		0x10
#define QDMA_NUM_QUEUES		16
#define MAX_RATE_WEIGHT		GENMASK(15, 12)
#define MAX_RATE_EN		BIT(11)
#define MAX_RATE_MAN		GENMASK(10, 4)
#define MAX_RATE_EXP		GENMASK(3, 0)
#define MAX_RATE_FIELDS		(MAX_RATE_EN | MAX_RATE_MAN | MAX_RATE_EXP)
#define TX_SEL_V2		GENMASK(31, 30)
/* vendor field definitions, one 16-bit half per scheduler (see header) */
#define SCH_HALF		GENMASK(15, 0)
#define SCH_MAX_WFQ		BIT(15)
#define SCH_RATE_EN		BIT(11)
#define SCH_RATE_MAN		GENMASK(10, 4)
#define SCH_RATE_EXP		GENMASK(3, 0)
#define LAN_SCHED		1	/* scheduler used for the LAN queues */
#define LAN_QUEUE_MASK		(BIT(3) | BIT(4) | BIT(5) | BIT(6) | BIT(8))	/* DSA p0-p3, p5 (+3) */

static void __iomem *base;
static char queues_str[64] = "3,4,5,6,8";	/* LAN queues: DSA p0-p3, p5 (+3) */
static int qlist[QDMA_NUM_QUEUES], nq;
static u32 orig_sch[QDMA_NUM_QUEUES], last_sch[QDMA_NUM_QUEUES];
static u32 orig_tx_sch_rate[QDMA_TX_SCH_WORDS];
static bool saved, capped;
static int rate_mbps;
static int watchdog_ms = 2000;
static char mode_str[16] = "queue";
static unsigned long watchdog_trips, writes, corrections;
static u32 last_w0;	/* last value written to tx_sch_rate word 0 */
static struct delayed_work wd_work;

static u32 sch_ofs(int q) { return QDMA_QTX_SCH + QTX_OFFSET * q; }
static u32 rd(u32 ofs) { return __raw_readl(base + ofs); }

static u64 val_rate_kbps(u32 v)
{
	u64 r = FIELD_GET(MAX_RATE_MAN, v);
	u32 e = FIELD_GET(MAX_RATE_EXP, v);

	while (e--)
		r *= 10;
	return r;
}

/* 6.18.56 has no QDMA page-select register: mainline addresses every queue register
 * directly and 0x45f0 is not part of mt7986_reg_map. Upstream (6.12.112) used it to
 * refuse writes when another code path had selected a page; that guard cannot hold
 * here, and blocking on an undefined register would disable the module outright.
 * Read it once as a diagnostic and never refuse on it.
 */
static bool page_reported;

static int page_ok(void)
{
	if (!page_reported) {
		page_reported = true;
		pr_info("qdma_rl: 0x%x reads 0x%08x (no page register on 6.18, not used)\n",
			QDMA_PAGE, rd(QDMA_PAGE));
	}
	return 0;
}

/* RTNL held. Save every register this module may change. */
static void save_all(void)
{
	int i;

	for (i = 0; i < nq; i++)
		last_sch[i] = orig_sch[i] = rd(sch_ofs(qlist[i]));
	for (i = 0; i < QDMA_TX_SCH_WORDS; i++)
		orig_tx_sch_rate[i] = rd(QDMA_TX_SCH_RATE + 4 * i);
	last_w0 = orig_tx_sch_rate[0];
	saved = true;
	for (i = 0; i < nq; i++)
		pr_info("qdma_rl: saved q%d QTX_SCH 0x%08x\n", qlist[i], orig_sch[i]);
	pr_info("qdma_rl: saved tx_sch_rate 0x%08x 0x%08x\n", orig_tx_sch_rate[0], orig_tx_sch_rate[1]);
}

/* RTNL held. */
static void restore_all(const char *why)
{
	int i;
	u32 cur;

	if (!saved || page_ok())
		return;
	for (i = 0; i < nq; i++) {
		cur = rd(sch_ofs(qlist[i]));
		if (cur != last_sch[i])
			pr_warn("qdma_rl: q%d changed behind us (0x%08x, last written 0x%08x)\n",
				qlist[i], cur, last_sch[i]);
		__raw_writel(orig_sch[i], base + sch_ofs(qlist[i]));
		last_sch[i] = orig_sch[i];
	}
	for (i = 0; i < QDMA_TX_SCH_WORDS; i++)
		if (rd(QDMA_TX_SCH_RATE + 4 * i) != orig_tx_sch_rate[i])
			__raw_writel(orig_tx_sch_rate[i], base + QDMA_TX_SCH_RATE + 4 * i);
	last_w0 = orig_tx_sch_rate[0];
	capped = false;
	rate_mbps = 0;
	pr_info("qdma_rl: restored %d queues (%s)\n", nq, why);
}

/* largest MAN*10^EXP kbit/s <= requested, MAN <= 127 (7 bits); same in QTX_SCH and tx_sch_rate */
static void encode(int mbps, u32 *man, u32 *exp)
{
	qdma_encode(mbps, man, exp);
}

static bool lan_only(void)
{
	int i;

	for (i = 0; i < nq; i++)
		if (!(LAN_QUEUE_MASK & BIT(qlist[i])))
			return false;
	return true;
}

/* RTNL held. Write val unless the register already holds it; count values changed behind our back. */
static void assert_reg(u32 ofs, u32 val, u32 *last)
{
	u32 cur = rd(ofs);

	if (cur == val)
		return;
	if (capped && cur != *last) {
		corrections++;
		pr_info_ratelimited("qdma_rl: re-assert 0x%x: 0x%08x -> 0x%08x\n", ofs, cur, val);
	}
	__raw_writel(val, base + ofs);
	*last = val;
}

/* RTNL held. LAN queues -> scheduler 1, scheduler 1 rate = mbps; scheduler 0 half and word 1 untouched. */
static int apply_sched(int mbps)
{
	u32 man, exp, half, w0;
	int i;

	if (!lan_only())
		return -EINVAL;
	encode(mbps, &man, &exp);
	for (i = 0; i < nq; i++)
		assert_reg(sch_ofs(qlist[i]), qdma_set_sel(orig_sch[i], LAN_SCHED), &last_sch[i]);
	half = qdma_sched_half(orig_tx_sch_rate[0] >> 16, man, exp);
	w0 = qdma_sched_word(rd(QDMA_TX_SCH_RATE), LAN_SCHED, half);
	assert_reg(QDMA_TX_SCH_RATE, w0, &last_w0);
	if (!capped)
		pr_info("qdma_rl: scheduler %d cap on %d queues, %d Mbit/s (man %u exp %u)\n",
			LAN_SCHED, nq, mbps, man, exp);
	capped = true;
	writes++;
	return 0;
}

/* RTNL held. */
static int apply_rate(int mbps)
{
	u32 val = 0, man, exp = 0;
	int i, err;

	if (!saved)
		return -ENODEV;
	if (mbps == 0) {
		restore_all("rate_mbps=0");
		return 0;
	}
	err = page_ok();
	if (err)
		return err;
	if (!strcmp(mode_str, "scheduler"))
		return apply_sched(mbps);

	encode(mbps, &man, &exp);
	for (i = 0; i < nq; i++) {
		val = orig_sch[i] & ~MAX_RATE_FIELDS;	/* keeps TX_SEL_V2, min rate, weight */
		if (!FIELD_GET(MAX_RATE_WEIGHT, val))
			val |= FIELD_PREP(MAX_RATE_WEIGHT, 10); /* driver uses 10 for its 1G setting */
		val |= MAX_RATE_EN | FIELD_PREP(MAX_RATE_MAN, man) | FIELD_PREP(MAX_RATE_EXP, exp);
		assert_reg(sch_ofs(qlist[i]), val, &last_sch[i]);	/* re-asserted on every write */
	}
	if (!capped)
		pr_info("qdma_rl: cap on %d queues, %d Mbit/s (%llu kbit/s encoded)\n",
			nq, mbps, val_rate_kbps(val));
	capped = true;
	writes++;
	return 0;
}

static void wd_fn(struct work_struct *w)
{
	if (watchdog_ms <= 0)	/* disabled after this work was queued */
		return;
	rtnl_lock();
	if (capped) {
		watchdog_trips++;
		pr_warn("qdma_rl: watchdog: no rate write for %d ms, restoring originals\n", watchdog_ms);
		restore_all("watchdog");
	}
	rtnl_unlock();
}

static int parse_queues(const char *s, int *out, int *n)
{
	char buf[64], *p = buf, *tok;
	int q, cnt = 0, err;
	u32 seen = 0;

	strscpy(buf, s, sizeof(buf));
	p = strim(buf);
	while ((tok = strsep(&p, ",")) != NULL) {
		if (!*tok)
			continue;
		err = kstrtoint(tok, 0, &q);
		if (err)
			return err;
		if (q < 0 || q >= QDMA_NUM_QUEUES || q == 1 || (seen & BIT(q)))
			return -EINVAL;	/* queue 1 is the WAN (GMAC1) queue: never touched */
		seen |= BIT(q);
		out[cnt++] = q;
	}
	if (!cnt)
		return -EINVAL;
	*n = cnt;
	return 0;
}

static int queues_set(const char *s, const struct kernel_param *kp)
{
	int tmp[QDMA_NUM_QUEUES], n, err = parse_queues(s, tmp, &n);

	if (err)
		return err;
	if (base) {
		rtnl_lock();
		if (capped) {
			rtnl_unlock();
			return -EBUSY;	/* set rate_mbps=0 first */
		}
	}
	if (!strcmp(mode_str, "scheduler")) {
		int i;

		for (i = 0; i < n; i++)
			if (!(LAN_QUEUE_MASK & BIT(tmp[i]))) {
				if (base)
					rtnl_unlock();
				return -EINVAL;
			}
	}
	memcpy(qlist, tmp, sizeof(tmp));
	nq = n;
	strscpy(queues_str, s, sizeof(queues_str));
	strreplace(queues_str, '\n', '\0');
	if (base) {
		save_all();
		rtnl_unlock();
	}
	return 0;
}

static int queues_get(char *buf, const struct kernel_param *kp)
{
	return sysfs_emit(buf, "%s\n", queues_str);
}

static int mode_set(const char *s, const struct kernel_param *kp)
{
	char m[16];
	int err = 0;

	strscpy(m, s, sizeof(m));
	strim(m);
	if (strcmp(m, "queue") && strcmp(m, "scheduler"))
		return -EINVAL;
	if (base)
		rtnl_lock();
	if (capped && strcmp(m, mode_str))
		err = -EBUSY;
	else if (!strcmp(m, "scheduler") && !lan_only())
		err = -EINVAL;	/* scheduler mode: LAN queues only */
	else
		strscpy(mode_str, m, sizeof(mode_str));
	if (base)
		rtnl_unlock();
	return err;
}

static int mode_get(char *buf, const struct kernel_param *kp)
{
	return sysfs_emit(buf, "%s\n", mode_str);
}

static int rate_set(const char *s, const struct kernel_param *kp)
{
	int r, err = kstrtoint(s, 0, &r);

	if (err)
		return err;
	if (r < 0 || r > 10000)
		return -EINVAL;
	if (!base)
		return -ENODEV;	/* rates are only accepted at runtime */
	rtnl_lock();
	err = apply_rate(r);
	if (!err)
		rate_mbps = r;
	rtnl_unlock();
	if (!err && r && watchdog_ms > 0)
		mod_delayed_work(system_wq, &wd_work, msecs_to_jiffies(watchdog_ms));
	return err;
}

static int reg_get(char *buf, const struct kernel_param *kp)
{
	int i, len = 0;
	u32 v;

	if (!base)
		return sysfs_emit(buf, "unmapped\n");
	len += sysfs_emit_at(buf, len, "mode=%s capped=%d rate_mbps=%d writes=%lu corrections=%lu watchdog_ms=%d trips=%lu page=%u\n",
			     mode_str, capped, rate_mbps, writes, corrections, watchdog_ms, watchdog_trips, rd(QDMA_PAGE));
	for (i = 0; i < nq; i++) {
		v = rd(sch_ofs(qlist[i]));
		len += sysfs_emit_at(buf, len,
			"q%d off=0x%x val=0x%08x orig=0x%08x sel=%lu max_en=%lu man=%lu exp=%lu weight=%lu max_kbps=%llu\n",
			qlist[i], sch_ofs(qlist[i]), v, orig_sch[i], FIELD_GET(TX_SEL_V2, v),
			FIELD_GET(MAX_RATE_EN, v), FIELD_GET(MAX_RATE_MAN, v), FIELD_GET(MAX_RATE_EXP, v),
			FIELD_GET(MAX_RATE_WEIGHT, v), val_rate_kbps(v));
	}
	for (i = 0; i < QDMA_TX_SCH_WORDS; i++) {
		int h;

		v = rd(QDMA_TX_SCH_RATE + 4 * i);
		len += sysfs_emit_at(buf, len, "tx_sch_rate[%d] off=0x%x val=0x%08x orig=0x%08x", i,
				     QDMA_TX_SCH_RATE + 4 * i, v, orig_tx_sch_rate[i]);
		for (h = 0; h < 2; h++) {
			u32 x = (v >> (16 * h)) & SCH_HALF;
			u64 r = FIELD_GET(SCH_RATE_MAN, x);
			u32 e = FIELD_GET(SCH_RATE_EXP, x);

			while (e--)
				r *= 10;
			len += sysfs_emit_at(buf, len, " sch%d:en=%lu,wfq=%lu,kbps=%llu", 2 * i + h,
					     FIELD_GET(SCH_RATE_EN, x), FIELD_GET(SCH_MAX_WFQ, x), r);
		}
		len += sysfs_emit_at(buf, len, "\n");
	}
	return len;
}

/* Read-only dump of every queue's QTX_SCH (e.g. the WAN queue 1); nothing saved or written. */
static int allq_get(char *buf, const struct kernel_param *kp)
{
	int q, len = 0;
	u32 v;

	if (!base)
		return sysfs_emit(buf, "unmapped\n");
	for (q = 0; q < QDMA_NUM_QUEUES; q++) {
		v = rd(sch_ofs(q));
		len += sysfs_emit_at(buf, len, "q%d val=0x%08x sel=%lu max_en=%lu max_kbps=%llu\n", q, v,
				     FIELD_GET(TX_SEL_V2, v), FIELD_GET(MAX_RATE_EN, v), val_rate_kbps(v));
	}
	return len;
}

static const struct kernel_param_ops queues_ops = { .set = queues_set, .get = queues_get };
static const struct kernel_param_ops mode_ops = { .set = mode_set, .get = mode_get };
static const struct kernel_param_ops rate_ops = { .set = rate_set, .get = param_get_int };
static const struct kernel_param_ops reg_ops = { .get = reg_get };
static const struct kernel_param_ops allq_ops = { .get = allq_get };

module_param_cb(queues, &queues_ops, NULL, 0644);
MODULE_PARM_DESC(queues, "comma list of QDMA TX queues to cap (default 3,4,5,6,8 = LAN ports; 1 refused)");
module_param_cb(mode, &mode_ops, NULL, 0644);
MODULE_PARM_DESC(mode, "queue (per-queue max rate) | scheduler (LAN queues -> scheduler 1, scheduler 1 rate-limited)");
module_param_cb(rate_mbps, &rate_ops, &rate_mbps, 0644);
MODULE_PARM_DESC(rate_mbps, "max rate in Mbit/s for every listed queue, 0 = restore all saved registers");
module_param(watchdog_ms, int, 0644);
MODULE_PARM_DESC(watchdog_ms, "restore originals if no rate write for this long while capped (0 = off)");
module_param_cb(reg, &reg_ops, NULL, 0444);
MODULE_PARM_DESC(reg, "live values of every register this module touches");
module_param_cb(allq, &allq_ops, NULL, 0444);
MODULE_PARM_DESC(allq, "read-only dump of QTX_SCH for all 16 queues");

static int __init qdma_rl_init(void)
{
	struct device_node *np;

	if (!nq && parse_queues(queues_str, qlist, &nq))
		return -EINVAL;
	np = of_find_compatible_node(NULL, NULL, "mediatek,mt7986-eth");
	if (!np)
		return -ENODEV;
	base = of_iomap(np, 0);	/* the driver owns the region; map without requesting it */
	of_node_put(np);
	if (!base)
		return -ENOMEM;
	INIT_DELAYED_WORK(&wd_work, wd_fn);
	rtnl_lock();
	save_all();
	rtnl_unlock();
	return 0;
}

static void __exit qdma_rl_exit(void)
{
	cancel_delayed_work_sync(&wd_work);
	rtnl_lock();
	if (capped)
		restore_all("rmmod");
	rtnl_unlock();
	iounmap(base);
}

module_init(qdma_rl_init);
module_exit(qdma_rl_exit);
MODULE_LICENSE("GPL");
MODULE_VERSION("3");
MODULE_DESCRIPTION("MT7986 QDMA per-queue max-rate knob with save/restore and watchdog");
