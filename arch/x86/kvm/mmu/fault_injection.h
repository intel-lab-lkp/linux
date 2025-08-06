/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __KVM_X86_MMU_FAULT_INJECTION_H
#define __KVM_X86_MMU_FAULT_INJECTION_H

#include <linux/types.h>
#include <linux/init.h>
#include <linux/dcache.h>

#ifdef CONFIG_KVM_FAULT_INJECTION

void kvm_mmu_fault_injection_init(struct dentry *dentry);
bool tdp_mmu_cmpxchg_should_fail(void);
bool tdp_mmu_should_inject_resched(void);

#else

static inline void kvm_mmu_fault_injection_init(struct dentry *dentry)
{
}
static inline bool tdp_mmu_cmpxchg_should_fail(void)
{
	return false;
}
static inline bool tdp_mmu_should_inject_resched(void)
{
	return false;
}

#endif /* CONFIG_FAULT_INJECTION_KVM */

#endif /* __KVM_X86_MMU_FAULT_INJECTION_H */
