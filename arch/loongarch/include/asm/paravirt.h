/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_LOONGARCH_PARAVIRT_H
#define _ASM_LOONGARCH_PARAVIRT_H

#ifdef CONFIG_PARAVIRT

#include <linux/jump_label.h>
#include <linux/cpumask.h>
#include <linux/smp.h>

DECLARE_STATIC_KEY_FALSE(virt_preempt_key);
DECLARE_STATIC_KEY_FALSE(virt_spin_lock_key);
DECLARE_STATIC_KEY_FALSE(pv_tlb_flush_key);
DECLARE_PER_CPU(struct kvm_steal_time, steal_time);

int __init pv_ipi_init(void);
int __init pv_time_init(void);
int __init pv_spinlock_init(void);

void kvm_flush_tlb_mask(const struct cpumask *cpumask,
			smp_call_func_t func, void *info);

static inline bool pv_tlb_flush_enabled(void)
{
	return static_branch_unlikely(&pv_tlb_flush_key);
}

#else

static inline int pv_ipi_init(void)
{
	return 0;
}

static inline int pv_time_init(void)
{
	return 0;
}

static inline int pv_spinlock_init(void)
{
	return 0;
}

static inline bool pv_tlb_flush_enabled(void)
{
	return false;
}

static inline void kvm_flush_tlb_mask(const struct cpumask *cpumask,
				       smp_call_func_t func, void *info)
{
}

#endif // CONFIG_PARAVIRT
#endif
