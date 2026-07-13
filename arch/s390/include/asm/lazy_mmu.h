/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __LAZY_MMU_H
#define __LAZY_MMU_H

void lazy_mmu_online_boot_cpu(void);
int lazy_mmu_online_cpu(gfp_t gfp, unsigned int cpu);
void lazy_mmu_offline_cpu(unsigned int cpu);

#endif /* __LAZY_MMU_H */
