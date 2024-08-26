// SPDX-License-Identifier: GPL-2.0

#include <linux/linkage.h>
#include <asm/cache.h>

	.text

/*
 * ARM64 ChaCha20 implementation meant for vDSO.  Produces a given positive
 * number of blocks of output with nonnce 0, taking an input key and 8-bytes
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

	/* v0 = "expand 32-byte k" */
	adr_l		x8, CTES
	ld1		{v5.4s}, [x8]
	/* v1,v2 = key */
	ld1		{ v6.4s, v7.4s }, [x1]
	/* v3 = counter || zero noonce  */
	ldr		d8, [x2]

	adr_l		x8, ONE
	ldr		q13, [x8]

	adr_l		x10, ROT8
	ld1		{v12.4s}, [x10]
.Lblock:
	/* copy state to auxiliary vectors for the final add after the permute.  */
	mov		v0.16b, v5.16b
	mov		v1.16b, v6.16b
	mov		v2.16b, v7.16b
	mov		v3.16b, v8.16b

	mov		w4, 20
.Lpermute:
	/*
	 * Permute one 64-byte block where the state matrix is stored in the four NEON
	 * registers v0-v3.  It performs matrix operations on four words in parallel,
	 * but requires shuffling to rearrange the words after each round.
	 */

.Ldoubleround:
	/* x0 += x1, x3 = rotl32(x3 ^ x0, 16) */
	add		v0.4s, v0.4s, v1.4s
	eor		v3.16b, v3.16b, v0.16b
	rev32		v3.8h, v3.8h

	/* x2 += x3, x1 = rotl32(x1 ^ x2, 12) */
	add		v2.4s, v2.4s, v3.4s
	eor		v4.16b, v1.16b, v2.16b
	shl		v1.4s, v4.4s, #12
	sri		v1.4s, v4.4s, #20

	/* x0 += x1, x3 = rotl32(x3 ^ x0, 8) */
	add		v0.4s, v0.4s, v1.4s
	eor		v3.16b, v3.16b, v0.16b
	tbl		v3.16b, {v3.16b}, v12.16b

	/* x2 += x3, x1 = rotl32(x1 ^ x2, 7) */
	add		v2.4s, v2.4s, v3.4s
	eor		v4.16b, v1.16b, v2.16b
	shl		v1.4s, v4.4s, #7
	sri		v1.4s, v4.4s, #25

	/* x1 = shuffle32(x1, MASK(0, 3, 2, 1)) */
	ext		v1.16b, v1.16b, v1.16b, #4
	/* x2 = shuffle32(x2, MASK(1, 0, 3, 2)) */
	ext		v2.16b, v2.16b, v2.16b, #8
	/* x3 = shuffle32(x3, MASK(2, 1, 0, 3)) */
	ext		v3.16b, v3.16b, v3.16b, #12

	/* x0 += x1, x3 = rotl32(x3 ^ x0, 16) */
	add		v0.4s, v0.4s, v1.4s
	eor		v3.16b, v3.16b, v0.16b
	rev32		v3.8h, v3.8h

	/* x2 += x3, x1 = rotl32(x1 ^ x2, 12) */
	add		v2.4s, v2.4s, v3.4s
	eor		v4.16b, v1.16b, v2.16b
	shl		v1.4s, v4.4s, #12
	sri		v1.4s, v4.4s, #20

	/* x0 += x1, x3 = rotl32(x3 ^ x0, 8) */
	add		v0.4s, v0.4s, v1.4s
	eor		v3.16b, v3.16b, v0.16b
	tbl		v3.16b, {v3.16b}, v12.16b

	/* x2 += x3, x1 = rotl32(x1 ^ x2, 7) */
	add		v2.4s, v2.4s, v3.4s
	eor		v4.16b, v1.16b, v2.16b
	shl		v1.4s, v4.4s, #7
	sri		v1.4s, v4.4s, #25

	/* x1 = shuffle32(x1, MASK(2, 1, 0, 3)) */
	ext		v1.16b, v1.16b, v1.16b, #12
	/* x2 = shuffle32(x2, MASK(1, 0, 3, 2)) */
	ext		v2.16b, v2.16b, v2.16b, #8
	/* x3 = shuffle32(x3, MASK(0, 3, 2, 1)) */
	ext		v3.16b, v3.16b, v3.16b, #4

	subs		w4, w4, #2
	b.ne		.Ldoubleround

	/* output0 = state0 + v0 */
	add		v0.4s, v0.4s, v5.4s
	/* output1 = state1 + v1 */
	add		v1.4s, v1.4s, v6.4s
	/* output2 = state2 + v2 */
	add		v2.4s, v2.4s, v7.4s
	/* output2 = state3 + v3 */
	add		v3.4s, v3.4s, v8.4s
	st1		{ v0.4s - v3.4s }, [x0]

	/* ++copy3.counter */
	add		d8, d8, d13

	/* output += 64, --nblocks */
	add		x0, x0, 64
	subs		x3, x3, #1
	b.ne		.Lblock

	/* counter = copy3.counter */
	str		d8, [x2]

	/* Zero out the potentially sensitive regs, in case nothing uses these again. */
	eor		v0.16b, v0.16b, v0.16b
	eor		v1.16b, v1.16b, v1.16b
	eor		v2.16b, v2.16b, v2.16b
	eor		v3.16b, v3.16b, v3.16b
	eor		v6.16b, v6.16b, v6.16b
	eor		v7.16b, v7.16b, v7.16b
	ret
SYM_FUNC_END(__arch_chacha20_blocks_nostack)

        .section        ".rodata", "a", %progbits
        .align          L1_CACHE_SHIFT

CTES:	.word		1634760805, 857760878, 	2036477234, 1797285236
ONE:    .xword		1, 0
ROT8:	.word		0x02010003, 0x06050407, 0x0a09080b, 0x0e0d0c0f

emit_aarch64_feature_1_and
