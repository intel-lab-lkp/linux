// SPDX-License-Identifier: GPL-2.0-only
/*
 * This file contains ARM64 specific KASAN sw_tags code.
 */

#include <linux/kasan.h>

bool __arch_kasan_non_canonical_hook(unsigned long addr)
{
	/*
	 * For Software Tag-Based KASAN, kasan_mem_to_shadow() uses the
	 * arithmetic shift. Normally, this would make checking for a possible
	 * shadow address complicated, as the shadow address computation
	 * operation would overflow only for some memory addresses. However, due
	 * to the chosen KASAN_SHADOW_OFFSET values and the fact the
	 * kasan_mem_to_shadow() only operates on pointers with the tag reset,
	 * the overflow always happens.
	 *
	 * For arm64, the top byte of the pointer gets reset to 0xFF. Thus, the
	 * possible shadow addresses belong to a region that is the result of
	 * kasan_mem_to_shadow() applied to the memory range
	 * [0xFF00000000000000, 0xFFFFFFFFFFFFFFFF]. Despite the overflow, the
	 * resulting possible shadow region is contiguous, as the overflow
	 * happens for both 0xFF00000000000000 and 0xFFFFFFFFFFFFFFFF.
	 *
	 * Reset the addr's tag bits so the inline mode which still uses
	 * the logical shift can work correctly. Otherwise it would
	 * always return because of the 'smaller than' comparison below.
	 */
	addr |= (0xFFULL << 56);
	if (addr < (unsigned long)kasan_mem_to_shadow((void *)(0xFFULL << 56)) ||
	    addr > (unsigned long)kasan_mem_to_shadow((void *)(~0ULL)))
		return true;
	return false;
}
