/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * ARM64 Nested virtualization defines
 */

#ifndef SELFTEST_KVM_NESTED_H
#define SELFTEST_KVM_NESTED_H

#define ARM_EXCEPTION_IRQ	  0
#define ARM_EXCEPTION_EL1_SERROR  1
#define ARM_EXCEPTION_TRAP	  2
#define ARM_EXCEPTION_IL	  3
#define ARM_EXCEPTION_EL2_IRQ	  4
#define ARM_EXCEPTION_EL2_SERROR  5
#define ARM_EXCEPTION_EL2_TRAP	  6

#ifndef __ASSEMBLER__

#include <asm/ptrace.h>
#include "kvm_util.h"

extern char hyp_vectors[];

enum vcpu_sysreg {
	__INVALID_SYSREG__,   /* 0 is reserved as an invalid value */

	SP_EL1,

	NR_SYS_REGS
};

struct cpu_context {
	struct user_pt_regs regs;	/* sp = sp_el0 */
	u64 sys_regs[NR_SYS_REGS];
};

struct vcpu {
	struct cpu_context context;
};

/*
 * KVM has host_data and hyp_context, combine them because we're only doing
 * hyp context.
 */
struct hyp_data {
	struct cpu_context hyp_context;
};

void prepare_hyp(void);
void init_vcpu(struct vcpu *vcpu, vm_paddr_t l2_pc, vm_paddr_t l2_stack_top);
int run_l2(struct vcpu *vcpu, struct hyp_data *hyp_data);

u64 do_hvc(u64 action, u64 arg1, u64 arg2);
u64 __guest_enter(struct vcpu *vcpu, struct cpu_context *hyp_context);
void __hyp_exception(u64 type);

void __sysreg_save_el1_state(struct cpu_context *ctxt);
void __sysreg_restore_el1_state(struct cpu_context *ctxt);

#endif /* !__ASSEMBLER__ */

#endif /* SELFTEST_KVM_NESTED_H */
