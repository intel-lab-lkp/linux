// SPDX-License-Identifier: GPL-2.0

#ifndef __ARM64_ASM_SETUP_H
#define __ARM64_ASM_SETUP_H

#include <linux/string.h>

#include <uapi/asm/setup.h>

/*
 * These two variables are used in the head.S file.
 */
extern phys_addr_t __fdt_pointer __initdata;
extern u64 __cacheline_aligned boot_args[4];

/*
 * rodata=on (default)
 *
 *    This applies read-only attributes to VM areas and to the linear
 *    alias of the backing pages as well. This prevents code or read-
 *    only data from being modified (inadvertently or intentionally),
 *    via another mapping for the same memory page.
 *
 *    But this might cause linear map region to be mapped down to base
 *    pages, which may adversely affect performance in some cases.
 *
 * rodata=off
 *
 *    This provides more block mappings and contiguous hints for linear
 *    map region which would minimize TLB footprint. This also leaves
 *    read-only kernel memory writable for debugging.
 *
 * rodata=noalias
 *
 *    This provides more block mappings and contiguous hints for linear
 *    map region which would minimize TLB footprint.
 */
static inline bool arch_parse_debug_rodata(char *arg)
{
	extern bool rodata_enabled;
	extern bool rodata_full;

	if (!arg)
		return false;

	if (!strcmp(arg, "on")) {
		rodata_enabled = rodata_full = true;
		return true;
	}

	if (!strcmp(arg, "off")) {
		rodata_enabled = rodata_full = false;
		return true;
	}

	if (!strcmp(arg, "noalias")) {
		rodata_enabled = true;
		rodata_full = false;
		return true;
	}

	return false;
}
#define arch_parse_debug_rodata arch_parse_debug_rodata

#endif
