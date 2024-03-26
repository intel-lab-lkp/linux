/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2023, Jiaxun Yang <jiaxun.yang@flygoat.com>
 * MIPS Low level debug routines
 *
 * Note: Those functions are designed carefully to only clobber
 * t0, t1, t9, so they can be used without stack. Also they are
 * position independent to be called from any place.
 */

#include <linux/compiler.h>

#include <asm/addrspace.h>
#include <asm/asm.h>
#include <asm/asm-offsets.h>
#include <asm/mipsregs.h>
#include <asm/regdef.h>

#include CONFIG_DEBUG_LL_INCLUDE

#ifdef DEBUG_LL_UART
/**
 * printch() - write a character to the UART
 * @a0: ASCII character to write
 *
 * clobbers t0, t9
 */
NESTED(printch, 0, ra)
	addruart	t9, t0
#ifdef CONFIG_DEBUG_UART_FLOW_CONTROL
	waituartcts	t0, t9
#endif
	waituarttxrdy	t0, t9
	senduart	a0, t9
	busyuart	t0, t9
	jr		ra
	END(printch)

/**
 * printascii() - write a string to the UART
 * @a0: pointer to NULL-terminated ASCII string
 *
 * clobbers t0, t1, t9
 *
 * Write a null-terminated ASCII string to the UART.
 */
NESTED(printascii, 0, ra)
	addruart	t9, t0
	move		t1, a0

1:	lb		a0, 0(t1)
	beqz		a0, 2f
#ifdef CONFIG_DEBUG_UART_FLOW_CONTROL
	waituartcts	t0, t9
#endif
	waituarttxrdy	t0, t9
	senduart	a0, t9
	busyuart	t0, t9
	PTR_ADDIU	t1, t1, 1
	b		1b

2:	jr		ra
	END(printascii)
#endif /* DEBUG_LL_UART */

/**
 * printhex() - write n byte hex value to the UART
 * @a0: Hex value to write to the UART
 * @a1: number of bytes to write to the UART
 *
 * clobbers: t0, t1, t9
 */

		.pushsection .bss
hexbuf_addr:	.space 32
		.popsection

NESTED(printhex, 0, ra)
	PTR_LA		t9, hexbuf_addr
	sll		a1, a1, 1
1:
	subu		t0, a1, 1
	sll		t0, t0, 2
	LONG_SRLV	t0, a0, t0
	andi		t0, t0, 0xf
	li		t1, '0'
	blt		t0, 10, 2f
	li		t1, 'a'
	addiu		t0, t0, -10
2:
	addu		t1, t1, t0
	sb		t1, 0(t9)
	PTR_ADDIU	t9, t9, 1
	addiu		a1, a1, -1
	bnez		a1, 1b
	sb		zero, 0(t9)

	move		a1, ra
	PTR_LA		a0, hexbuf_addr
	bal		printascii

	jr		a1
	END(printhex)

NESTED(printhex1, 0, ra)
	li		a1, 1
	b		printhex
	END(printhex1)

NESTED(printhex2, 0, ra)
	li		a1, 2
	b		printhex
	END(printhex2)

NESTED(printhex4, 0, ra)
	li		a1, 4
	b		printhex
	END(printhex4)

#ifdef CONFIG_64BIT
NESTED(printhex8, 0, ra)
	li		a1, 8
	b		printhex
	END(printhex8)
#endif

NESTED(printhexl, 0, ra)
	li		a1, PTRSIZE
	b		printhex
	END(printhexl)
