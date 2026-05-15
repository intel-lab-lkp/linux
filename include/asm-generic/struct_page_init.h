/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_GENERIC_STRUCT_PAGE_INIT_H
#define _ASM_GENERIC_STRUCT_PAGE_INIT_H

#include <linux/compiler.h>
#include <linux/types.h>

static __always_inline void arch_optimize_store_u64(u64 *dst, u64 val)
{
	*dst = val;
}

static __always_inline void arch_optimize_store_drain(void)
{
}

#endif /* _ASM_GENERIC_STRUCT_PAGE_INIT_H */
