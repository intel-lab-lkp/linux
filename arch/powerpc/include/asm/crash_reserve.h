/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_POWERPC_CRASH_RESERVE_H
#define _ASM_POWERPC_CRASH_RESERVE_H

#include <asm/rtas.h>

/* crash kernel regions are Page size agliged */
#define CRASH_ALIGN             PAGE_SIZE

#ifdef CONFIG_ARCH_HAS_GENERIC_CRASHKERNEL_RESERVATION
static inline bool arch_add_crash_res_to_iomem(void)
{
	return false;
}
#define arch_add_crash_res_to_iomem arch_add_crash_res_to_iomem
#endif
#define DEFAULT_CRASH_KERNEL_LOW_SIZE SZ_64M

#define CRASH_ADDR_LOW_MAX	RTAS_INSTANTIATE_MAX
#define CRASH_ADDR_HIGH_MAX	memblock_end_of_DRAM()

#endif /* _ASM_POWERPC_CRASH_RESERVE_H */
