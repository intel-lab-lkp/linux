// SPDX-License-Identifier: GPL-2.0
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include "fault_injection.h"

#include <linux/dcache.h>
#include <linux/debugfs.h>
#include <linux/error-injection.h>
#include <linux/fault-inject.h>
#include <linux/init.h>
#include <linux/kvm_host.h>

static DECLARE_FAULT_ATTR(fail_tdp_mmu_cmpxchg);
static DECLARE_FAULT_ATTR(fail_tdp_mmu_resched);

bool tdp_mmu_cmpxchg_should_fail(void)
{
	return should_fail(&fail_tdp_mmu_cmpxchg, 1);
}
ALLOW_ERROR_INJECTION(tdp_mmu_cmpxchg_should_fail, TRUE);

bool tdp_mmu_should_inject_resched(void)
{
	return should_fail(&fail_tdp_mmu_resched, 1);
}
ALLOW_ERROR_INJECTION(tdp_mmu_should_inject_resched, TRUE);

void kvm_mmu_fault_injection_init(struct dentry *dentry)
{
#ifdef CONFIG_FAULT_INJECTION_DEBUG_FS
	fault_create_debugfs_attr("fail_tdp_mmu_cmpxchg", dentry,
				  &fail_tdp_mmu_cmpxchg);
	fault_create_debugfs_attr("fail_tdp_mmu_resched", dentry,
				  &fail_tdp_mmu_resched);
#endif
}
