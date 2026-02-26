/* SPDX-License-Identifier: GPL-2.0 */
/*
 * arm64 KFENCE support.
 *
 * Copyright (C) 2020, Google LLC.
 */

#ifndef __ASM_KFENCE_H
#define __ASM_KFENCE_H

#include <asm/set_memory.h>

static inline bool kfence_protect_page(unsigned long addr, bool protect)
{
	set_memory_valid(addr, 1, !protect);

	return true;
}

#ifdef CONFIG_KFENCE
extern bool kfence_early_init;

extern phys_addr_t arm64_kfence_alloc_pool(void);

extern void arm64_kfence_map_pool(phys_addr_t kfence_pool, pgd_t *pgdp);

static inline bool arm64_kfence_can_set_direct_map(void)
{
	return !kfence_early_init;
}
bool arch_kfence_init_pool(void);
#else /* CONFIG_KFENCE */
static inline bool arm64_kfence_can_set_direct_map(void) { return false; }

static inline phys_addr_t arm64_kfence_alloc_pool(void) { return 0; }

static inline void arm64_kfence_map_pool(phys_addr_t kfence_pool, pgd_t *pgdp) { }
#endif /* CONFIG_KFENCE */

#endif /* __ASM_KFENCE_H */
