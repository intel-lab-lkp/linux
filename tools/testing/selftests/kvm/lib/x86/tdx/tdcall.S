/* SPDX-License-Identifier: GPL-2.0-only */
/* Adapted from arch/x86/virt/vmx/tdx/tdxcall.S */

#ifndef __ASSEMBLY__
#define __ASSEMBLY__
#endif

#include <linux/bits.h>
#include "tdx/tdcall.h"
#include "tdx/tdcall_offsets.h"

/*
 * TDCALL is supported in Binutils >= 2.36, add it for older version.
 */
#define tdcall		.byte 0x66,0x0f,0x01,0xcc

/*
 * Bitmasks of exposed registers (with VMM).
 */
#define TDX_R10		BIT(10)
#define TDX_R11		BIT(11)
#define TDX_R12		BIT(12)
#define TDX_R13		BIT(13)
#define TDX_R14		BIT(14)
#define TDX_R15		BIT(15)

/*
 * These registers are clobbered to hold arguments for each
 * TDVMCALL. They are safe to expose to the VMM.
 * Each bit in this mask represents a register ID. Bit field
 * details can be found in TDX GHCI specification, section
 * titled "TDCALL [TDG.VP.VMCALL] leaf".
 */
#define TDVMCALL_EXPOSE_REGS_MASK    \
         (TDX_R10 | TDX_R11 | TDX_R12 | TDX_R13 | TDX_R14 | TDX_R15)

.code64
.section .text

.globl __tdx_tdcall
.type __tdx_tdcall, @function
__tdx_tdcall:
	/* Set up stack frame */
	push %rbp
	movq %rsp, %rbp

	/* Save callee-saved GPRs as mandated by the x86_64 ABI */
	push %r15
	push %r14
	push %r13
	push %r12

	/* Mangle function call ABI into TDCALL ABI: */
	/* Set TDCALL leaf ID (TDVMCALL (0)) in RAX */
	xor %eax, %eax

	/* Copy tdcall registers from arg struct: */
	movq TDX_TDCALL_R10(%rdi), %r10
	movq TDX_TDCALL_R11(%rdi), %r11
	movq TDX_TDCALL_R12(%rdi), %r12
	movq TDX_TDCALL_R13(%rdi), %r13
	movq TDX_TDCALL_R14(%rdi), %r14
	movq TDX_TDCALL_R15(%rdi), %r15

	movl $TDVMCALL_EXPOSE_REGS_MASK, %ecx

	tdcall

	/* TDVMCALL leaf return code is in R10 */
	movq %r10, %rax

	/* Copy tdcall result registers to arg struct if needed */
	testq $TDX_TDCALL_HAS_OUTPUT, %rsi
	jz .Lout

	movq %r10, TDX_TDCALL_R10(%rdi)
	movq %r11, TDX_TDCALL_R11(%rdi)
	movq %r12, TDX_TDCALL_R12(%rdi)
	movq %r13, TDX_TDCALL_R13(%rdi)
	movq %r14, TDX_TDCALL_R14(%rdi)
	movq %r15, TDX_TDCALL_R15(%rdi)
.Lout:
	/* Restore callee-saved GPRs as mandated by the x86_64 ABI */
	pop %r12
	pop %r13
	pop %r14
	pop %r15

	pop %rbp
	ret

/* Disable executable stack */
.section .note.GNU-stack,"",%progbits
