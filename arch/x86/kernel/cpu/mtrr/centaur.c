// SPDX-License-Identifier: GPL-2.0
#include <linux/init.h>
#include <linux/mm.h>

#include <asm/mtrr.h>
#include <asm/msr.h>

#include "mtrr.h"
#include <linux/compiler.h>  /* for __maybe_unused */

static struct {
	unsigned long high;
	unsigned long low;
} centaur_mcr[8];

static u8 centaur_mcr_reserved;
static u8 centaur_mcr_type;	/* 0 for winchip, 1 for winchip2 */

/**
 * centaur_get_free_region - Get a free MTRR.
 *
 * @base: The starting (base) address of the region.
 * @size: The size (in bytes) of the region.
 *
 * Returns: the index of the region on success, else -1 on error.
 */
static int
centaur_get_free_region(unsigned long base, unsigned long size, int replace_reg)
{
	unsigned long lbase, lsize;
	mtrr_type ltype;
	int i, max;

	max = num_var_ranges;
	if (replace_reg >= 0 && replace_reg < max)
		return replace_reg;

	for (i = 0; i < max; ++i) {
		if (centaur_mcr_reserved & (1 << i))
			continue;
		mtrr_if->get(i, &lbase, &lsize, &ltype);
		if (lsize == 0)
			return i;
	}

	return -ENOSPC;
}

static void __maybe_unused
centaur_get_mcr(unsigned int reg, unsigned long *base,
		unsigned long *size, mtrr_type * type)
{
	*base = centaur_mcr[reg].high >> PAGE_SHIFT;
	*size = -(centaur_mcr[reg].low & 0xfffff000) >> PAGE_SHIFT;
	*type = MTRR_TYPE_WRCOMB;		/* write-combining  */

	if (centaur_mcr_type == 1 && ((centaur_mcr[reg].low & 31) & 2))
		*type = MTRR_TYPE_UNCACHABLE;
	if (centaur_mcr_type == 1 && (centaur_mcr[reg].low & 31) == 25)
		*type = MTRR_TYPE_WRBACK;
	if (centaur_mcr_type == 0 && (centaur_mcr[reg].low & 31) == 31)
		*type = MTRR_TYPE_WRBACK;
}

static void __maybe_unused
centaur_set_mcr(unsigned int reg, unsigned long base,
		unsigned long size, mtrr_type type)
{
	unsigned long low, high;

	if (size == 0) {
		/* Disable */
		high = low = 0;
	} else {
		high = base << PAGE_SHIFT;
		if (centaur_mcr_type == 0) {
			/* Only support write-combining... */
			low = -size << PAGE_SHIFT | 0x1f;
		} else {
			if (type == MTRR_TYPE_UNCACHABLE)
				low = -size << PAGE_SHIFT | 0x02; /* NC */
			else
				low = -size << PAGE_SHIFT | 0x09; /* WWO, WC */
		}
	}
	centaur_mcr[reg].high = high;
	centaur_mcr[reg].low = low;
	wrmsr(MSR_IDT_MCR0 + reg, low, high);
}

static int __maybe_unused
centaur_validate_add_page(unsigned long base, unsigned long size, unsigned int type)
{
	if (centaur_mcr_type == 0) {
		/* Winchip2: supports both Write-Combining and Uncached */
		if (type != MTRR_TYPE_WRCOMB && type != MTRR_TYPE_UNCACHABLE) {
			pr_warn("mtrr: only write-combining and uncacheable supported\n");
			return -EINVAL;
		}
	} else {
		/* Other Centaur: only Write-Combining */
		if (type != MTRR_TYPE_WRCOMB) {
			pr_warn("mtrr: only write-combining supported\n");
			return -EINVAL;
		}
	}
	return 0;
}

/* The ops structure is used externally, so the compiler won't complain */
const struct mtrr_ops centaur_mtrr_ops = {
	.var_regs          = 8,
	.set               = centaur_set_mcr,
	.get               = centaur_get_mcr,
	.get_free_region   = centaur_get_free_region,
	.validate_add_page = centaur_validate_add_page,
	.have_wrcomb       = positive_have_wrcomb,
};
