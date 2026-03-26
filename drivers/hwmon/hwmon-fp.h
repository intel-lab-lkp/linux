/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Floating-point number to integer conversions
 *
 * Copyright (c) 2026 Rong Zhang <i@rong.moe>
 */

#ifndef __HWMON_FP_H__
#define __HWMON_FP_H__

#include <asm/bitsperlong.h>
#include <linux/bug.h>
#include <linux/build_bug.h>
#include <linux/compiler.h>
#include <linux/limits.h>
#include <linux/types.h>
#include <linux/units.h>

int hwmon_fp_float_to_s64_unsafe(u32 flt, u32 scale, s64 *val);
int hwmon_fp_mul_to_s64_unsafe(u32 flt1, u32 flt2, u32 scale_ntz, ulong scale_ctz, s64 *val);
int hwmon_fp_div_to_s64_unsafe(u32 flt1, u32 flt2, u32 scale, bool div0_ok, s64 *val);

#define HWMON_FP_FLOAT_SIGNIFICAND_BITS	24
#define HWMON_FP_FLOAT_SIGNIFICAND_MAX	BIT(HWMON_FP_FLOAT_SIGNIFICAND_BITS)

#define HWMON_FP_MUL_SCALE_MAX		BIT(64 - HWMON_FP_FLOAT_SIGNIFICAND_BITS * 2)

static inline int __hwmon_fp_check_scale(u32 scale)
{
	return WARN_ON(scale <= 0) ? -EINVAL : 0;
}

#define hwmon_fp_check_scale(scale)				\
	__builtin_choose_expr(__is_constexpr(scale),		\
			      BUILD_BUG_ON_ZERO((scale) <= 0),	\
			      __hwmon_fp_check_scale(scale))

#define HWMON_FP_SCALE_IN	MILLI	/* millivolt		 (mV) */
#define HWMON_FP_SCALE_TEMP	MILLI	/* millidegree Celsius	(m°C) */
#define HWMON_FP_SCALE_CURR	MILLI	/* milliampere		 (mA) */
#define HWMON_FP_SCALE_POWER	MICRO	/* microWatt		 (uW) */
#define HWMON_FP_SCALE_ENERGY	MICRO	/* microJoule		 (uJ) */

/**
 * hwmon_fp_float_to_s64() - Convert a float (binary32) number into a signed
 * 64-bit integer.
 * @flt:	Float (binary32) number.
 * @scale:	Scale factor.
 * @val:	Pointer to store the converted value.
 *
 * Special case:
 *             inf -> S64_MAX or S64_MIN
 *             NaN -> -EINVAL;
 *      (overflow) -> S64_MAX or S64_MIN
 *     (underflow) -> 0
 *
 * Return: 0 on success, or an error code.
 */
#define hwmon_fp_float_to_s64(flt, scale, val)		\
	(hwmon_fp_check_scale(scale) ||			\
	 hwmon_fp_float_to_s64_unsafe(flt, scale, val))

/*
 * Handling multification is very tricky, as large scale factors must not lead
 * to overflow. Fortunately, cutting off all trailing zeros and restoring them
 * while right shifting is enough reduce the scale factor used in
 * multiplication to a small enough value.
 */
static inline int __hwmon_fp_mul_to_s64(u32 flt1, u32 flt2, u32 scale, s64 *val)
{
	ulong scale_ctz;

	if (WARN_ON(scale <= 0))
		return -EINVAL;

	scale_ctz = __ffs(scale);
	scale >>= scale_ctz;

	if (WARN_ON(scale >= HWMON_FP_MUL_SCALE_MAX))
		return -EINVAL;

	return hwmon_fp_mul_to_s64_unsafe(flt1, flt2, scale, scale_ctz, val);
}

#define __hwmon_fp_mul_to_s64_const(flt1, flt2, scale, val)			\
({										\
	u32 _scale_ntz = (scale);						\
	ulong _scale_ctz;							\
										\
	BUILD_BUG_ON(_scale_ntz <= 0);						\
										\
	_scale_ctz = __builtin_ctzl(_scale_ntz);				\
	_scale_ntz >>= _scale_ctz;						\
										\
	BUILD_BUG_ON(_scale_ntz >= HWMON_FP_MUL_SCALE_MAX);			\
										\
	hwmon_fp_mul_to_s64_unsafe(flt1, flt2, _scale_ntz, _scale_ctz, val);	\
})

