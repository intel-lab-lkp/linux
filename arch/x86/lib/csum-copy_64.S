/*
 * Copyright 2002, 2003 Andi Kleen, SuSE Labs.
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file COPYING in the main directory of this archive
 * for more details. No warranty for anything given at all.
 */
#include <linux/linkage.h>
#include <asm/errno.h>
#include <asm/asm.h>

/*
 * Checksum copy with exception handling.
 * On exceptions src_err_ptr or dst_err_ptr is set to -EFAULT and the
 * destination is zeroed.
 *
 * Input
 * rdi  source
 * rsi  destination
 * edx  len (32bit)
 *
 * Output
 * eax  64bit sum. undefined in case of exception.
 *
 * Wrappers need to take care of valid exception sum and zeroing.
 * They also should align source or destination to 8 bytes.
 */

.macro source
10:
	_ASM_EXTABLE_UA(10b, .Lfault)
.endm

.macro dest
20:
	_ASM_EXTABLE_UA(20b, .Lfault)
.endm

.macro restore_regs_and_ret
	movq 0*8(%rsp), %rbx
	movq 1*8(%rsp), %r12
	movq 2*8(%rsp), %r14
	movq 3*8(%rsp), %r13
	movq 4*8(%rsp), %r15
	addq $5*8, %rsp
	RET
.endm

.macro	_csum_partial_copy
	subq  $5*8, %rsp
	movq  %rbx, 0*8(%rsp)
	movq  %r12, 1*8(%rsp)
	movq  %r14, 2*8(%rsp)
	movq  %r13, 3*8(%rsp)
	movq  %r15, 4*8(%rsp)

	movl  $-1, %eax
	xorl  %r9d, %r9d
	movl  %edx, %ecx
	cmpl  $8, %ecx
	jb    .Lshort\@

	testb  $7, %sil
	jne   .Lunaligned\@
.Laligned\@:
	.set  INP, %rdi		/* input pointer */
	.set  OUTP, %rsi	/* output pointer */
	.set  SUM, %rax		/* checksum accumulator */
	.set  ZERO, %r9		/* zero register */
	.set  LEN, %ecx		/* byte count */
	.set  LEN64B, %r12d	/* 64-byte block count */
	.set  TMP1, %rbx
	.set  TMP2, %r8
	.set  TMP3, %r11
	.set  TMP4, %rdx
	.set  TMP5, %r10
	.set  TMP6, %r15
	.set  TMP7, %r14
	.set  TMP8, %r13

	movl  LEN, LEN64B

	shrl  $6, LEN64B
	jz	.Lhandle_tail\@     /* < 64 */

	clc

	.p2align 4
.Lloop\@:
	source
	movq  (INP), TMP1
	source
	movq  8(INP), TMP2
	source
	movq  16(INP), TMP3
	source
	movq  24(INP), TMP4

	source
	movq  32(INP), TMP5
	source
	movq  40(INP), TMP6
	source
	movq  48(INP), TMP7
	source
	movq  56(INP), TMP8

30:
	/*
	 * No _ASM_EXTABLE_UA; this is used for intentional prefetch on a
	 * potentially unmapped kernel address.
	 */
	_ASM_EXTABLE(30b, 2f)
	prefetcht0 5*64(%rdi)
2:
	adcq  TMP1, SUM
	adcq  TMP2, SUM
	adcq  TMP3, SUM
	adcq  TMP4, SUM
	adcq  TMP5, SUM
	adcq  TMP6, SUM
	adcq  TMP7, SUM
	adcq  TMP8, SUM

	decl LEN64B

	dest
	movq TMP1, (OUTP)
	dest
	movq TMP2, 8(OUTP)
	dest
	movq TMP3, 16(OUTP)
	dest
	movq TMP4, 24(OUTP)

	dest
	movq TMP5, 32(OUTP)
	dest
	movq TMP6, 40(OUTP)
	dest
	movq TMP7, 48(OUTP)
	dest
	movq TMP8, 56(OUTP)

	leaq 64(INP), INP
	leaq 64(OUTP), OUTP

	jnz	.Lloop\@

	adcq  ZERO, SUM

	/* do last up to 56 bytes */
.Lhandle_tail\@:
	/* ecx:	count, rcx.63: the end result needs to be rol8 */
	movq %rcx, %r10
	andl $63, %ecx
	shrl $3, %ecx
	jz	.Lfold\@
	clc
	.p2align 4
.Lloop_8\@:
	source
	movq (INP), TMP1
	adcq TMP1, SUM
	decl LEN
	dest
	movq TMP1, (OUTP)
	leaq 8(INP), INP /* preserve carry */
	leaq 8(OUTP), OUTP
	jnz	.Lloop_8\@
	adcq ZERO, SUM	/* add in carry */

.Lfold\@:
	/* reduce checksum to 32bits */
	movl %eax, %ebx
	shrq $32, %rax
	addl %ebx, %eax
	adcl %r9d, %eax

	/* do last up to 6 bytes */
.Lhandle_7\@:
	movl %r10d, %ecx
	andl $7, %ecx
.L1\@:				/* .Lshort\@ rejoins the common path here */
	shrl $1, %ecx
	jz   .Lhandle_1\@
	movl $2, %edx
	xorl %ebx, %ebx
	clc
	.p2align 4
.Lloop_1\@:
	source
	movw (%rdi), %bx
	adcl %ebx, %eax
	decl %ecx
	dest
	movw %bx, (%rsi)
	leaq 2(%rdi), %rdi
	leaq 2(%rsi), %rsi
	jnz .Lloop_1\@
	adcl %r9d, %eax	/* add in carry */

	/* handle last odd byte */
.Lhandle_1\@:
	testb $1, %r10b
	jz    .Lende\@
	xorl  %ebx, %ebx
	source
	movb (%rdi), %bl
	dest
	movb %bl, (%rsi)
	addl %ebx, %eax
	adcl %r9d, %eax		/* carry */

.Lende\@:
	testq %r10, %r10
	js  .Lwas_odd\@
.Lout\@:
	restore_regs_and_ret
.Lshort\@:
	movl %ecx, %r10d
	jmp  .L1\@
.Lunaligned\@:
	xorl %ebx, %ebx
	testb $1, %sil
	jne  .Lodd\@
1:	testb $2, %sil
	je   2f
	source
	movw (%rdi), %bx
	dest
	movw %bx, (%rsi)
	leaq 2(%rdi), %rdi
	subq $2, %rcx
	leaq 2(%rsi), %rsi
	addq %rbx, %rax
2:	testb $4, %sil
	je .Laligned\@
	source
	movl (%rdi), %ebx
	dest
	movl %ebx, (%rsi)
	leaq 4(%rdi), %rdi
	subq $4, %rcx
	leaq 4(%rsi), %rsi
	addq %rbx, %rax
	jmp .Laligned\@

.Lodd\@:
	source
	movb (%rdi), %bl
	dest
	movb %bl, (%rsi)
	leaq 1(%rdi), %rdi
	leaq 1(%rsi), %rsi
	/* decrement, set MSB */
	leaq -1(%rcx, %rcx), %rcx
	rorq $1, %rcx
	shll $8, %ebx
	addq %rbx, %rax
	jmp 1b

.Lwas_odd\@:
	roll $8, %eax
	jmp .Lout\@
.endm

	/* Exception: just return 0 */
.Lfault:
	xorl %eax, %eax
	restore_regs_and_ret

SYM_FUNC_START(csum_partial_copy_generic)
	_csum_partial_copy
SYM_FUNC_END(csum_partial_copy_generic)
