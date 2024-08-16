// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2024 Xi Ruoyao <xry111@xry111.site>. All Rights Reserved.
 *
 * Based on arch/x86/entry/vdso/vgetrandom-chacha.S:
 *
 * Copyright (C) 2022-2024 Jason A. Donenfeld <Jason@zx2c4.com>. All Rights
 * Reserved.
 */

#include <asm/asm.h>
#include <asm/regdef.h>
#include <linux/linkage.h>

.section	.rodata
.align 4
CONSTANTS:	.octa 0x6b20657479622d323320646e61707865

.text

/*
 * Loongson SIMD eXtension implementation of ChaCha20. Produces a given
 * positive number of blocks of output with a nonce of 0, taking an input
 * key and 8-byte counter. Importantly does not spill to the stack. Its
 * arguments are:
 *
 *	a0: output bytes
 *	a1: 32-byte key input
 *	a2: 8-byte counter input/output
 *	a3: number of 64-byte blocks to write to output
 */
SYM_FUNC_START(__arch_chacha20_blocks_nostack_lsx)
#define output		a0
#define key		a1
#define counter		a2
#define nblocks		a3
#define i		t0
/* LSX registers vr0-vr23 are caller-save. */
#define state0		$vr0
#define state1		$vr1
#define state2		$vr2
#define state3		$vr3
#define copy0		$vr4
#define copy1		$vr5
#define copy2		$vr6
#define copy3		$vr7
#define one		$vr8

	/* copy0 = "expand 32-byte k" */
	la.pcrel	t1, CONSTANTS
	vld		copy0, t1, 0
	/* copy1, copy2 = key */
	vld		copy1, key, 0
	vld		copy2, key, 0x10
	/* copy3 = counter || zero nonce */
	vldrepl.d	copy3, counter, 0
	vinsgr2vr.d	copy3, zero, 1
	/* one = 1 || 0 */
	vldi		one, 0b0110000000001
	vinsgr2vr.d	one, zero, 1

.Lblock:
	/* state = copy */
	vori.b		state0, copy0, 0
	vori.b		state1, copy1, 0
	vori.b		state2, copy2, 0
	vori.b		state3, copy3, 0

	li.w		i, 10
.Lpermute:
	/* state0 += state1, state3 = rotl32(state3 ^ state0, 16) */
	vadd.w		state0, state0, state1
	vxor.v		state3, state3, state0
	vrotri.w	state3, state3, 16

	/* state2 += state3, state1 = rotl32(state1 ^ state2, 12) */
	vadd.w		state2, state2, state3
	vxor.v		state1, state1, state2
	vrotri.w	state1, state1, 20

	/* state0 += state1, state3 = rotl32(state3 ^ state0, 8) */
	vadd.w		state0, state0, state1
	vxor.v		state3, state3, state0
	vrotri.w	state3, state3, 24

	/* state2 += state3, state1 = rotl32(state1 ^ state2, 7) */
	vadd.w		state2, state2, state3
	vxor.v		state1, state1, state2
	vrotri.w	state1, state1, 25

	/* state1[0,1,2,3] = state1[1,2,3,0] */
	vshuf4i.w	state1, state1, 0b00111001
	/* state2[0,1,2,3] = state2[2,3,0,1] */
	vshuf4i.w	state2, state2, 0b01001110
	/* state3[0,1,2,3] = state3[1,2,3,0] */
	vshuf4i.w	state3, state3, 0b10010011

	/* state0 += state1, state3 = rotl32(state3 ^ state0, 16) */
	vadd.w		state0, state0, state1
	vxor.v		state3, state3, state0
	vrotri.w	state3, state3, 16

	/* state2 += state3, state1 = rotl32(state1 ^ state2, 12) */
	vadd.w		state2, state2, state3
	vxor.v		state1, state1, state2
	vrotri.w	state1, state1, 20

	/* state0 += state1, state3 = rotl32(state3 ^ state0, 8) */
	vadd.w		state0, state0, state1
	vxor.v		state3, state3, state0
	vrotri.w	state3, state3, 24

	/* state2 += state3, state1 = rotl32(state1 ^ state2, 7) */
	vadd.w		state2, state2, state3
	vxor.v		state1, state1, state2
	vrotri.w	state1, state1, 25

	/* state1[0,1,2,3] = state1[3,0,1,2] */
	vshuf4i.w	state1, state1, 0b10010011
	/* state2[0,1,2,3] = state2[2,3,0,1] */
	vshuf4i.w	state2, state2, 0b01001110
	/* state3[0,1,2,3] = state3[1,2,3,0] */
	vshuf4i.w	state3, state3, 0b00111001

	addi.w		i, i, -1
	bnez		i, .Lpermute

	/* output0 = state0 + copy0 */
	vadd.w		state0, state0, copy0
	vst		state0, output, 0
	/* output1 = state1 + copy1 */
	vadd.w		state1, state1, copy1
	vst		state1, output, 0x10
	/* output2 = state2 + copy2 */
	vadd.w		state2, state2, copy2
	vst		state2, output, 0x20
	/* output3 = state3 + copy3 */
	vadd.w		state3, state3, copy3
	vst		state3, output, 0x30

	/* ++copy3.counter */
	vadd.d		copy3, copy3, one

	/* output += 64 */
	PTR_ADDI	output, output, 64
	/* --nblocks */
	PTR_ADDI	nblocks, nblocks, -1
	bnez		nblocks, .Lblock

	/* counter = copy3.counter */
	vstelm.d	copy3, counter, 0, 0

	/* Zero out the potentially sensitive regs, in case nothing uses these again. */
	vldi		state0, 0
	vldi		state1, 0
	vldi		state2, 0
	vldi		state3, 0
	vldi		copy1, 0
	vldi		copy2, 0

	jr		ra
SYM_FUNC_END(__arch_chacha20_blocks_nostack_lsx)
