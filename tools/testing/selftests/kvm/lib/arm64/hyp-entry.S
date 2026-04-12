/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * adapted from arch/arm64/kvm/hyp/hyp-entry.S
 */

#include "nested.h"

// skip over x0, x1 saved on entry, must be used only before the stack is modified
.macro get_vcpu_ptr vcpu
	ldr	\vcpu, [sp, #16]
.endm

	.text

el1_sync:	// Guest trapped into EL2

	get_vcpu_ptr	x1
	mov	x0, #ARM_EXCEPTION_TRAP
	b	__guest_exit

el1_irq:
el1_fiq:
	get_vcpu_ptr	x1
	mov	x0, #ARM_EXCEPTION_IRQ
	b	__guest_exit

el1_error:
	get_vcpu_ptr	x1
	mov	x0, #ARM_EXCEPTION_EL1_SERROR
	b	__guest_exit

el2_sync:
	mov	x0, #ARM_EXCEPTION_EL2_TRAP
	b	__hyp_exception

el2_irq:
el2_fiq:
	mov	x0, #ARM_EXCEPTION_EL2_IRQ
	b	__hyp_exception

el2_error:
	mov	x0, #ARM_EXCEPTION_EL2_SERROR
	b	__hyp_exception


	.ltorg

	.align 11

.globl hyp_vectors
hyp_vectors:

.macro exception_vector target
	.align 7
	stp	x0, x1, [sp, #-16]!
	b	\target
.endm

	exception_vector	el2_sync	// Synchronous EL2t
	exception_vector	el2_irq		// IRQ EL2t
	exception_vector	el2_fiq		// FIQ EL2t
	exception_vector	el2_error	// Error EL2t

	exception_vector	el2_sync	// Synchronous EL2h
	exception_vector	el2_irq		// IRQ EL2h
	exception_vector	el2_fiq		// FIQ EL2h
	exception_vector	el2_error	// Error EL2h

	exception_vector	el1_sync	// Synchronous 64-bit EL1
	exception_vector	el1_irq		// IRQ 64-bit EL1
	exception_vector	el1_fiq		// FIQ 64-bit EL1
	exception_vector	el1_error	// Error 64-bit EL1

	exception_vector	el1_sync	// Synchronous 32-bit EL1
	exception_vector	el1_irq		// IRQ 32-bit EL1
	exception_vector	el1_fiq		// FIQ 32-bit EL1
	exception_vector	el1_error	// Error 32-bit EL1
