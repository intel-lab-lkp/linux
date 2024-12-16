/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __LINUX_BITS_H
#define __LINUX_BITS_H

#include <linux/const.h>
#include <vdso/bits.h>
#include <uapi/linux/bits.h>
#include <asm/bitsperlong.h>

#define BIT_MASK(nr)		(UL(1) << ((nr) % BITS_PER_LONG))
#define BIT_WORD(nr)		((nr) / BITS_PER_LONG)
#define BIT_ULL_MASK(nr)	(ULL(1) << ((nr) % BITS_PER_LONG_LONG))
#define BIT_ULL_WORD(nr)	((nr) / BITS_PER_LONG_LONG)
#define BITS_PER_BYTE		8

/*
 * Create a contiguous bitmask starting at bit position @lo and ending at
 * position @hi. For example
 * GENMASK_ULL(39, 21) gives us the 64bit vector 0x000000ffffe00000.
 */
#if !defined(__ASSEMBLY__)
#include <linux/build_bug.h>
#define GENMASK_INPUT_CHECK(hi, lo) \
	(BUILD_BUG_ON_ZERO(__builtin_choose_expr( \
		__is_constexpr((lo) > (hi)), (lo) > (hi), 0)))

#define GENMASK(hi, lo) \
	(GENMASK_INPUT_CHECK(hi, lo) + __GENMASK(hi, lo))
#define GENMASK_ULL(hi, lo) \
	(GENMASK_INPUT_CHECK(hi, lo) + __GENMASK_ULL(hi, lo))

#define GENMASK_U128(hi, lo) \
	(GENMASK_INPUT_CHECK(hi, lo) + __GENMASK_U128(hi, lo))
#else
/*
 * BUILD_BUG_ON_ZERO is not available in h files included from asm files,
 * 128bit exprssions don't work, neither can C 0UL (etc) constants be used.
 * These definitions only have to work for constants and don't require
 * that ~0 have any specific number of set bits.
 */
#define GENMASK(hi, lo) ___GENMASK(1, hi, lo)
#define GENMASK_ULL(hi, lo) ___GENMASK(1, hi, lo)
#endif

#endif	/* __LINUX_BITS_H */
