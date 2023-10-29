/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2023, Jiaxun Yang <jiaxun.yang@flygoat.com>
 * MIPS Low level debug include file for UHI semihosting
 */

#include <asm/asm.h>

/**
 * printch() - write a character to UHI Plog
 * @a0: ASCII character to write
 *
 * clobbers v0, t9
 * UHI may clobber k0, k1
 */
		.pushsection .bss
charbuf_addr:	.space 8
		.popsection
NESTED(printch, 0, ra)
	PTR_LI	t9, charbuf_addr
	sb	a0, 0(t9)
	sb	zero, 1(t9)
	move	a0, t9
	li	v0, 0x1
	li	t9, 13 /* plog */
	.set push
	.set MIPS_ISA_LEVEL_RAW
	sdbbp	1
	.set pop
	jr	ra
	END(printch)

/**
 * printascii() - write a string to UHI Plog
 * @a0: pointer to NULL-terminated ASCII string
 *
 * clobbers v0
 * UHI may clobber k0, k1
 */
NESTED(printascii, 0, ra)
	li	v0, 0x1
	li	t9, 13 /* plog */
	.set push
	.set MIPS_ISA_LEVEL_RAW
	sdbbp	1
	.set pop
	jr		ra
	END(printascii)
