// SPDX-License-Identifier: GPL-2.0
/*
 * Generate definitions needed by the preprocessor.
 * This code generates raw asm output which is post-processed
 * to extract and format the required data.
 */

#define COMPILE_OFFSETS
#include <linux/kbuild.h>
#include "slab.h"

int main(void)
{
	/* The constants to put into include/generated/kmem_cache_size.h */
	DEFINE(KMEM_CACHE_SIZE, sizeof(struct kmem_cache));
	DEFINE(KMEM_CACHE_ALIGN, __alignof(struct kmem_cache));
	/* End of constants */

	return 0;
}
