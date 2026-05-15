/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_X86_STRUCT_PAGE_INIT_H
#define _ASM_X86_STRUCT_PAGE_INIT_H

#include <linux/compiler.h>
#include <linux/types.h>

/*
 * x86-64 guarantees SSE2, so MOVNTI and SFENCE are always available there.
 *
 * KASAN/KMSAN rely on compiler-instrumented stores. Keep the x86 override
 * disabled for those configs and fall back to plain stores instead.
 */
#if defined(CONFIG_X86_64) && !defined(CONFIG_KASAN) && !defined(CONFIG_KMSAN)
static __always_inline void arch_optimize_store_u64(u64 *dst, u64 val)
{
	asm volatile("movnti %1, %0" : "=m"(*dst) : "r"(val));
}

static __always_inline void arch_optimize_store_drain(void)
{
	asm volatile("sfence" : : : "memory");
}
#else
#include <asm-generic/struct_page_init.h>
#endif

#endif /* _ASM_X86_STRUCT_PAGE_INIT_H */
