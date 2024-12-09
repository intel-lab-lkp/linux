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
 * rodata=on (default):
 *    Apply read-only attributes of VM areas to the linear alias of
 *    the backing pages as well. This prevents code or read-only data
 *    from being modified (inadvertently or intentionally) via another
 *    mapping of the same memory page.
 *
 *    This requires the linear region to be mapped down to pages,
 *    which may adversely affect performance in some cases.
 *
 * rodata=off:
 *    It provides us more block mappings and contiguous hits
 *    to map the linear region which minimize the TLB footprint.
 *    Leave read-only kernel memory writable for debugging.
 *
 * rodata=noalias:
 *    It provides us more block mappings and contiguous hits
 *    to map the linear region which minimize the TLB footprint.
 *    And the linear aliases of pages belonging to read-only mappings
 *    in vmalloc region are also marked as read-only.
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
