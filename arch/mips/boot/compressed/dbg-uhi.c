// SPDX-License-Identifier: GPL-2.0
/*
 * zboot debug output for MIPS UHI semihosting
 */

#include <asm/uhi.h>

void puts(const char *s)
{
	uhi_plog(s, 0);
}
