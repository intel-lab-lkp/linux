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

.macro prefetch
30:
	/*
	 * No _ASM_EXTABLE_UA; this is used for intentional prefetch on a
	 * potentially unmapped kernel address.
	 */
	_ASM_EXTABLE(30b, 2f)
	prefetcht0 5*64(%rdi)
2:
.endm

.macro loadregs offset, src, regs:vararg
	source
	i = 0
.irp  r, \regs
	movq  8*(\offset + i)(\src), \r
.endr
.endm

.macro storeregs offset, dst, regs:vararg
	dest
	i = 0
.irp  r, \regs
	movq  \r, 8*(\offset + i)(\dst)
.endr
.endm

.macro sumregs sum, regs:vararg
.irp  r, \regs
	adcq  \r, \sum
.endr
.endm

.macro incr ptr, count
	leaq  8*(\count)(\ptr), \ptr
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
.if USE_APX
	cmpl  $3, LEN64B
	jb	.Lloop_64\@         /* < 192 */
	clc
	.p2align 4
.Lloop_192\@:
	.set  TMP9, %r16
	.set  TMP10, %r17
	.set  TMP11, %r18
	.set  TMP12, %r19
	.set  TMP13, %r20
	.set  TMP14, %r21
	.set  TMP15, %r22
	.set  TMP16, %r23
	.set  TMP17, %r24
	.set  TMP18, %r25
	.set  TMP19, %r26
	.set  TMP20, %r27
	.set  TMP21, %r28
	.set  TMP22, %r29
	.set  TMP23, %r30
	.set  TMP24, %r31

	.p2align 4
	loadregs 0, INP, TMP1, TMP2, TMP3, TMP4, TMP5, TMP6, TMP7, TMP8
	loadregs 8, INP, TMP9, TMP10, TMP11, TMP12, TMP13, TMP14, TMP15, TMP16
	loadregs 16, INP, TMP17, TMP18, TMP19, TMP20, TMP21, TMP22, TMP23, TMP24

	sumregs SUM, TMP1, TMP2, TMP3, TMP4, TMP5, TMP6, TMP7, TMP8
	sumregs SUM, TMP9, TMP10, TMP11, TMP12, TMP13, TMP14, TMP15, TMP16
	sumregs SUM, TMP17, TMP18, TMP19, TMP20, TMP21, TMP22, TMP23, TMP24

	storeregs 0, OUTP, TMP1, TMP2, TMP3, TMP4, TMP5, TMP6, TMP7, TMP8
	storeregs 8, OUTP, TMP9, TMP10, TMP11, TMP12, TMP13, TMP14, TMP15, TMP16
	storeregs 16, OUTP, TMP17, TMP18, TMP19, TMP20, TMP21, TMP22, TMP23, TMP24

	incr INP, 24
	incr OUTP, 24

	sub  $3, LEN64B
	cmp  $3, LEN64B
	jnb	.Lloop_192\@
.else
	clc

	.p2align 4
.endif
.Lloop_64\@:
	loadregs 0, INP, TMP1, TMP2, TMP3, TMP4, TMP5, TMP6, TMP7, TMP8

	prefetch

	sumregs SUM, TMP1, TMP2, TMP3, TMP4, TMP5, TMP6, TMP7, TMP8

	decl LEN64B

	storeregs 0, OUTP, TMP1, TMP2, TMP3, TMP4, TMP5, TMP6, TMP7, TMP8

	incr INP, 8
	incr OUTP, 8

	jnz	.Lloop_64\@

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
	loadregs 0, INP, TMP1
	sumregs SUM, TMP1
	decl LEN
	storeregs 0, OUTP, TMP1
	incr INP, 1 /* preserve carry */
	incr OUTP, 1
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

.set	USE_APX, 0
SYM_FUNC_START(csum_partial_copy)
	_csum_partial_copy
SYM_FUNC_END(csum_partial_copy)

#ifdef CONFIG_X86_APX
.set	USE_APX, 1
SYM_FUNC_START(csum_partial_copy_apx)
	_csum_partial_copy
SYM_FUNC_END(csum_partial_copy_apx)
#endif
