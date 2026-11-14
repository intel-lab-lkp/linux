// SPDX-License-Identifier: GPL-2.0-only
/*
 * Tuning tables of the RKVENC v1 encoder. Tables with a closed form are
 * computed once at module load in integer fixed-point arithmetic.
 */

#include <linux/build_bug.h>
#include <linux/cache.h>
#include <linux/math64.h>

#include "rkvenc-vepu541.h"

/* Fraction bits of the fixed-point numbers; the largest value is below 2^8 */
#define RKVENC_FX_BITS 56
#define RKVENC_FX_ONE (1ULL << RKVENC_FX_BITS)
#define RKVENC_FX_TWO (2ULL << RKVENC_FX_BITS)

/* Weights and lambdas grow by 2^(1/3) per step, sixths give square roots */
#define RKVENC_STEPS_PER_OCTAVE 3
#define RKVENC_SIXTHS_PER_OCTAVE 6
#define RKVENC_SIXTHS_PER_STEP 2

/* H.264 weight k = floor(16 * 2^(k / 3)); 16 is the neutral weight */
#define RKVENC_H264_KLUT_NEUTRAL 16

/* H.264 lambda j = floor(3.6 * 2^((j + 5) / 3)), the factor as 18 / 5 */
#define RKVENC_H264_LAMBDA_FACTOR_NUM 18
#define RKVENC_H264_LAMBDA_FACTOR_DEN 5
#define RKVENC_H264_LAMBDA_J_OFFSET 5

/* HEVC lambda 0.57 * 2^((q - 12) / 3), 11 fraction bits */
#define RKVENC_HEVC_LAMBDA_PCT 57
#define RKVENC_HEVC_LAMBDA_QP_REF 12
#define RKVENC_HEVC_LAMBDA_FRAC 11
#define RKVENC_PERCENT 100

/* HEVC weight i = S[i mod 3] * 2^(i div 3), S about 2^(7 + n / 3) */
#define RKVENC_HEVC_KLUT_S0 128
#define RKVENC_HEVC_KLUT_S1 161
#define RKVENC_HEVC_KLUT_S2 204
#define RKVENC_HEVC_LAMBDA_B_S0 112
#define RKVENC_HEVC_LAMBDA_B_S1 137
#define RKVENC_HEVC_LAMBDA_B_S2 176
#define RKVENC_KLUT_ENTRY_BITS 18

/* Three KLUT weights per word pair, the middle one split across both */
#define RKVENC_KLUT_PAIRS (RKVENC_KLUT_WORDS / 2)
#define RKVENC_KLUT_EVEN_WORD(n) (2 * (n))
#define RKVENC_KLUT_ODD_WORD(n) (2 * (n) + 1)
#define RKVENC_KLUT_SPLIT_BITS 9
#define RKVENC_KLUT_SPLIT_EVEN_SHIFT 23
#define RKVENC_KLUT_LAST_ODD_SHIFT 14

static_assert(RKVENC_SIXTHS_PER_STEP * RKVENC_STEPS_PER_OCTAVE ==
	      RKVENC_SIXTHS_PER_OCTAVE);
static_assert(RKVENC_HEVC_LAMBDA_QP_REF % RKVENC_SIXTHS_PER_OCTAVE == 0);

u32 rkvenc_h264_klut[RKVENC_KLUT_WORDS] __ro_after_init;
u32 rkvenc_hevc_klut[RKVENC_KLUT_WORDS] __ro_after_init;
u32 rkvenc_h264_lambda[RKVENC_QP_MAX + 2] __ro_after_init;
u32 rkvenc_hevc_lambda_satd[RKVENC_QP_MAX + 1] __ro_after_init;
u32 rkvenc_hevc_lambda_a[RKVENC_QP_MAX + 1] __ro_after_init;
u32 rkvenc_hevc_lambda_b[RKVENC_QP_MAX + 1] __ro_after_init;

typedef u64 (*rkvenc_fx_pow_t)(u64 x);

static u64 fx_mul(u64 a, u64 b)
{
	return mul_u64_u64_shr(a, b, RKVENC_FX_BITS);
}

