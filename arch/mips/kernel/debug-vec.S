/* SPDX-License-Identifier: GPL-2.0 */
/*
 *  Copyright (C) 2023, Jiaxun Yang <jiaxun.yang@flygoat.com>
 *  MIPS Low level exception vectors and handler
 */

#include <asm/addrspace.h>
#include <asm/asm.h>
#include <asm/asm-offsets.h>
#include <asm/asmmacro.h>


.balign 0x1000
LEAF(debug_ll_vecs)
	.set	push
	.set	noreorder
	move	a0, zero
	PTR_LA  t9, debug_ll_exception
	jalr	t9
	nop
	b	.
	nop

.org	0x80
	move	a0, zero
	PTR_LA  t9, debug_ll_exception
	jalr	t9
	nop
	b	.
	nop

.org	0x100
	move	a0, zero
	PTR_LA  t9, debug_ll_exception
	jalr	t9
	nop
	b	.
	nop

.org	0x180
	move	a0, zero
	PTR_LA  t9, debug_ll_exception
	jalr	t9
	nop
	b	.
	nop

.org	0x200
	move	a0, zero
	PTR_LA  t9, debug_ll_exception
	jalr	t9
	nop
	b	.
	nop

.org	0x280
	move	a0, zero
	PTR_LA  t9, debug_ll_exception
	jalr	t9
	nop
	b	.
	nop

.org	0x300
	move	a0, zero
	PTR_LA  t9, debug_ll_exception
	jalr	t9
	nop
	b	.
	nop

.org	0x380
	move	a0, zero
	PTR_LA  t9, debug_ll_exception
	jalr	t9
	nop
	b	.
	nop

.org	0x400
	move	a0, zero
	PTR_LA  t9, debug_ll_exception
	jalr	t9
	nop
	b	.
	nop

.org	0x480
	move	a0, zero
	PTR_LA  t9, debug_ll_exception
	jalr	t9
	nop
	b	.
	nop

	.set	pop
END(debug_ll_vecs)


/**
 * debug_ll_exception() - dump relevant exception state to LL outputs
 * @a0: pointer to NULL-terminated ASCII string naming the exception
 *
 * If a0 is NULL, it will print out exception vector number calculated
 * from the ra insted. This is useful for debugging exceptions that
 * happen before the exception handler is set up, or routed from BEV.
 */
LEAF(debug_ll_exception)
	move		k0, ra
	move		k1, a0
	PTR_LA		t3, printascii

	PTR_LA		a0, str_newline
	jalr		t3  /* Call printascii */
	PTR_LA		a0, str_exp
	jalr		t3  /* Call printascii */
	beqz		k1, 1f
	move		a0, k1
	jalr		t3  /* Call printascii */
	b		2f
1:
	PTR_SRL		a0, k0, 7
	PTR_SLL		a0, a0, 7
	PTR_LA		t2, printhexl
	jalr		t2
2:
	PTR_LA		a0, str_newline
	jalr		t3  /* Call printascii */
	PTR_LA		a0, str_newline
	jalr		t3  /* Call printascii */

#define DUMP_COP0_REG(reg, name, sz, _mfc0)		\
	PTR_LA		a0, 8f;				\
	jalr		t3;				\
	_mfc0		a0, reg;			\
	PTR_LA		t2, printhex##sz;		\
	jalr		t2;				\
	PTR_LA		a0, str_newline;		\
	jalr		t3;				\
	TEXT(name)

#if defined(CONFIG_SMP) && defined(CONFIG_CPU_MIPSR6)
	DUMP_COP0_REG(CP0_GLOBALNUMBER, "GlobalNumber: 0x", 4, mfc0)
#endif
#if MIPS_ISA_REV >= 2
	DUMP_COP0_REG(CP0_EBASE,    "EBase:    0x", l, MFC0)
#endif
#ifdef CONFIG_64BIT
	DUMP_COP0_REG(CP0_XCONTEXT, "Xcontext: 0x", 8, dmfc0)
#else
	DUMP_COP0_REG(CP0_CONTEXT,  "Context:  0x", 4, mfc0)
#endif
	DUMP_COP0_REG(CP0_CAUSE,    "Cause:    0x", 4, mfc0)
	DUMP_COP0_REG(CP0_STATUS,   "Status:   0x", 4, mfc0)
	DUMP_COP0_REG(CP0_EPC,      "EPC:      0x", l, MFC0)
	DUMP_COP0_REG(CP0_BADVADDR, "BadVAddr: 0x", l, MFC0)
#if MIPS_ISA_REV >= 6
	DUMP_COP0_REG(CP0_BADINSTR, "BadInstr: 0x", 4, mfc0)
#endif
	PTR_LA		a0, str_newline
	jalr		t3  /* Call printascii */
	jr		k0
	END(debug_ll_exception)

.pushsection	.rodata.str
str_exp: .asciiz "Low level exception: "
str_newline: .asciiz "\r\n"
.popsection

NESTED(setup_debug_ll_exception, 0, ra)
	PTR_LA		t0, debug_ll_vecs
#if MIPS_ISA_REV >= 2
	/* Set ebase to debug_ll_vecs */
#if defined (CONFIG_64BIT) && !defined(KBUILD_64BIT_SYM32)
	ori		t0, MIPS_EBASE_WG
#endif
	MTC0		t0, CP0_EBASE
#else
	/* Copy debug_ll_vecs to start of KSEG0 */
	PTR_LI		t1, CKSEG0
	PTR_ADDIU	t2, t0, 0x400 /* Only copy 0x400 as that is what reserved on old systems */
copy_word:
	PTR_LW		t3, 0(t0)
	PTR_SW		t3, 0(t1)
	PTR_ADDIU	t0, t0, PTRSIZE
	PTR_ADDIU	t1, t1, PTRSIZE
	PTR_BNE		t0, t2, copy_word
#endif
	/* Clear BEV bit in status register */
	mfc0		t0, CP0_STATUS
	and		t0, t0, ~ST0_BEV
	mtc0		t0, CP0_STATUS
	jr		ra
	END(setup_debug_ll_exception)
