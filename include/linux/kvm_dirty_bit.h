/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2026 ARM Ltd.
 * Author: Leonardo Bras <leo.bras@arm.com>
 */

#ifndef __KVM_DIRTY_BIT_H__
#define __KVM_DIRTY_BIT_H__

#ifndef CONFIG_HAVE_KVM_HW_DIRTY_BIT

static inline int kvm_arch_dirty_log_clear(struct kvm *kvm,
					   struct kvm_memory_slot *memslot,
					   struct kvm_clear_dirty_log *log,
					   unsigned long *bitmap,
					   bool *flush)
{
	return -ENXIO;
}

static inline int kvm_arch_dirty_ring_clear(struct kvm *kvm,
					    struct kvm_dirty_ring *ring,
					    int *nr_entries_reset)
{
	return -ENXIO;
}

#else /* CONFIG_HAVE_KVM_HW_DIRTY_BIT */

#include <asm/kvm_dirty_bit.h>

#endif /* CONFIG_HAVE_KVM_HW_DIRTY_BIT */

#endif /* __KVM_DIRTY_BIT_H__ */