/**
 * hwmon_fp_mul_to_s64() - Multiply two float (binary32) numbers and convert the
 * product into a signed 64-bit integer.
 * @flt1:	Multiplicand stored in float (binary32) format.
 * @flt2:	Multiplier stored in float (binary32) format.
 * @scale:	Scale factor.
 * @val:	Pointer to store the product.
 *
 * Calculate @scale * @flt1 * @flt2.
 *
 * Special case:
 *     0 * inf -> -EINVAL
 *     x * inf -> S64_MAX or S64_MIN
 *     x *   0 -> 0
 *
 * Return: 0 on success, or an error code.
 */
#define hwmon_fp_mul_to_s64(flt1, flt2, scale, val)			\
	__builtin_choose_expr(__is_constexpr(scale),			\
		__hwmon_fp_mul_to_s64_const(flt1, flt2, scale, val),	\
		__hwmon_fp_mul_to_s64(flt1, flt2, scale, val))

/**
 * hwmon_fp_div_to_s64() - Divide two float (binary32) numbers and convert the
 * quotient into a signed 64-bit integer.
 * @flt1:	Dividend stored in float (binary32) format.
 * @flt2:	Divisor stored in float (binary32) format.
 * @scale:	Scale factor.
 * @div0_ok:	If true, return 0 when @flt2 is 0. Otherwise, -EINVAL is returned.
 * @val:	Pointer to store the quotient.
 *
 * Calculate @scale * (@flt1 / @flt2).
 *
 * Special case:
 *     inf / inf -> -EINVAL
 *     inf /   x -> S64_MAX or S64_MIN
 *       x /   0 -> See div0_ok
 *       x / inf -> 0
 *       0 /   x -> 0
 *
 * Return: 0 on success, or an error code.
 */
#define hwmon_fp_div_to_s64(flt1, flt2, scale, div0_ok, val)		\
	(hwmon_fp_check_scale(scale) ||					\
	 hwmon_fp_div_to_s64_unsafe(flt1, flt2, scale, div0_ok, val))

#if BITS_PER_LONG == 64

static_assert(sizeof(long) == sizeof(s64));

static inline long hwmon_fp_s64_to_long(s64 val64)
{
	return val64;
}

# define hwmon_fp_float_to_long(flt, scale, val) \
	hwmon_fp_float_to_s64(flt, scale, (s64 *)val)
# define hwmon_fp_div_to_long(flt1, flt2, scale, div0_ok, val) \
	hwmon_fp_div_to_s64(flt1, flt2, scale, div0_ok, (s64 *)val)
# define hwmon_fp_mul_to_long(flt1, flt2, scale, val) \
	hwmon_fp_mul_to_s64(flt1, flt2, scale, (s64 *)val)

#else /* BITS_PER_LONG == 64 */

static inline long hwmon_fp_s64_to_long(s64 val64)
{
	if (unlikely(val64 > LONG_MAX))
		return LONG_MAX;
	else if (unlikely(val64 < LONG_MIN))
		return LONG_MIN;
	else
		return val64;
}

# define hwmon_fp_float_to_long(flt, scale, val)		\
({								\
	s64 _val64;						\
	int _ret;						\
								\
	_ret = hwmon_fp_float_to_s64(flt, scale, &_val64);	\
	if (!_ret)						\
		*(val) = hwmon_fp_s64_to_long(_val64);		\
								\
	_ret;							\
})

# define hwmon_fp_div_to_long(flt1, flt2, scale, div0_ok, val)			\
({										\
	s64 _val64;								\
	int _ret;								\
										\
	_ret = hwmon_fp_div_to_s64(flt1, flt2, scale, div0_ok, &_val64);	\
	if (!_ret)								\
		*(val) = hwmon_fp_s64_to_long(_val64);				\
										\
	_ret;									\
})

# define hwmon_fp_mul_to_long(flt1, flt2, scale, val)		\
({								\
	s64 _val64;						\
	int _ret;						\
								\
	_ret = hwmon_fp_mul_to_s64(flt1, flt2, scale, &_val64);	\
	if (!_ret)						\
		*(val) = hwmon_fp_s64_to_long(_val64);		\
								\
	_ret;							\
})

#endif /* BITS_PER_LONG == 64 */

#endif /* __HWMON_FP_H__ */
