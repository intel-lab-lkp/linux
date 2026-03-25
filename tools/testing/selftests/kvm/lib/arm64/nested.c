// SPDX-License-Identifier: GPL-2.0
/*
 * ARM64 Nested virtualization helpers
 */

#include "kvm_util.h"
#include "nested.h"
#include "processor.h"
#include "test_util.h"

#include <asm/sysreg.h>

static void hvc_handler(struct ex_regs *regs)
{
	GUEST_ASSERT_EQ(get_current_el(), 2);
	GUEST_PRINTF("hvc handler\n");
	regs->pstate = PSR_MODE_EL2h | PSR_D_BIT | PSR_A_BIT | PSR_I_BIT | PSR_F_BIT;
	regs->pc = (u64)after_hvc;
}

void prepare_l2_stack(struct kvm_vm *vm, struct kvm_vcpu *vcpu)
{
	size_t l2_stack_size;
	uint64_t l2_stack_paddr;

	l2_stack_size = vm->page_size == 4096 ? DEFAULT_STACK_PGS * vm->page_size :
					 vm->page_size;
	l2_stack_paddr = __vm_phy_pages_alloc(vm, l2_stack_size / vm->page_size,
					      0, 0, false);
	vcpu_set_reg(vcpu, ARM64_CORE_REG(sp_el1), l2_stack_paddr + l2_stack_size);
}

void prepare_hyp_state(struct kvm_vm *vm, struct kvm_vcpu *vcpu)
{
	vcpu_set_reg(vcpu, KVM_ARM64_SYS_REG(SYS_HCR_EL2), HCR_EL2_RW);
}

void prepare_eret_destination(struct kvm_vm *vm, struct kvm_vcpu *vcpu, void *l2_pc)
{
	vm_paddr_t do_hvc_paddr = addr_gva2gpa(vm, (vm_vaddr_t)do_hvc);
	vm_paddr_t l2_pc_paddr = addr_gva2gpa(vm, (vm_vaddr_t)l2_pc);

	vcpu_set_reg(vcpu, KVM_ARM64_SYS_REG(SYS_SPSR_EL2), PSR_MODE_EL1h |
							    PSR_D_BIT     |
							    PSR_A_BIT     |
							    PSR_I_BIT     |
							    PSR_F_BIT);
	vcpu_set_reg(vcpu, KVM_ARM64_SYS_REG(SYS_ELR_EL2), l2_pc_paddr);
	/* HACK: use TPIDR_EL2 to pass address, see run_l2() in nested_asm.S */
	vcpu_set_reg(vcpu, KVM_ARM64_SYS_REG(SYS_TPIDR_EL2), do_hvc_paddr);
}

void prepare_nested_sync_handler(struct kvm_vm *vm, struct kvm_vcpu *vcpu)
{
	if (!vm->handlers) {
		vm_init_descriptor_tables(vm);
		vcpu_init_descriptor_tables(vcpu);
	}
	vm_install_sync_handler(vm, VECTOR_SYNC_LOWER_64,
				ESR_ELx_EC_HVC64, hvc_handler);
}
