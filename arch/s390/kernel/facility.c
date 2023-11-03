// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright IBM Corp. 2023
 */

#include <asm/facility.h>

unsigned int stfle_size(void)
{
	static unsigned int size = 0;
	u64 dummy;

	if (!size) {
		size = __stfle_asm(&dummy, 1) + 1;
	}
	return size;
}
EXPORT_SYMBOL(stfle_size);
