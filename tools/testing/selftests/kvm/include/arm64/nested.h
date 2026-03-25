/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * ARM64 Nested virtualization defines
 */

#ifndef SELFTEST_KVM_NESTED_H
#define SELFTEST_KVM_NESTED_H

uint64_t get_l1_vtcr(void);

void nested_map(struct kvm_vm *vm, vm_paddr_t guest_pgd,
		uint64_t nested_paddr, uint64_t paddr, uint64_t size);
void nested_map_memslot(struct kvm_vm *vm, vm_paddr_t guest_pgd,
			uint32_t memslot);

void prepare_l2_stack(struct kvm_vm *vm, struct kvm_vcpu *vcpu);
void prepare_hyp_state(struct kvm_vm *vm, struct kvm_vcpu *vcpu);
void prepare_eret_destination(struct kvm_vm *vm, struct kvm_vcpu *vcpu, void *l2_pc);
void prepare_nested_sync_handler(struct kvm_vm *vm, struct kvm_vcpu *vcpu);

void run_l2(void);
void after_hvc(void);
void do_hvc(void);

#endif /* SELFTEST_KVM_NESTED_H */
