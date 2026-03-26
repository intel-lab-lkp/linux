// SPDX-License-Identifier: GPL-2.0+
/*
 * Floating-point number to integer conversions
 *
 * Currently, only float (binary32) is supported.
 *
 * Copyright (c) 2026 Rong Zhang <i@rong.moe>
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/bits.h>
#include <linux/bitfield.h>
#include <linux/bitops.h>
#include <linux/compiler.h>
#include <linux/types.h>
#include <linux/limits.h>
#include <linux/minmax.h>
#include <linux/module.h>

#include "hwmon-fp.h"

#define FLOAT_SIGN_MASK			BIT(31)
#define FLOAT_EXPONENT_MASK		GENMASK(30, 23)
#define FLOAT_MANTISSA_MASK		GENMASK(22, 0)
#define FLOAT_EXPONENT_OFFSET		127
#define FLOAT_FRACTION_Q		23
#define FLOAT_IMPLICIT_BIT		BIT(23)

#define HWMON_FP_SCALE_MAGNIFY_SHIFT_L	6

struct float_struct {
	u32 significand;
	s16 shift_r;		/* = Q - exponent */
	bool sign;
	bool inf;		/* See below. */
};

/*
 * The sign of a floating-point number carries significant information,
 * return a saturated value for infinity so its sign is retained.
 */
static inline int hwmon_fp_infinity_to_s64(bool sign, s64 *val)
{
	*val = sign ? S64_MIN : S64_MAX;
	return 0;
}

static int hwmon_fp_parse_float(u32 flt, struct float_struct *fs)
{
	u32 mantissa = FIELD_GET(FLOAT_MANTISSA_MASK, flt);
	u8 exponent = FIELD_GET(FLOAT_EXPONENT_MASK, flt);
	bool sign = FIELD_GET(FLOAT_SIGN_MASK, flt);

	if (unlikely(exponent == FLOAT_EXPONENT_MASK >> FLOAT_FRACTION_Q)) {
		if (mantissa != 0) /* NaN */
			return -EINVAL;

		/* Infinity */
		fs->significand = HWMON_FP_FLOAT_SIGNIFICAND_MAX; /* Distinguish from fs(zero). */
		fs->shift_r = 0;
		fs->sign = sign;
		fs->inf = true;

		return 0;
	}

	fs->sign = sign;
	fs->inf = false;

	if (likely(exponent != 0)) {
		/* Normal */
		fs->significand = (FLOAT_IMPLICIT_BIT | mantissa);
		fs->shift_r = FLOAT_FRACTION_Q - (exponent - FLOAT_EXPONENT_OFFSET);
	} else if (unlikely(mantissa != 0)) {	/* exponent == 0 && mantissa != 0 */
		/* Subnormal */
		fs->significand = mantissa;
		fs->shift_r = FLOAT_FRACTION_Q - (1 - FLOAT_EXPONENT_OFFSET);
	} else {				/* exponent == 0 && mantissa == 0 */
		/* Zero */
		fs->significand = 0; /* Only fs(zero) has fs->significand == 0. */
		fs->shift_r = 0;
	}

	return 0;
}

static int hwmon_fp_raw_to_s64(u64 significand, int shift_r, bool sign, s64 *val)
{
	u64 temp;

	if (unlikely(shift_r >= 64) || significand == 0) {
		*val = 0;
		return 0;
	}

	if (shift_r < 0) {
		/*
		 * Left shift:
		 *
		 *   (significand * 2^-Q) * 2^exponent
		 * = significand * 2^(exponent - Q)
		 * = significand * 2^-shift_r
		 * = significand << -shift_r
		 */
		shift_r = -shift_r;
		temp = significand << shift_r;

		if (unlikely(temp >> shift_r != significand))
			return hwmon_fp_infinity_to_s64(sign, val);
	} else if (shift_r == 0) {
		temp = significand;
	} else { /* shift_r > 0 */
		/*
		 * Right shift:
		 *
		 *   (significand * 2^-Q) * 2^exponent
		 * = significand / 2^(Q - exponent)
		 * = significand / 2^shift_r
		 * = significand >> shift_r
		 */
		temp = significand >> shift_r;

		/* Round to nearest. */
		temp += !!(significand & BIT_U64(shift_r - 1));
	}

	if (unlikely((s64)temp < 0))
		return hwmon_fp_infinity_to_s64(sign, val);

	*val = (sign ? -1 : 1) * (s64)temp;
	return 0;
}

static int __hwmon_fp_float_to_s64_unsafe(const struct float_struct *fs, u32 scale, s64 *val)
{
	if (unlikely(fs->inf))
		return hwmon_fp_infinity_to_s64(fs->sign, val);

	return hwmon_fp_raw_to_s64((u64)scale * (u64)fs->significand,
				   fs->shift_r, fs->sign, val);
}

