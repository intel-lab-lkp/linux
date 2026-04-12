// SPDX-License-Identifier: GPL-2.0
/*
 * ARM64 Nested virtualization helpers
 */

#include "nested.h"
#include "processor.h"
#include "test_util.h"
#include <asm/sysreg.h>

void prepare_hyp(void)
{
	write_sysreg(HCR_EL2_E2H | HCR_EL2_RW, hcr_el2);
	write_sysreg(hyp_vectors, vbar_el2);
	isb();
}

void init_vcpu(struct vcpu *vcpu, vm_paddr_t l2_pc, vm_paddr_t l2_stack_top)
{
	memset(vcpu, 0, sizeof(*vcpu));
	vcpu->context.regs.pc = l2_pc;
	vcpu->context.regs.pstate = PSR_MODE_EL1h | PSR_D_BIT | PSR_A_BIT | PSR_I_BIT | PSR_F_BIT;
	vcpu->context.sys_regs[SP_EL1] = l2_stack_top;
}

void __sysreg_save_el1_state(struct cpu_context *ctxt)
{
	ctxt->sys_regs[SP_EL1] = read_sysreg(sp_el1);
}

void __sysreg_restore_el1_state(struct cpu_context *ctxt)
{
	write_sysreg(ctxt->sys_regs[SP_EL1], sp_el1);
}

int run_l2(struct vcpu *vcpu, struct hyp_data *hyp_data)
{
	u64 ret;

	__sysreg_restore_el1_state(&vcpu->context);

	write_sysreg(vcpu->context.regs.pstate, spsr_el2);
	write_sysreg(vcpu->context.regs.pc, elr_el2);

	ret =  __guest_enter(vcpu, &hyp_data->hyp_context);

	vcpu->context.regs.pc = read_sysreg(elr_el2);
	vcpu->context.regs.pstate = read_sysreg(spsr_el2);

	__sysreg_save_el1_state(&vcpu->context);

	return ret;
}

void __hyp_exception(u64 type)
{
	GUEST_FAIL("Unexpected hyp exception! type: %lx\n", type);
}
