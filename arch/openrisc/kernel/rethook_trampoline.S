/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Author: Sahil Siddiq <sahilcdq0@gmail.com>
 */

#include <linux/linkage.h>

#include <asm/asm.h>
#include <asm/asm-offsets.h>
#include <asm/ptrace.h>

	.text

	.macro save_all_base_regs
	l.sw	PT_GPR2(r1),r2
	l.sw	PT_GPR3(r1),r3
	l.sw	PT_GPR4(r1),r4
	l.sw	PT_GPR5(r1),r5
	l.sw	PT_GPR6(r1),r6
	l.sw	PT_GPR7(r1),r7
	l.sw	PT_GPR8(r1),r8
	l.sw	PT_GPR9(r1),r9
	l.sw	PT_GPR10(r1),r10
	l.sw	PT_GPR11(r1),r11
	l.sw	PT_GPR12(r1),r12
	l.sw	PT_GPR13(r1),r13
	l.sw	PT_GPR14(r1),r14
	l.sw	PT_GPR15(r1),r15
	l.sw	PT_GPR16(r1),r16
	l.sw	PT_GPR17(r1),r17
	l.sw	PT_GPR18(r1),r18
	l.sw	PT_GPR19(r1),r19
	l.sw	PT_GPR20(r1),r20
	l.sw	PT_GPR21(r1),r21
	l.sw	PT_GPR22(r1),r22
	l.sw	PT_GPR23(r1),r23
	l.sw	PT_GPR24(r1),r24
	l.sw	PT_GPR25(r1),r25
	l.sw	PT_GPR26(r1),r26
	l.sw	PT_GPR27(r1),r27
	l.sw	PT_GPR28(r1),r28
	l.sw	PT_GPR29(r1),r29
	l.sw	PT_GPR30(r1),r30
	l.sw	PT_GPR31(r1),r31
	.endm

	.macro restore_all_base_regs
	l.lwz	r2,PT_GPR2(r1)
	l.lwz	r3,PT_GPR3(r1)
	l.lwz	r4,PT_GPR4(r1)
	l.lwz	r5,PT_GPR5(r1)
	l.lwz	r6,PT_GPR6(r1)
	l.lwz	r7,PT_GPR7(r1)
	l.lwz	r8,PT_GPR8(r1)
	/* Return address in r9 is stale, so don't restore it */
	l.lwz	r10,PT_GPR10(r1)
	l.lwz	r11,PT_GPR11(r1)
	l.lwz	r12,PT_GPR12(r1)
	l.lwz	r13,PT_GPR13(r1)
	l.lwz	r14,PT_GPR14(r1)
	l.lwz	r15,PT_GPR15(r1)
	l.lwz	r16,PT_GPR16(r1)
	l.lwz	r17,PT_GPR17(r1)
	l.lwz	r18,PT_GPR18(r1)
	l.lwz	r19,PT_GPR19(r1)
	l.lwz	r20,PT_GPR20(r1)
	l.lwz	r21,PT_GPR21(r1)
	l.lwz	r22,PT_GPR22(r1)
	l.lwz	r23,PT_GPR23(r1)
	l.lwz	r24,PT_GPR24(r1)
	l.lwz	r25,PT_GPR25(r1)
	l.lwz	r26,PT_GPR26(r1)
	l.lwz	r27,PT_GPR27(r1)
	l.lwz	r28,PT_GPR28(r1)
	l.lwz	r29,PT_GPR29(r1)
	l.lwz	r30,PT_GPR30(r1)
	l.lwz	r31,PT_GPR31(r1)
	.endm

SYM_CODE_START(arch_rethook_trampoline)
	/* Allocate enough space on stack for pt_regs */
	l.addi	r1,r1,-PT_SIZE
	save_all_base_regs

	l.addi	r3,r1,0	/* pt_regs */
	l.jal	arch_rethook_trampoline_callback
	l.nop

	/* use the result as the return-address */
	l.addi	r9,r11,0

	restore_all_base_regs
	l.addi	r1,r1,PT_SIZE

	l.jr	r9
	l.nop
SYM_CODE_END(arch_rethook_trampoline)
