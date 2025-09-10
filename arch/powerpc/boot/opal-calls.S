/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (c) 2016 IBM Corporation.
 */

#include "ppc_asm.h"
#include "../include/asm/opal-api.h"

	.text

	.globl opal_kentry
opal_kentry:
	/* r3 is the fdt ptr */
	mtctr r4
	li	r4, 0
	li	r5, 0
	li	r6, 0
	li	r7, 0
#ifdef CONFIG_PPC64_BOOT_WRAPPER
	LOAD_REG_ADDR(r11, opal)
#else
	/* R_PPC_ADDR16_{HA,LO} are not supported, so copy pc into r11 */
	bcl	20,31,0f
0:	mflr	r11

	addis	r11,r11,(opal - 0b)@ha
	addi	r11,r11,(opal - 0b)@l;
#endif
	ld	r8,0(r11)
	ld	r9,8(r11)
	bctr

#ifdef CONFIG_PPC64_BOOT_WRAPPER

#define OPAL_CALL(name, token)				\
	.globl name;					\
name:							\
	li	r0, token;				\
	b	opal_call;

opal_call:
	mflr	r11
	std	r11,16(r1)
	mfcr	r12
	stw	r12,8(r1)
	mr	r13,r2

	/* Set opal return address */
	LOAD_REG_ADDR(r11, opal_return)
	mtlr	r11
	mfmsr	r12

	/* switch to BE when we enter OPAL */
	li	r11,MSR_LE
	andc	r12,r12,r11
	mtspr	SPRN_HSRR1,r12

	/* load the opal call entry point and base */
	LOAD_REG_ADDR(r11, opal)
	ld	r12,8(r11)
	ld	r2,0(r11)
	mtspr	SPRN_HSRR0,r12
	hrfid

opal_return:
	FIXUP_ENDIAN
	mr	r2,r13;
	lwz	r11,8(r1);
	ld	r12,16(r1)
	mtcr	r11;
	mtlr	r12
	blr

OPAL_CALL(opal_console_write,			OPAL_CONSOLE_WRITE);
OPAL_CALL(opal_console_read,			OPAL_CONSOLE_READ);
OPAL_CALL(opal_console_write_buffer_space,	OPAL_CONSOLE_WRITE_BUFFER_SPACE);
OPAL_CALL(opal_poll_events,			OPAL_POLL_EVENTS);
OPAL_CALL(opal_console_flush,			OPAL_CONSOLE_FLUSH);

#else /* !CONFIG_PPC64_BOOT_WRAPPER */

opal_call:
	mflr	r11
	stw	r11,4(r1)
	mfcr	r12
	stw	r12,16(r1)
	mr	r13,r2

	/* R_PPC_ADDR16_{HA,LO} are not supported, so copy pc into r2 */
	bcl	20,31,0f
0:	mflr	r2

	/* Set opal return address */
	addis	r11,r2,(opal_return - 0b)@ha
	addi	r11,r11,(opal_return - 0b)@l;
	mtlr	r11

	/* switch to 64-bit when we enter OPAL */
	mfmsr	r12
	li	r11,1
	rldicl	r11,r11,MSR_SF_LG,0
	or	r12,r12,r11
	mtspr	SPRN_HSRR1,r12

	/* load the opal call entry point and base */
	addis	r11,r2,(opal - 0b)@ha
	addi	r11,r11,(opal - 0b)@l;
	ld	r12,8(r11)
	ld	r2,0(r11)
	mtspr	SPRN_HSRR0,r12

	hrfid

opal_return:
	/* switch back to 32-bit */
	mfmsr	r12
	li	r11,1
	rldicl	r11,r11,MSR_SF_LG,0
	andc	r12,r12,r11
	mtmsrd	r12
	isync

	/* split 64-bit return value into 2 registers */
	rldicl	r4,r3,0,32
	rldicl	r3,r3,32,32

	mr	r2,r13
	lwz	r11,16(r1)
	lwz	r12,4(r1)
	mtcr	r11;
	mtlr	r12
	blr

	/*
	 * Wrapper functions need to combine and shift arguments into the
	 * 64-bit calling convention.
	 */

	.globl opal_console_write
opal_console_write:
	li	r0, OPAL_CONSOLE_WRITE
	sldi	r3,r3,32
	or	r3,r3,r4
	mr	r4,r5
	mr	r5,r6
	b	opal_call

	.globl opal_console_read
opal_console_read:
	li	r0, OPAL_CONSOLE_READ
	sldi	r3,r3,32
	or	r3,r3,r4
	mr	r4,r5
	mr	r5,r6
	b	opal_call

	.globl opal_console_write_buffer_space
opal_console_write_buffer_space:
	li	r0, OPAL_CONSOLE_WRITE_BUFFER_SPACE
	sldi	r3,r3,32
	or	r3,r3,r4
	mr	r4,r5
	b	opal_call

	.globl opal_poll_events
opal_poll_events:
	li	r0, OPAL_POLL_EVENTS
	b	opal_call

	.globl opal_console_flush
opal_console_flush:
	li	r0, OPAL_CONSOLE_FLUSH
	sldi	r3,r3,32
	or	r3,r3,r4
	b	opal_call

#endif /* CONFIG_PPC64_BOOT_WRAPPER */
