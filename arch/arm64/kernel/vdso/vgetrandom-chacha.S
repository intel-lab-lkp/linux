// SPDX-License-Identifier: GPL-2.0

#include <linux/linkage.h>
#include <asm/cache.h>
#include <asm/assembler.h>

	.text

#define state0		v0
#define state1		v1
#define state2		v2
#define state3		v3
#define copy0		v4
#define copy1		v5
#define copy2		v6
#define copy3		v7
#define copy3_d		d7
#define one_d		d16
#define one_q		q16
#define tmp		v17
#define rot8		v18

/*
 * ARM64 ChaCha20 implementation meant for vDSO.  Produces a given positive
 * number of blocks of output with nonce 0, taking an input key and 8-bytes
 * counter.  Importantly does not spill to the stack.
 *
 * void __arch_chacha20_blocks_nostack(uint8_t *dst_bytes,
 *				       const uint8_t *key,
 * 				       uint32_t *counter,
 *				       size_t nblocks)
 *
 * 	x0: output bytes
 *	x1: 32-byte key input
 *	x2: 8-byte counter input/output
 *	x3: number of 64-byte block to write to output
 */
SYM_FUNC_START(__arch_chacha20_blocks_nostack)

	/* copy0 = "expand 32-byte k" */
	adr_l		x8, CTES
	ld1		{copy0.4s}, [x8]
	/* copy1,copy2 = key */
	ld1		{ copy1.4s, copy2.4s }, [x1]
	/* copy3 = counter || zero nonce  */
	ldr		copy3_d, [x2]

	adr_l		x8, ONE
	ldr		one_q, [x8]

	adr_l		x10, ROT8
	ld1		{rot8.4s}, [x10]
.Lblock:
	/* copy state to auxiliary vectors for the final add after the permute.  */
	mov		state0.16b, copy0.16b
	mov		state1.16b, copy1.16b
	mov		state2.16b, copy2.16b
	mov		state3.16b, copy3.16b

	mov		w4, 20
.Lpermute:
	/*
	 * Permute one 64-byte block where the state matrix is stored in the four NEON
	 * registers state0-state3.  It performs matrix operations on four words in parallel,
	 * but requires shuffling to rearrange the words after each round.
	 */

.Ldoubleround:
	/* state0 += state1, state3 = rotl32(state3 ^ state0, 16) */
	add		state0.4s, state0.4s, state1.4s
	eor		state3.16b, state3.16b, state0.16b
	rev32		state3.8h, state3.8h

	/* state2 += state3, state1 = rotl32(state1 ^ state2, 12) */
	add		state2.4s, state2.4s, state3.4s
	eor		tmp.16b, state1.16b, state2.16b
	shl		state1.4s, tmp.4s, #12
	sri		state1.4s, tmp.4s, #20

	/* state0 += state1, state3 = rotl32(state3 ^ state0, 8) */
	add		state0.4s, state0.4s, state1.4s
	eor		state3.16b, state3.16b, state0.16b
	tbl		state3.16b, {state3.16b}, rot8.16b

	/* state2 += state3, state1 = rotl32(state1 ^ state2, 7) */
	add		state2.4s, state2.4s, state3.4s
	eor		tmp.16b, state1.16b, state2.16b
	shl		state1.4s, tmp.4s, #7
	sri		state1.4s, tmp.4s, #25

	/* state1[0,1,2,3] = state1[1,2,3,0] */
	ext		state1.16b, state1.16b, state1.16b, #4
	/* state2[0,1,2,3] = state2[2,3,0,1] */
	ext		state2.16b, state2.16b, state2.16b, #8
	/* state3[0,1,2,3] = state3[1,2,3,0] */
	ext		state3.16b, state3.16b, state3.16b, #12

	/* state0 += state1, state3 = rotl32(state3 ^ state0, 16) */
	add		state0.4s, state0.4s, state1.4s
	eor		state3.16b, state3.16b, state0.16b
	rev32		state3.8h, state3.8h

	/* state2 += state3, state1 = rotl32(state1 ^ state2, 12) */
	add		state2.4s, state2.4s, state3.4s
	eor		tmp.16b, state1.16b, state2.16b
	shl		state1.4s, tmp.4s, #12
	sri		state1.4s, tmp.4s, #20

	/* state0 += state1, state3 = rotl32(state3 ^ state0, 8) */
	add		state0.4s, state0.4s, state1.4s
	eor		state3.16b, state3.16b, state0.16b
	tbl		state3.16b, {state3.16b}, rot8.16b

	/* state2 += state3, state1 = rotl32(state1 ^ state2, 7) */
	add		state2.4s, state2.4s, state3.4s
	eor		tmp.16b, state1.16b, state2.16b
	shl		state1.4s, tmp.4s, #7
	sri		state1.4s, tmp.4s, #25

	/* state1[0,1,2,3] = state1[3,0,1,2] */
	ext		state1.16b, state1.16b, state1.16b, #12
	/* state2[0,1,2,3] = state2[2,3,0,1] */
	ext		state2.16b, state2.16b, state2.16b, #8
	/* state3[0,1,2,3] = state3[1,2,3,0] */
	ext		state3.16b, state3.16b, state3.16b, #4

	subs		w4, w4, #2
	b.ne		.Ldoubleround

	/* output0 = state0 + state0 */
	add		state0.4s, state0.4s, copy0.4s
	/* output1 = state1 + state1 */
	add		state1.4s, state1.4s, copy1.4s
	/* output2 = state2 + state2 */
	add		state2.4s, state2.4s, copy2.4s
	/* output2 = state3 + state3 */
	add		state3.4s, state3.4s, copy3.4s
	st1		{ state0.4s - state3.4s }, [x0]

	/* ++copy3.counter */
	add		copy3_d, copy3_d, one_d

	/* output += 64, --nblocks */
	add		x0, x0, 64
	subs		x3, x3, #1
	b.ne		.Lblock

	/* counter = copy3.counter */
	str		copy3_d, [x2]

	/* Zero out the potentially sensitive regs, in case nothing uses these again. */
	eor		state0.16b, state0.16b, state0.16b
	eor		state1.16b, state1.16b, state1.16b
	eor		state2.16b, state2.16b, state2.16b
	eor		state3.16b, state3.16b, state3.16b
	eor		copy1.16b, copy1.16b, copy1.16b
	eor		copy2.16b, copy2.16b, copy2.16b
	ret
SYM_FUNC_END(__arch_chacha20_blocks_nostack)

        .section        ".rodata", "a", %progbits
        .align          L1_CACHE_SHIFT

CTES:	.word		1634760805, 857760878, 	2036477234, 1797285236
ONE:    .xword		1, 0
ROT8:	.word		0x02010003, 0x06050407, 0x0a09080b, 0x0e0d0c0f

emit_aarch64_feature_1_and
