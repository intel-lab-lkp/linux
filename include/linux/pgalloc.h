/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_PGALLOC_H
#define _LINUX_PGALLOC_H
#include <asm/pgalloc.h>

#ifdef CONFIG_ARCH_USE_SYNC_GLOBAL_PGDS
void arch_sync_global_p4ds(unsigned long start, unsigned long end);
void arch_sync_global_pgds(unsigned long start, unsigned long end);
#else
static inline void arch_sync_global_p4ds(unsigned long start, unsigned long end) {}
static inline void arch_sync_global_pgds(unsigned long start, unsigned long end) {}
#endif

static inline void p4d_populate_kernel(unsigned long addr, p4d_t *p4d, pud_t *pud)
{
	p4d_populate(&init_mm, p4d, pud);
	arch_sync_global_p4ds(addr, addr);
}

static inline void pgd_populate_kernel(unsigned long addr, pgd_t *pgd, p4d_t *p4d)
{
	pgd_populate(&init_mm, pgd, p4d);
	arch_sync_global_pgds(addr, addr);
}

#endif /* _LINUX_PGALLOC_H */