static u64 fx_pow2(u64 x)
{
	return fx_mul(x, x);
}

static u64 fx_pow6(u64 x)
{
	u64 x3 = fx_mul(fx_pow2(x), x);

	return fx_mul(x3, x3);
}

/* Largest x below 2.0 whose power does not exceed target, by bisection */
static u64 fx_root(rkvenc_fx_pow_t pow, u64 target)
{
	u64 lo = 0, hi = RKVENC_FX_TWO, mid;

	while (lo < hi) {
		mid = (lo + hi + 1) / 2;
		if (pow(mid) <= target)
			lo = mid;
		else
			hi = mid - 1;
	}

	return lo;
}

/* x * 2^exp, rounded down or to nearest */
static u32 fx_floor(u64 x, int exp)
{
	return x >> (RKVENC_FX_BITS - exp);
}

static u32 fx_round(u64 x, int exp)
{
	unsigned int s = RKVENC_FX_BITS - exp;

	return (x + (1ULL << (s - 1))) >> s;
}

static void klut_pack(u32 *words, const u32 *weights)
{
	u32 n, e0, e1, e2, low, i = 0;

	for (n = 0; n < RKVENC_KLUT_PAIRS; n++) {
		e0 = weights[i++];
		e1 = i < RKVENC_KLUT_ENTRIES ? weights[i++] : 0;
		e2 = i < RKVENC_KLUT_ENTRIES ? weights[i++] : 0;
		low = e1 & GENMASK(RKVENC_KLUT_SPLIT_BITS - 1, 0);
		words[RKVENC_KLUT_EVEN_WORD(n)] =
			e0 | low << RKVENC_KLUT_SPLIT_EVEN_SHIFT;
		words[RKVENC_KLUT_ODD_WORD(n)] =
			e1 >> RKVENC_KLUT_SPLIT_BITS |
			e2 << RKVENC_KLUT_LAST_ODD_SHIFT;
	}
}

static void h264_klut_init(const u64 *step_frac)
{
	u32 weights[RKVENC_KLUT_ENTRIES];
	u64 x;
	u32 k;

	for (k = 0; k < RKVENC_KLUT_ENTRIES; k++) {
		x = RKVENC_H264_KLUT_NEUTRAL *
		    step_frac[k % RKVENC_STEPS_PER_OCTAVE];
		weights[k] = fx_floor(x, k / RKVENC_STEPS_PER_OCTAVE);
	}

	klut_pack(rkvenc_h264_klut, weights);
}

static void hevc_klut_init(void)
{
	static const u16 s[RKVENC_STEPS_PER_OCTAVE] = { RKVENC_HEVC_KLUT_S0,
							RKVENC_HEVC_KLUT_S1,
							RKVENC_HEVC_KLUT_S2 };
	u32 weights[RKVENC_KLUT_ENTRIES];
	u64 v;
	u32 i;

	for (i = 0; i < RKVENC_KLUT_ENTRIES; i++) {
		v = (u64)s[i % RKVENC_STEPS_PER_OCTAVE]
		    << (i / RKVENC_STEPS_PER_OCTAVE);
		weights[i] =
			min_t(u64, v, GENMASK(RKVENC_KLUT_ENTRY_BITS - 1, 0));
	}

	klut_pack(rkvenc_hevc_klut, weights);
}

static void h264_lambda_init(const u64 *step_frac)
{
	u32 j, m;
	u64 x;

	for (j = 0; j < ARRAY_SIZE(rkvenc_h264_lambda); j++) {
		m = j + RKVENC_H264_LAMBDA_J_OFFSET;
		x = mul_u64_u32_div(step_frac[m % RKVENC_STEPS_PER_OCTAVE],
				    RKVENC_H264_LAMBDA_FACTOR_NUM,
				    RKVENC_H264_LAMBDA_FACTOR_DEN);
		rkvenc_h264_lambda[j] =
			fx_floor(x, m / RKVENC_STEPS_PER_OCTAVE);
	}
}

