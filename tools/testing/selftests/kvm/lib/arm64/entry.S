/* SPDX-License-Identifier: GPL-2.0 */
/*
 * adapted from arch/arm64/kvm/hyp/entry.S
 */

 .globl do_hvc
 do_hvc:
	hvc	#0
	ret

/*
 * Manually define these for now
 */
// offsetof(struct vcpu, context)
#define CPU_CONTEXT		0
// offsetof(struct cpu_context, regs)
#define CPU_USER_PT_REGS	0

#define CPU_XREG_OFFSET(x)	(CPU_USER_PT_REGS + 8*x)
#define CPU_LR_OFFSET		CPU_XREG_OFFSET(30)
#define CPU_SP_EL0_OFFSET	(CPU_LR_OFFSET + 8)

.macro save_callee_saved_regs ctxt
	str	x18,      [\ctxt, #CPU_XREG_OFFSET(18)]
	stp	x19, x20, [\ctxt, #CPU_XREG_OFFSET(19)]
	stp	x21, x22, [\ctxt, #CPU_XREG_OFFSET(21)]
	stp	x23, x24, [\ctxt, #CPU_XREG_OFFSET(23)]
	stp	x25, x26, [\ctxt, #CPU_XREG_OFFSET(25)]
	stp	x27, x28, [\ctxt, #CPU_XREG_OFFSET(27)]
	stp	x29, lr,  [\ctxt, #CPU_XREG_OFFSET(29)]
.endm

.macro restore_callee_saved_regs ctxt
	ldr	x18,      [\ctxt, #CPU_XREG_OFFSET(18)]
	ldp	x19, x20, [\ctxt, #CPU_XREG_OFFSET(19)]
	ldp	x21, x22, [\ctxt, #CPU_XREG_OFFSET(21)]
	ldp	x23, x24, [\ctxt, #CPU_XREG_OFFSET(23)]
	ldp	x25, x26, [\ctxt, #CPU_XREG_OFFSET(25)]
	ldp	x27, x28, [\ctxt, #CPU_XREG_OFFSET(27)]
	ldp	x29, lr,  [\ctxt, #CPU_XREG_OFFSET(29)]
.endm

.macro save_sp_el0 ctxt, tmp
	mrs	\tmp,	sp_el0
	str	\tmp,	[\ctxt, #CPU_SP_EL0_OFFSET]
.endm

.macro restore_sp_el0 ctxt, tmp
	ldr	\tmp,	  [\ctxt, #CPU_SP_EL0_OFFSET]
	msr	sp_el0, \tmp
.endm

/*
 * u64 __guest_enter(struct vcpu *vcpu, struct cpu_context *hyp_context);
 */
.globl __guest_enter
__guest_enter:
	// x0: vcpu
	// x1: hyp context

	// Store vcpu and hyp context pointer on the stack
	stp	x0, x1, [sp, #-16]!

	// Store the hyp regs
	save_callee_saved_regs x1

	// Save hyp's sp_el0
	save_sp_el0	x1, x2

	// x29 = vCPU user pt regs
	add	x29, x0, #CPU_CONTEXT

	// Restore the guest's sp_el0
	restore_sp_el0 x29, x0

	// Restore guest regs x0-x17
	ldp	x0, x1,   [x29, #CPU_XREG_OFFSET(0)]
	ldp	x2, x3,   [x29, #CPU_XREG_OFFSET(2)]
	ldp	x4, x5,   [x29, #CPU_XREG_OFFSET(4)]
	ldp	x6, x7,   [x29, #CPU_XREG_OFFSET(6)]
	ldp	x8, x9,   [x29, #CPU_XREG_OFFSET(8)]
	ldp	x10, x11, [x29, #CPU_XREG_OFFSET(10)]
	ldp	x12, x13, [x29, #CPU_XREG_OFFSET(12)]
	ldp	x14, x15, [x29, #CPU_XREG_OFFSET(14)]
	ldp	x16, x17, [x29, #CPU_XREG_OFFSET(16)]

	// Restore guest regs x18-x29, lr
	restore_callee_saved_regs x29

	// Do not touch any register after this!
	eret

.globl __guest_exit
__guest_exit:
	// x0: return code
	// x1: vcpu
	// x2-x29,lr: vcpu regs
	// vcpu x0-x1 on the stack

	add	x1, x1, #CPU_CONTEXT

	// Store the guest regs x2 and x3
	stp	x2, x3,   [x1, #CPU_XREG_OFFSET(2)]

	// Retrieve the guest regs x0-x1 from the stack
	ldp	x2, x3, [sp], #16	// x0, x1

	// Store the guest regs x0-x1 and x4-x17
	stp	x2, x3,   [x1, #CPU_XREG_OFFSET(0)]
	stp	x4, x5,   [x1, #CPU_XREG_OFFSET(4)]
	stp	x6, x7,   [x1, #CPU_XREG_OFFSET(6)]
	stp	x8, x9,   [x1, #CPU_XREG_OFFSET(8)]
	stp	x10, x11, [x1, #CPU_XREG_OFFSET(10)]
	stp	x12, x13, [x1, #CPU_XREG_OFFSET(12)]
	stp	x14, x15, [x1, #CPU_XREG_OFFSET(14)]
	stp	x16, x17, [x1, #CPU_XREG_OFFSET(16)]

	// Store the guest regs x18-x29, lr
	save_callee_saved_regs x1

	// Store the guest's sp_el0
	save_sp_el0	x1, x2

	// At this point x0 and x1 on the stack is popped, so next is vCPU
	// pointer, then hyp_context pointer
	// *sp == vCPU, *(sp + 8) == hyp_context
	// load x2 = hyp_context, x3 is just for ldp and popping sp
	ldp	x3, x2, [sp], #16

	// Restore hyp's sp_el0
	restore_sp_el0 x2, x3

	// Now restore the hyp regs
	restore_callee_saved_regs x2

	dsb	sy		// Synchronize against in-flight ld/st
	ret
