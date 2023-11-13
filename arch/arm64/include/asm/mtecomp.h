/* SPDX-License-Identifier: GPL-2.0 */

#ifndef __ASM_MTECOMP_H
#define __ASM_MTECOMP_H

#include <linux/types.h>

/**
 * mte_is_compressed() - check if the supplied pointer contains compressed tags.
 * @ptr: pointer returned by kmalloc() or mte_compress().
 *
 * Returns: true iff bit 0 of @ptr is 1, which is only possible if @ptr was
 * returned by mte_is_compressed().
 */
static inline bool mte_is_compressed(void *ptr)
{
	return ((unsigned long)ptr & 1);
}

#if defined(CONFIG_ARM64_MTE_COMP)

void *mte_compress(u8 *tags);
bool mte_decompress(void *handle, u8 *tags);

#else

static inline void *mte_compress(u8 *tags)
{
	return NULL;
}

static inline bool mte_decompress(void *data, u8 *tags)
{
	return false;
}

#endif // CONFIG_ARM64_MTE_COMP

#endif // __ASM_MTECOMP_H