/* The SATD lambda is the square root, sqrt(0.57) * 2^((q - 12) / 6) */
static void hevc_lambda_init(const u64 *sixth, u64 sqrt_factor)
{
	u32 pct = (RKVENC_HEVC_LAMBDA_PCT << RKVENC_HEVC_LAMBDA_FRAC) >>
		  (RKVENC_HEVC_LAMBDA_QP_REF / RKVENC_STEPS_PER_OCTAVE);
	int exp = RKVENC_HEVC_LAMBDA_FRAC -
		  RKVENC_HEVC_LAMBDA_QP_REF / RKVENC_SIXTHS_PER_OCTAVE;
	static const u16 b[RKVENC_STEPS_PER_OCTAVE] = {
		RKVENC_HEVC_LAMBDA_B_S0, RKVENC_HEVC_LAMBDA_B_S1,
		RKVENC_HEVC_LAMBDA_B_S2
	};
	u32 q, r;
	u64 x;

	for (q = 0; q <= RKVENC_QP_MAX; q++) {
		r = q % RKVENC_STEPS_PER_OCTAVE;
		x = mul_u64_u32_div(sixth[RKVENC_SIXTHS_PER_STEP * r], pct,
				    RKVENC_PERCENT);
		rkvenc_hevc_lambda_a[q] =
			fx_round(x, q / RKVENC_STEPS_PER_OCTAVE);

		x = fx_mul(sqrt_factor, sixth[q % RKVENC_SIXTHS_PER_OCTAVE]);
		rkvenc_hevc_lambda_satd[q] =
			fx_round(x, q / RKVENC_SIXTHS_PER_OCTAVE + exp);

		rkvenc_hevc_lambda_b[q] = b[r] << (q / RKVENC_STEPS_PER_OCTAVE);
	}
}

void __init rkvenc_tables_init(void)
{
	u64 sixth[RKVENC_SIXTHS_PER_OCTAVE];
	u64 step_frac[RKVENC_STEPS_PER_OCTAVE];
	u64 factor, c, sqrt_factor;
	u32 r;

	c = fx_root(fx_pow6, RKVENC_FX_TWO);
	sixth[0] = RKVENC_FX_ONE;
	for (r = 1; r < ARRAY_SIZE(sixth); r++)
		sixth[r] = fx_mul(sixth[r - 1], c);

	for (r = 0; r < ARRAY_SIZE(step_frac); r++)
		step_frac[r] = sixth[RKVENC_SIXTHS_PER_STEP * r];

	factor =
		div_u64(RKVENC_HEVC_LAMBDA_PCT * RKVENC_FX_ONE, RKVENC_PERCENT);
	sqrt_factor = fx_root(fx_pow2, factor);

	h264_klut_init(step_frac);
	hevc_klut_init();
	h264_lambda_init(step_frac);
	hevc_lambda_init(sixth, sqrt_factor);
}

/* Values without a closed form */
const u8 rkvenc_h264_aq_thd[16] = {
	0, 0, 0, 0, 3, 3, 5, 5, 8, 8, 8, 15, 15, 20, 25, 25,
};

const s8 rkvenc_h264_aq_step_i[16] = {
	-8, -7, -6, -5, -4, -3, -2, -1, 0, 1, 3, 3, 4, 5, 8, 8,
};

const s8 rkvenc_h264_aq_step_p[16] = {
	-8, -7, -6, -5, -4, -3, -2, -1, 0, 1, 2, 3, 4, 5, 7, 8,
};

const u8 rkvenc_hevc_aq_thd[16] = {
	0, 0, 0, 0, 3, 3, 5, 5, 8, 8, 8, 15, 15, 20, 25, 35,
};

const s8 rkvenc_hevc_aq_step[16] = {
	-8, -7, -6, -5, -4, -3, -2, -1, 0, 1, 2, 3, 4, 5, 7, 8,
};

const u16 rkvenc_hevc_intra_thd[4] = { 2, 6, 16, 36 };
const u8 rkvenc_hevc_intra_wgt32[7] = { 23, 22, 21, 20, 22, 24, 26 };
const u8 rkvenc_hevc_intra_wgt16[7] = { 17, 17, 17, 18, 17, 18, 18 };
