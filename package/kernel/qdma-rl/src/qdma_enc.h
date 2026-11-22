/* SPDX-License-Identifier: GPL-2.0 */
/* Pure encoding helpers shared by qdma_rl.c and the host self-test (enc_test.c). */
#ifndef QDMA_ENC_H
#define QDMA_ENC_H

#define ENC_MAN_MAX	127u		/* 7-bit mantissa (MAX_RATE_MAN / SCH_RATE_MAN) */
#define ENC_HALF_WFQ	(1u << 15)	/* SCH_MAX_WFQ */
#define ENC_HALF_EN	(1u << 11)	/* SCH_RATE_EN */
#define ENC_SEL_SHIFT	30		/* TX_SEL_V2 = bits 31:30 */

/* largest MAN*10^EXP kbit/s <= mbps*1000 */
static inline void qdma_encode(unsigned int mbps, unsigned int *man, unsigned int *exp)
{
	unsigned long long kbps = (unsigned long long)mbps * 1000;

	*exp = 0;
	while (kbps > ENC_MAN_MAX) {
		kbps /= 10;
		(*exp)++;
	}
	*man = (unsigned int)kbps;
}

/* one scheduler's 16-bit half: keep the original MAX_WFQ, enable the rate */
static inline unsigned int qdma_sched_half(unsigned int orig_half, unsigned int man, unsigned int exp)
{
	return (orig_half & ENC_HALF_WFQ) | ENC_HALF_EN | (man << 4) | (exp & 0xf);
}

/* tx_sch_rate word with scheduler `id`'s half replaced (id & 1 selects the upper half) */
static inline unsigned int qdma_sched_word(unsigned int cur, int id, unsigned int half)
{
	int shift = (id & 1) ? 16 : 0;

	return (cur & ~(0xffffu << shift)) | (half << shift);
}

/* QTX_SCH with TX_SEL_V2 set to `sched`, everything else kept */
static inline unsigned int qdma_set_sel(unsigned int qtx_sch, unsigned int sched)
{
	return (qtx_sch & ~(3u << ENC_SEL_SHIFT)) | ((sched & 3u) << ENC_SEL_SHIFT);
}
#endif
