// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2024 Christophe Leroy <christophe.leroy@csgroup.eu>, CS GROUP France
 */

#include <linux/linkage.h>

#include <asm/ppc_asm.h>

.macro quarterround4 a1 b1 c1 d1 a2 b2 c2 d2 a3 b3 c3 d3 a4 b4 c4 d4
	add	\a1, \a1, \b1
	add	\a2, \a2, \b2
	add	\a3, \a3, \b3
	add	\a4, \a4, \b4
	xor	\d1, \d1, \a1
	xor	\d2, \d2, \a2
	xor	\d3, \d3, \a3
	xor	\d4, \d4, \a4
	rotlwi	\d1, \d1, 16
	rotlwi	\d2, \d2, 16
	rotlwi	\d3, \d3, 16
	rotlwi	\d4, \d4, 16
	add	\c1, \c1, \d1
	add	\c2, \c2, \d2
	add	\c3, \c3, \d3
	add	\c4, \c4, \d4
	xor	\b1, \b1, \c1
	xor	\b2, \b2, \c2
	xor	\b3, \b3, \c3
	xor	\b4, \b4, \c4
	rotlwi	\b1, \b1, 12
	rotlwi	\b2, \b2, 12
	rotlwi	\b3, \b3, 12
	rotlwi	\b4, \b4, 12
	add	\a1, \a1, \b1
	add	\a2, \a2, \b2
	add	\a3, \a3, \b3
	add	\a4, \a4, \b4
	xor	\d1, \d1, \a1
	xor	\d2, \d2, \a2
	xor	\d3, \d3, \a3
	xor	\d4, \d4, \a4
	rotlwi	\d1, \d1, 8
	rotlwi	\d2, \d2, 8
	rotlwi	\d3, \d3, 8
	rotlwi	\d4, \d4, 8
	add	\c1, \c1, \d1
	add	\c2, \c2, \d2
	add	\c3, \c3, \d3
	add	\c4, \c4, \d4
	xor	\b1, \b1, \c1
	xor	\b2, \b2, \c2
	xor	\b3, \b3, \c3
	xor	\b4, \b4, \c4
	rotlwi	\b1, \b1, 7
	rotlwi	\b2, \b2, 7
	rotlwi	\b3, \b3, 7
	rotlwi	\b4, \b4, 7
.endm

#define QUARTERROUND4(a1,b1,c1,d1,a2,b2,c2,d2,a3,b3,c3,d3,a4,b4,c4,d4) quarterround4 16+a1 16+b1 16+c1 16+d1 16+a2 16+b2 16+c2 16+d2 16+a3 16+b3 16+c3 16+d3 16+a4 16+b4 16+c4 16+d4

/*
 * Very basic 32 bits implementation of ChaCha20. Produces a given positive number
 * of blocks of output with a nonce of 0, taking an input key and 8-byte
 * counter. Importantly does not spill to the stack. Its arguments are:
 *
 *	r3: output bytes
 *	r4: 32-byte key input
 *	r5: 8-byte counter input/output
 *	r6: number of 64-byte blocks to write to output
 *
 *	r0: counter of blocks (initialised with r6)
 *	r4: Value '4' after key has been read.
 *	r6-r13: key
 *	r14-r15 : counter
 *	r16-r31 : state
 */
SYM_FUNC_START(__arch_chacha20_blocks_nostack)
#ifdef __powerpc64__
	std	r14,-144(r1)
	std	r15,-136(r1)
	std	r16,-128(r1)
	std	r17,-120(r1)
	std	r18,-112(r1)
	std	r19,-104(r1)
	std	r20,-96(r1)
	std	r21,-88(r1)
	std	r22,-80(r1)
	std	r23,-72(r1)
	std	r24,-64(r1)
	std	r25,-56(r1)
	std	r26,-48(r1)
	std	r27,-40(r1)
	std	r28,-32(r1)
	std	r29,-24(r1)
	std	r30,-16(r1)
	std	r31,-8(r1)
#else
	stwu	r1, -96(r1)
#if defined(CONFIG_CPU_LITTLE_ENDIAN)
	stw	r14,24(r1)
	stw	r15,28(r1)
	stw	r16,32(r1)
	stw	r17,36(r1)
	stw	r18,40(r1)
	stw	r19,44(r1)
	stw	r20,48(r1)
	stw	r21,52(r1)
	stw	r22,56(r1)
	stw	r23,60(r1)
	stw	r24,64(r1)
	stw	r25,68(r1)
	stw	r26,72(r1)
	stw	r27,76(r1)
	stw	r28,80(r1)
	stw	r29,84(r1)
	stw	r30,88(r1)
	stw	r31,92(r1)
#else
	stmw	r14, 24(r1)
#endif
#endif
	mr	r0, r6

	li	r31, 4

	LWZX_LE	r6, 0, r4
	LWZX_LE	r7, r31, r4
	addi	r4, r4, 8
	LWZX_LE	r8, 0, r4
	LWZX_LE	r9, r31, r4
	addi	r4, r4, 8
	LWZX_LE	r10, 0, r4
	LWZX_LE	r11, r31, r4
	addi	r4, r4, 8
	LWZX_LE	r12, 0, r4
	LWZX_LE	r13, r31, r4

	li	r4, 4

