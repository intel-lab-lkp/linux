/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2026 ARM Ltd.
 * Author: Leonardo Bras <leo.bras@arm.com>
 */

#ifndef __ARM64_KVM_DIRTY_BIT_H__
#define __ARM64_KVM_DIRTY_BIT_H__

#include <asm/kvm_pgtable.h>

enum hacdbs_status {
	HACDBS_OFF,
	HACDBS_IDLE,
	HACDBS_RUNNING,
	HACDBS_ERROR
};

struct hacdbs {
	enum hacdbs_status status;
	int size;
};

DECLARE_PER_CPU(struct hacdbs, hacdbs_pcp);

void __init kvm_hacdbs_init(void);
void kvm_hacdbs_cpu_up(void);
void kvm_hacdbs_cpu_down(void);

int __kvm_arch_dirty_log_clear(struct kvm *kvm,
			       struct kvm_memory_slot *memslot,
			       struct kvm_clear_dirty_log *log,
			       unsigned long *bitmap,
			       bool *flush);

static inline bool kvm_arch_dirty_clear_enabled(struct kvm *kvm)
{
	return this_cpu_read(hacdbs_pcp.status) == HACDBS_IDLE &&
	       (kvm->arch.mmu.pgt->flags & KVM_PGTABLE_S2_DBM);
}

static inline int kvm_arch_dirty_log_clear(struct kvm *kvm,
					   struct kvm_memory_slot *memslot,
					   struct kvm_clear_dirty_log *log,
					   unsigned long *bitmap,
					   bool *flush)
{
	if (!kvm_arch_dirty_clear_enabled(kvm))
		return -EPERM;

	return __kvm_arch_dirty_log_clear(kvm, memslot, log, bitmap, flush);
}

#endif /* __ARM64_KVM_DIRTY_BIT_H__ */
