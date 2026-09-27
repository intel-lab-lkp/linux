// SPDX-License-Identifier: GPL-2.0-only
/*
 * Fused copy + Internet checksum for arm64: NEON path
 *
 * Copyright (C) 2026 Demian Shulhan <demyansh@gmail.com>
 *
 * This file is compiled with CC_FLAGS_FPU (see Makefile) and must only be
 * entered between kernel_neon_begin() and kernel_neon_end(). Everything
 * else about the contract is in csum-copy.h.
 *
 * The routine runs to completion without chunking or yielding regardless
 * of @len. In task context kernel-mode NEON is preemptible (the caller's
 * FPSIMD state lives in a caller-provided buffer that is switched with the
 * task). In softirq context preemption is off, but the lengths that reach
 * this function are bounded by a single skb fragment or iov segment, i.e.
 * tens of KiB at the very most, which is a few microseconds here and well
 * within what the surrounding network softirq processing already spends.
 *
 * Main loop: 64 bytes per iteration. Each 16-byte vector is reduced with
 * UADDLP (8x16 -> 4x32); pairs of vectors share a 32-bit accumulator via
 * UADALP, which is then drained into a 64-bit accumulator. This is six
 * vector ALU operations per iteration; giving every vector its own 64-bit
 * accumulator would need eight and measures ~10% slower, as the loop is
 * bound by vector ALU throughput rather than by dependency latency. A
 * 32-bit lane only ever holds the sum of four 16-bit words, so it cannot
 * overflow, and the 64-bit lanes cannot overflow for any 'int' length.
 * Two independent 64-bit accumulators keep the loop-carried dependency to
 * a single UADALP each. The dispatcher only enters this path for
 * len >= CSUM_COPY_NEON_MIN_LEN, so the loop is entered unconditionally.
 *
 * Head: misaligned 16-byte stores are markedly more expensive than
 * misaligned loads, so @dst is aligned to 16 bytes first by copying one
 * full vector and masking the checksum contribution down to the leading
 * 'head' bytes. The main loop then runs on the byte grid starting at
 * @src + head; if head is odd, that grid is offset from the 16-bit word
 * grid of the buffer. Rather than fixing up every loop vector, the head
 * vector is byte-swapped within each lane (REV16) to match the loop grid
 * and the final folded 16-bit sum is rotated by 8 bits, which is exact for
 * ones' complement sums (the same trick as csum_block_add()/do_csum()).
 *
 * Tail (1..15 bytes): rather than dribbling bytes through GPRs, copy the
 * last 16 bytes of the buffer as one vector and mask off the bytes that
 * belong to the body. That vector overlaps up to 15 body bytes, so it is
 * copied *before* the body: the body's stores then overwrite the overlap
 * with the very bytes that were summed, and the destination always matches
 * the returned checksum even if the source is modified concurrently. If the
 * tail length is odd the tail vector is offset by one byte from the loop
 * grid, which is again corrected with REV16.
 *
 * Data is handled as uint8x16_t throughout: that is how it sits in memory,
 * and masking and REV16 are byte operations. The 16-bit word view only
 * appears in the two pairwise-add helpers.
 */

#include <linux/types.h>
#include <asm/neon-intrinsics.h>

#include "csum-copy.h"

/*
 * Byte mask table. A 16-byte load at offset n (1..15) yields (16 - n) zero
 * bytes followed by n 0xff bytes, selecting the last n bytes of a vector;
 * a load at offset 32 - n yields n 0xff bytes followed by zeros, selecting
 * the first n bytes. One unaligned load, no lane-index compare.
 */
static const u8 csum_copy_mask[48] __aligned(16) = {
	[16 ... 31] = 0xff,
};

/* Adjacent 16-bit words of @v summed into 32-bit lanes (UADDLP). */
static __always_inline uint32x4_t words_sum(uint8x16_t v)
{
	return vpaddlq_u16(vreinterpretq_u16_u8(v));
}

/* @acc += adjacent 16-bit words of @v (UADALP). */
static __always_inline uint32x4_t words_add(uint32x4_t acc, uint8x16_t v)
{
	return vpadalq_u16(acc, vreinterpretq_u16_u8(v));
}

__wsum __csum_partial_copy_neon(const void *src, void *dst, int len)
{
	const u8 *s = src;
	u8 *d = dst;
	uint64x2_t acc0 = { }, acc1 = { };
	unsigned int head = -(unsigned long)d & 15;
	unsigned int tail = (len - head) & 15;
	u32 sum;

	if (tail) {
		/* Final vector, ending exactly at the buffer end; see above */
		uint8x16_t v = vld1q_u8(s + len - 16);

		vst1q_u8(d + len - 16, v);
		v &= vld1q_u8(csum_copy_mask + tail);
		if (tail & 1)
			v = vrev16q_u8(v);	/* odd tail: match loop grid */
		acc1 = vpadalq_u32(acc1, words_sum(v));
	}

	if (head) {
		uint8x16_t v = vld1q_u8(s);

		vst1q_u8(d, v);
		v &= vld1q_u8(csum_copy_mask + 32 - head);
		if (head & 1)
			v = vrev16q_u8(v);	/* odd head: match loop grid */
		acc0 = vpadalq_u32(acc0, words_sum(v));

		s += head;
		d += head;
		len -= head;
	}

	do {
		uint8x16_t v0 = vld1q_u8(s);
		uint8x16_t v1 = vld1q_u8(s + 16);
		uint8x16_t v2 = vld1q_u8(s + 32);
		uint8x16_t v3 = vld1q_u8(s + 48);
		uint32x4_t t0, t1;

		vst1q_u8(d, v0);
		vst1q_u8(d + 16, v1);
		vst1q_u8(d + 32, v2);
		vst1q_u8(d + 48, v3);

		t0 = words_sum(v0);
		t1 = words_sum(v1);
		t0 = words_add(t0, v2);
		t1 = words_add(t1, v3);
		acc0 = vpadalq_u32(acc0, t0);
		acc1 = vpadalq_u32(acc1, t1);

		s += 64;
		d += 64;
		len -= 64;
	} while (len >= 64);

	if (len & 32) {
		uint8x16_t v0 = vld1q_u8(s);
		uint8x16_t v1 = vld1q_u8(s + 16);

		vst1q_u8(d, v0);
		vst1q_u8(d + 16, v1);
		acc0 = vpadalq_u32(acc0, words_add(words_sum(v0), v1));

		s += 32;
		d += 32;
	}
	if (len & 16) {
		uint8x16_t v = vld1q_u8(s);

		vst1q_u8(d, v);
		acc1 = vpadalq_u32(acc1, words_sum(v));
	}

	sum = (__force u32)csum_fold64(vaddvq_u64(acc0 + acc1));

	if (head & 1) {
		/* Fold to 16 bits and rotate back onto the buffer's word grid */
		sum = (sum & 0xffff) + (sum >> 16);
		sum = (sum & 0xffff) + (sum >> 16);
		sum = ((sum & 0xff) << 8) | (sum >> 8);
	}

	return (__force __wsum)sum;
}