#ifdef __powerpc64__
	LDX_LE	r14, 0, r5
	srdi	r15, r14, 32
#else
	LWZX_LE	r14, 0, r5
	LWZX_LE	r15, r4, r5
#endif
.Lblock:
	li	r31, 10

	lis	r16, 0x6170
	lis	r17, 0x3320
	lis	r18, 0x7962
	lis	r19, 0x6b20
	addi	r16, r16, 0x7865
	addi	r17, r17, 0x646e
	addi	r18, r18, 0x2d32
	addi	r19, r19, 0x6574

	mtctr	r31

	mr	r20, r6
	mr	r21, r7
	mr	r22, r8
	mr	r23, r9
	mr	r24, r10
	mr	r25, r11
	mr	r26, r12
	mr	r27, r13

	mr	r28, r14
	mr	r29, r15
	li	r30, 0
	li	r31, 0

.Lpermute:
	QUARTERROUND4( 0, 4, 8,12, 1, 5, 9,13, 2, 6,10,14, 3, 7,11,15)
	QUARTERROUND4( 0, 5,10,15, 1, 6,11,12, 2, 7, 8,13, 3, 4, 9,14)

	bdnz	.Lpermute

	addis	r16, r16, 0x6170
	addis	r17, r17, 0x3320
	addis	r18, r18, 0x7962
	addis	r19, r19, 0x6b20
	addi	r16, r16, 0x7865
	addi	r17, r17, 0x646e
	addi	r18, r18, 0x2d32
	addi	r19, r19, 0x6574

	add	r20, r20, r6
	add	r21, r21, r7
	add	r22, r22, r8
	add	r23, r23, r9
	add	r24, r24, r10
	add	r25, r25, r11
	add	r26, r26, r12
	add	r27, r27, r13

	add	r28, r28, r14
	add	r29, r29, r15

	STWX_LE	r16, 0, r3
	STWX_LE	r17, r4, r3
	addi	r3, r3, 8
	STWX_LE	r18, 0, r3
	STWX_LE	r19, r4, r3
	addi	r3, r3, 8
	STWX_LE	r20, 0, r3
	STWX_LE	r21, r4, r3
	addi	r3, r3, 8
	STWX_LE	r22, 0, r3
	STWX_LE	r23, r4, r3
	addi	r3, r3, 8
	STWX_LE	r24, 0, r3
	STWX_LE	r25, r4, r3
	addi	r3, r3, 8
	STWX_LE	r26, 0, r3
	STWX_LE	r27, r4, r3
	addi	r3, r3, 8
	STWX_LE	r28, 0, r3
	STWX_LE	r29, r4, r3
	addi	r3, r3, 8
	STWX_LE	r30, 0, r3
	STWX_LE	r31, r4, r3
	addi	r3, r3, 8

#ifdef __powerpc64__
	addi	r14, r14, 1
	srdi	r15, r14, 32
#else
	addic	r14, r14, 1
	addze	r15, r15
#endif

	subic.	r0, r0, 1
	bne	.Lblock

	STWX_LE	r14, 0, r5
	STWX_LE	r15, r4, r5

	li	r6, 0
	li	r7, 0
	li	r8, 0
	li	r9, 0
	li	r10, 0
	li	r11, 0
	li	r12, 0
	li	r13, 0

#ifdef __powerpc64__
	ld	r14,-144(r1)
	ld	r15,-136(r1)
	ld	r16,-128(r1)
	ld	r17,-120(r1)
	ld	r18,-112(r1)
	ld	r19,-104(r1)
	ld	r20,-96(r1)
	ld	r21,-88(r1)
	ld	r22,-80(r1)
	ld	r23,-72(r1)
	ld	r24,-64(r1)
	ld	r25,-56(r1)
	ld	r26,-48(r1)
	ld	r27,-40(r1)
	ld	r28,-32(r1)
	ld	r29,-24(r1)
	ld	r30,-16(r1)
	ld	r31,-8(r1)
#else
#if defined(CONFIG_CPU_LITTLE_ENDIAN)
	lwz	r14,24(r1)
	lwz	r15,28(r1)
	lwz	r16,32(r1)
	lwz	r17,36(r1)
	lwz	r18,40(r1)
	lwz	r19,44(r1)
	lwz	r20,48(r1)
	lwz	r21,52(r1)
	lwz	r22,56(r1)
	lwz	r23,60(r1)
	lwz	r24,64(r1)
	lwz	r25,68(r1)
	lwz	r26,72(r1)
	lwz	r27,76(r1)
	lwz	r28,80(r1)
	lwz	r29,84(r1)
	lwz	r30,88(r1)
	lwz	r31,92(r1)
#else
	lmw	r14, 24(r1)
#endif
	addi	r1, r1, 96
#endif
	blr
SYM_FUNC_END(__arch_chacha20_blocks_nostack)
