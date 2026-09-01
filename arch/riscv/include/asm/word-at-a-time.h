/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2012 Regents of the University of California
 *
 * Derived from arch/x86/include/asm/word-at-a-time.h
 */

#ifndef _ASM_RISCV_WORD_AT_A_TIME_H
#define _ASM_RISCV_WORD_AT_A_TIME_H


#include <asm/asm-extable.h>
#include <linux/bitops.h>
#include <linux/wordpart.h>

#if !(defined(CONFIG_RISCV_ISA_ZBB) && defined(CONFIG_TOOLCHAIN_HAS_ZBB))
#include <asm-generic/word-at-a-time.h>
#else
struct word_at_a_time {
	const unsigned long one_bits, high_bits;
};

#define WORD_AT_A_TIME_CONSTANTS { REPEAT_BYTE(0x01), REPEAT_BYTE(0x80) }

static inline unsigned long has_zero(unsigned long val,
	unsigned long *bits, const struct word_at_a_time *c)
{
	unsigned long mask = ((val - c->one_bits) & ~val) & c->high_bits;
	*bits = mask;
	return mask;
}

static inline unsigned long prep_zero_mask(unsigned long val,
	unsigned long bits, const struct word_at_a_time *c)
{
	return bits;
}

static inline unsigned long create_zero_mask(unsigned long bits)
{
	bits = (bits - 1) & ~bits;
	return bits >> 7;
}

#ifdef CONFIG_64BIT
/*
 * Jan Achrenius on G+: microoptimized version of
 * the simpler "(mask & ONEBYTES) * ONEBYTES >> 56"
 * that works for the bytemasks without having to
 * mask them first.
 */
static inline long count_masked_bytes(unsigned long mask)
{
	return mask*0x0001020304050608ul >> 56;
}

#else	/* 32-bit case */

/* Carl Chatfield / Jan Achrenius G+ version for 32-bit */
static inline long count_masked_bytes(long mask)
{
	/* (000000 0000ff 00ffff ffffff) -> ( 1 1 2 3 ) */
	long a = (0x0ff0001+mask) >> 23;
	/* Fix the 1 for 00 case */
	return a & mask;
}
#endif

static inline unsigned long find_zero(unsigned long mask)
{
	if (riscv_has_extension_likely(RISCV_ISA_EXT_ZBB))
		return !mask ? 0 : ((__fls(mask) + 1) >> 3);

	return count_masked_bytes(mask);
}

/* The mask we created is directly usable as a bytemask */
#define zero_bytemask(mask) (mask)

#endif /* !(defined(CONFIG_RISCV_ISA_ZBB) && defined(CONFIG_TOOLCHAIN_HAS_ZBB)) */

#ifdef CONFIG_DCACHE_WORD_ACCESS

/*
 * Load an unaligned word from kernel space.
 *
 * In the (very unlikely) case of the word being a page-crosser
 * and the next page not being mapped, take the exception and
 * return zeroes in the non-existing part.
 */
static inline unsigned long load_unaligned_zeropad(const void *addr)
{
	unsigned long ret;

	/* Load word from unaligned pointer addr */
	asm(
	"1:	" REG_L " %0, %2\n"
	"2:\n"
	_ASM_EXTABLE_LOAD_UNALIGNED_ZEROPAD(1b, 2b, %0, %1)
	: "=&r" (ret)
	: "r" (addr), "m" (*(unsigned long *)addr));

	return ret;
}

#endif	/* CONFIG_DCACHE_WORD_ACCESS */

#endif /* _ASM_RISCV_WORD_AT_A_TIME_H */