int hwmon_fp_float_to_s64_unsafe(u32 flt, u32 scale, s64 *val)
{
	struct float_struct fs;
	int ret;

	ret = hwmon_fp_parse_float(flt, &fs);
	if (ret)
		return ret;

	return __hwmon_fp_float_to_s64_unsafe(&fs, scale, val);
}
EXPORT_SYMBOL_GPL(hwmon_fp_float_to_s64_unsafe);

static int __hwmon_fp_mul_to_s64_unsafe(const struct float_struct *fs1,
					const struct float_struct *fs2,
					u32 scale_ntz, ulong scale_ctz, s64 *val)
{
	bool sign = fs1->sign ^ fs2->sign;
	u64 scaled_significand;
	int shift_r;

	if (unlikely((fs1->inf && fs2->significand == 0) || (fs1->significand == 0 && fs2->inf)))
		return -EINVAL;

	if (unlikely(fs1->inf || fs2->inf))
		return hwmon_fp_infinity_to_s64(sign, val);

	if (fs1->significand == 0 || fs2->significand == 0) {
		*val = 0;
		return 0;
	}

	/*
	 *   scale_ntz * 2^scale_ctz * significand1 * 2^-shift_r1 * significand2 * 2^-shift_r2
	 * = scale_ntz * significand1 * significand2 * 2^-(shift_r1 + shift_r2 - scale_ctz)
	 * = (scale_ntz * significand1 * significand2) >> (shift_r1 + shift_r2 - scale_ctz)
	 */
	scaled_significand = (u64)scale_ntz * (u64)fs1->significand * (u64)fs2->significand;
	shift_r = fs1->shift_r + fs2->shift_r - scale_ctz;

	return hwmon_fp_raw_to_s64(scaled_significand, shift_r, sign, val);
}

int hwmon_fp_mul_to_s64_unsafe(u32 flt1, u32 flt2, u32 scale, ulong scale_ctz, s64 *val)
{
	struct float_struct fs1, fs2;
	int ret;

	ret = hwmon_fp_parse_float(flt1, &fs1) || hwmon_fp_parse_float(flt2, &fs2);
	if (ret)
		return ret;

	return __hwmon_fp_mul_to_s64_unsafe(&fs1, &fs2, scale, scale_ctz, val);
}
EXPORT_SYMBOL_GPL(hwmon_fp_mul_to_s64_unsafe);

static int __hwmon_fp_div_to_s64_unsafe(const struct float_struct *fs1,
					const struct float_struct *fs2,
					u32 scale, bool div0_ok, s64 *val)
{
	bool sign = fs1->sign ^ fs2->sign;
	u64 scaled_significand;
	int shift_r;

	if (unlikely(fs1->inf && fs2->inf))
		return -EINVAL;

	if (fs2->significand == 0) {
		if (div0_ok) {
			*val = 0;
			return 0;
		}
		return -EINVAL;
	}

	if (unlikely(fs1->inf))
		return hwmon_fp_infinity_to_s64(sign, val);

	if (unlikely(fs2->inf) || fs1->significand == 0) {
		*val = 0;
		return 0;
	}

	/*
	 * Make the dividend as large as possible to improve accuracy, otherwise
	 * the divide-and-right-shift procedure may produce an inaccurate result.
	 *
	 *   scale * (significand1 * 2^-shift_r1) / (significand2 * 2^-shift_r2)
	 * = scale * 2^6 * 2^-6 * (significand1 * 2^-shift_r1) / (significand2 * 2^-shift_r2)
	 * = (((scale * 2^6) * significand1) / significand2) * 2^-(shift_r1 - shift_r2 + 6)
	 * = (((scale << 6) * significand1) / significand2) >> (shift_r1 - shift_r2 + 6)
	 *
	 * This will never overflow: (2^32 - 1) * 2^6 * (2^24 - 1) < (2^62 - 1).
	 */
	scaled_significand = ((u64)scale << HWMON_FP_SCALE_MAGNIFY_SHIFT_L) * (u64)fs1->significand;
	scaled_significand =
		(scaled_significand + (u64)fs2->significand / 2) / (u64)fs2->significand;

	shift_r = fs1->shift_r - fs2->shift_r + HWMON_FP_SCALE_MAGNIFY_SHIFT_L;

	return hwmon_fp_raw_to_s64(scaled_significand, shift_r, sign, val);
}

int hwmon_fp_div_to_s64_unsafe(u32 flt1, u32 flt2, u32 scale, bool div0_ok, s64 *val)
{
	struct float_struct fs1, fs2;
	int ret;

	ret = hwmon_fp_parse_float(flt1, &fs1) || hwmon_fp_parse_float(flt2, &fs2);
	if (ret)
		return ret;

	return __hwmon_fp_div_to_s64_unsafe(&fs1, &fs2, scale, div0_ok, val);
}
EXPORT_SYMBOL_GPL(hwmon_fp_div_to_s64_unsafe);

MODULE_AUTHOR("Rong Zhang <i@rong.moe>");
MODULE_DESCRIPTION("hwmon floating-point number to integer conversions");
MODULE_LICENSE("GPL");
