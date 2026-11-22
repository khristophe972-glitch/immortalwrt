// SPDX-License-Identifier: GPL-2.0
/* Host self-test for qdma_enc.h: cc -o enc_test enc_test.c && ./enc_test */
#include <assert.h>
#include <stdio.h>
#include "qdma_enc.h"

int main(void)
{
	unsigned int man, exp, h;

	qdma_encode(300, &man, &exp); assert(man == 30 && exp == 4);
	qdma_encode(345, &man, &exp); assert(man == 34 && exp == 4);	/* rounds down */
	qdma_encode(100, &man, &exp); assert(man == 100 && exp == 3);
	qdma_encode(1000, &man, &exp); assert(man == 100 && exp == 4);
	qdma_encode(1, &man, &exp); assert(man == 100 && exp == 1);
	/* preflight tx_sch_rate 0x80008000: scheduler 1 at 200 Mbit/s, scheduler 0 half untouched */
	qdma_encode(200, &man, &exp);
	h = qdma_sched_half(0x8000, man, exp);
	assert(h == 0x8944);
	assert(qdma_sched_word(0x80008000, 1, h) == 0x89448000);
	assert(qdma_sched_word(0x80008000, 0, h) == 0x80008944);
	/* q6 preflight 0x3814a816 -> scheduler 1; per-queue fields kept */
	assert(qdma_set_sel(0x3814a816, 1) == 0x7814a816);
	assert(qdma_set_sel(0x7814a816, 0) == 0x3814a816);
	assert(qdma_set_sel(0x38140000, 1) == 0x78140000);
	puts("enc_test: all passed");
	return 0;
}
