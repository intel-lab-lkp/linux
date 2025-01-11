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

enum kvm_x86_stats {
	VM_STAT(mmu_shadow_zapped),
	VM_STAT(mmu_pte_write),
	VM_STAT(mmu_pde_zapped),
	VM_STAT(mmu_flooded),
	VM_STAT(mmu_recycled),
	VM_STAT(mmu_cache_miss),
	VM_STAT(mmu_unsync),
	VM_STAT(pages_4k),
	VM_STAT(pages_2m),
	VM_STAT(pages_1g),
	VM_STAT(pages),
	VM_STAT(nx_lpage_splits),
	VM_STAT(max_mmu_page_hash_collisions),
	VM_STAT(max_mmu_rmap_size),

	VCPU_STAT(pf_taken),
	VCPU_STAT(pf_fixed),
	VCPU_STAT(pf_emulate),
	VCPU_STAT(pf_spurious),
	VCPU_STAT(pf_fast),
	VCPU_STAT(pf_mmio_spte_created),
	VCPU_STAT(pf_guest),
	VCPU_STAT(tlb_flush),
	VCPU_STAT(invlpg),
	VCPU_STAT(exits),
	VCPU_STAT(io_exits),
	VCPU_STAT(mmio_exits),
	VCPU_STAT(signal_exits),
	VCPU_STAT(irq_window_exits),
	VCPU_STAT(nmi_window_exits),
	VCPU_STAT(l1d_flush),
	VCPU_STAT(halt_exits),
	VCPU_STAT(request_irq_exits),
	VCPU_STAT(irq_exits),
	VCPU_STAT(host_state_reload),
	VCPU_STAT(fpu_reload),
	VCPU_STAT(insn_emulation),
	VCPU_STAT(insn_emulation_fail),
	VCPU_STAT(hypercalls),
	VCPU_STAT(irq_injections),
	VCPU_STAT(nmi_injections),
	VCPU_STAT(req_event),
	VCPU_STAT(nested_run),
	VCPU_STAT(directed_yield_attempted),
	VCPU_STAT(directed_yield_successful),
	VCPU_STAT(preemption_reported),
	VCPU_STAT(preemption_other),
	VCPU_STAT(guest_mode),
	VCPU_STAT(notify_window_exits),
};

#endif  // SELFTEST_KVM_UTIL_ARCH_H
