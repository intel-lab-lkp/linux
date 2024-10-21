/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SELFTEST_KVM_UTIL_ARCH_H
#define SELFTEST_KVM_UTIL_ARCH_H

#include <stdbool.h>
#include <stdint.h>

#include "kvm_util_types.h"
#include "test_util.h"

extern bool is_forced_emulation_enabled;

struct kvm_vm_arch {
	vm_vaddr_t gdt;
	vm_vaddr_t tss;
	vm_vaddr_t idt;

	uint64_t c_bit;
	uint64_t s_bit;
	int sev_fd;
	bool is_pt_protected;
};

static inline bool __vm_arch_has_protected_memory(struct kvm_vm_arch *arch)
{
	return arch->c_bit || arch->s_bit;
}

#define vm_arch_has_protected_memory(vm) \
	__vm_arch_has_protected_memory(&(vm)->arch)

#define vcpu_arch_put_guest(mem, __val)							\
do {											\
	const typeof(mem) val = (__val);						\
											\
	if (!is_forced_emulation_enabled || guest_random_bool(&guest_rng)) {		\
		(mem) = val;								\
	} else if (guest_random_bool(&guest_rng)) {					\
		__asm__ __volatile__(KVM_FEP "mov %1, %0"				\
				     : "+m" (mem)					\
				     : "r" (val) : "memory");				\
	} else {									\
		uint64_t __old = READ_ONCE(mem);					\
											\
		__asm__ __volatile__(KVM_FEP LOCK_PREFIX "cmpxchg %[new], %[ptr]"	\
				     : [ptr] "+m" (mem), [old] "+a" (__old)		\
				     : [new]"r" (val) : "memory", "cc");		\
	}										\
} while (0)

#define DEFINE_ARCH_VM_STAT					\
	DEFINE_CHECK_STAT(vm, mmu_shadow_zapped)		\
	DEFINE_CHECK_STAT(vm, mmu_pte_write)			\
	DEFINE_CHECK_STAT(vm, mmu_pde_zapped)			\
	DEFINE_CHECK_STAT(vm, mmu_flooded)			\
	DEFINE_CHECK_STAT(vm, mmu_recycled)			\
	DEFINE_CHECK_STAT(vm, mmu_cache_miss)			\
	DEFINE_CHECK_STAT(vm, mmu_unsync)			\
	DEFINE_CHECK_STAT(vm, pages_4k)				\
	DEFINE_CHECK_STAT(vm, pages_2m)				\
	DEFINE_CHECK_STAT(vm, pages_1g)				\
	DEFINE_CHECK_STAT(vm, pages)				\
	DEFINE_CHECK_STAT(vm, nx_lpage_splits)			\
	DEFINE_CHECK_STAT(vm, max_mmu_page_hash_collisions)	\
	DEFINE_CHECK_STAT(vm, max_mmu_rmap_size)		\

#define DEFINE_ARCH_VCPU_STAT					\
	DEFINE_CHECK_STAT(vcpu, pf_taken)			\
	DEFINE_CHECK_STAT(vcpu, pf_fixed)			\
	DEFINE_CHECK_STAT(vcpu, pf_emulate)			\
	DEFINE_CHECK_STAT(vcpu, pf_spurious)			\
	DEFINE_CHECK_STAT(vcpu, pf_fast)			\
	DEFINE_CHECK_STAT(vcpu, pf_mmio_spte_created)		\
	DEFINE_CHECK_STAT(vcpu, pf_guest)			\
	DEFINE_CHECK_STAT(vcpu, tlb_flush)			\
	DEFINE_CHECK_STAT(vcpu, invlpg)				\
	DEFINE_CHECK_STAT(vcpu, exits)				\
	DEFINE_CHECK_STAT(vcpu, io_exits)			\
	DEFINE_CHECK_STAT(vcpu, mmio_exits)			\
	DEFINE_CHECK_STAT(vcpu, signal_exits)			\
	DEFINE_CHECK_STAT(vcpu, irq_window_exits)		\
	DEFINE_CHECK_STAT(vcpu, nmi_window_exits)		\
	DEFINE_CHECK_STAT(vcpu, l1d_flush)			\
	DEFINE_CHECK_STAT(vcpu, halt_exits)			\
	DEFINE_CHECK_STAT(vcpu, request_irq_exits)		\
	DEFINE_CHECK_STAT(vcpu, irq_exits)			\
	DEFINE_CHECK_STAT(vcpu, host_state_reload)		\
	DEFINE_CHECK_STAT(vcpu, fpu_reload)			\
	DEFINE_CHECK_STAT(vcpu, insn_emulation)			\
	DEFINE_CHECK_STAT(vcpu, insn_emulation_fail)		\
	DEFINE_CHECK_STAT(vcpu, hypercalls)			\
	DEFINE_CHECK_STAT(vcpu, irq_injections)			\
	DEFINE_CHECK_STAT(vcpu, nmi_injections)			\
	DEFINE_CHECK_STAT(vcpu, req_event)			\
	DEFINE_CHECK_STAT(vcpu, nested_run)			\
	DEFINE_CHECK_STAT(vcpu, directed_yield_attempted)	\
	DEFINE_CHECK_STAT(vcpu, directed_yield_successful)	\
	DEFINE_CHECK_STAT(vcpu, preemption_reported)		\
	DEFINE_CHECK_STAT(vcpu, preemption_other)		\
	DEFINE_CHECK_STAT(vcpu, guest_mode)			\
	DEFINE_CHECK_STAT(vcpu, notify_window_exits)		\

#endif  // SELFTEST_KVM_UTIL_ARCH_H
