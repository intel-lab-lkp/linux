/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __LINUX_BITS_H
#define __LINUX_BITS_H

#include <vdso/bits.h>
#include <uapi/linux/bits.h>

#define BIT_MASK(nr)		(UL(1) << ((nr) % BITS_PER_LONG))
#define BIT_WORD(nr)		((nr) / BITS_PER_LONG)
#define BIT_ULL_MASK(nr)	(ULL(1) << ((nr) % BITS_PER_LONG_LONG))
#define BIT_ULL_WORD(nr)	((nr) / BITS_PER_LONG_LONG)
#define BITS_PER_BYTE		8
#define BITS_PER_TYPE(type)	(sizeof(type) * BITS_PER_BYTE)

/*
 * Create a contiguous bitmask starting at bit position @l and ending at
 * position @h. For example
 * GENMASK_ULL(39, 21) gives us the 64bit vector 0x000000ffffe00000.
 */
#if !defined(__ASSEMBLY__)

#include <linux/build_bug.h>
#include <linux/compiler.h>
#include <linux/overflow.h>

#define GENMASK_INPUT_CHECK(h, l) BUILD_BUG_ON_ZERO(const_true((l) > (h)))

/*
 * Generate a mask for the specified type @t. Additional checks are made to
 * guarantee the value returned fits in that type, relying on
 * -Wshift-count-overflow compiler check to detect incompatible arguments.
 * For example, all these create build errors or warnings:
 *
 * - GENMASK(15, 20): wrong argument order
 * - GENMASK(72, 15): doesn't fit unsigned long
 * - GENMASK_U32(33, 15): doesn't fit in a u32
 */
#define GENMASK_TYPE(t, h, l)					\
	((unsigned int)GENMASK_INPUT_CHECK(h, l) +		\
	 ((t)-1 << (l) & (t)-1 >> (BITS_PER_TYPE(t) - 1 - (h))))
#endif

#define GENMASK(h, l)		GENMASK_TYPE(unsigned long, h, l)
#define GENMASK_ULL(h, l)	GENMASK_TYPE(unsigned long long, h, l)

#define GENMASK_U8(h, l)	GENMASK_TYPE(u8, h, l)
#define GENMASK_U16(h, l)	GENMASK_TYPE(u16, h, l)
#define GENMASK_U32(h, l)	GENMASK_TYPE(u32, h, l)
#define GENMASK_U64(h, l)	GENMASK_TYPE(u64, h, l)
#define GENMASK_U128(h, l)	GENMASK_TYPE(u128, h, l)

#if !defined(__ASSEMBLY__)
/*
 * Fixed-type variants of BIT(), with additional checks like GENMASK_TYPE().
 * The following examples generate compiler warnings from BIT_INPUT_CHECK().
 *
 * - BIT_U8(8)
 * - BIT_U32(-1)
 * - BIT_U32(40)
 */
#define BIT_INPUT_CHECK(type, nr) \
	BUILD_BUG_ON_ZERO(const_true((nr) >= BITS_PER_TYPE(type)))

#define BIT_TYPE(type, nr) ((unsigned int)BIT_INPUT_CHECK(type, nr) + ((type)1 << (nr)))
#endif /* defined(__ASSEMBLY__) */

#define BIT_U8(nr)	BIT_TYPE(u8, nr)
#define BIT_U16(nr)	BIT_TYPE(u16, nr)
#define BIT_U32(nr)	BIT_TYPE(u32, nr)
#define BIT_U64(nr)	BIT_TYPE(u64, nr)

#if defined(__ASSEMBLY__)

/*
 * The assmebler only supports one size of signed integer rather than
 * the fixed width integer types of C.
 * There is also no method for reported invalid input.
 * Error in .h files will usually be picked up when compiled into C files.
 *
 * Define type-size agnostic definitions that generate the correct value
 * provided it can be represented by the assembler.
 */

#define GENMASK_TYPE(t, h, l)	((2 << (h)) - (1 << (l)))
#define BIT_TYPE(type, nr)	(1 << (nr))

#endif /* defined(__ASSEMBLY__) */

#endif	/* __LINUX_BITS_H */
