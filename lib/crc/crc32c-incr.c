// SPDX-License-Identifier: GPL-2.0-only
/*
 * GF(2) matrix-based CRC32c incremental update.
 *
 * When a contiguous range of bits is flipped, the new CRC can be
 * derived from the old one without re-scanning the buffer:
 *   New_CRC = Old_CRC ^ CRC(flip_mask << trailing_bits)
 *
 * The delta CRC is computed by decomposing num_bits and trailing_bits
 * into power-of-2 components and combining them via the CRC
 * concatenation property, giving O(log N) complexity.
 *
 * Memory usage: ~9.8KB
 * - crc32c_incr_nibble_table: 19 * 8 * 16 * 4 = 9728 bytes
 * - crc32c_incr_ones_lookup:  20 * 4          = 80   bytes
 *
 * Tables are generated at compile time by gen_crc32c_incr_table.
 * INCR_MAX_ORDER 19 supports up to 64KB buffers (2^19 bits).
 *
 * Copyright (C) 2026 Alibaba Inc.
 */

#include <linux/bitops.h>
#include <linux/bug.h>
#include <linux/export.h>
#include <linux/crc32.h>

#include "crc32c-incr-table.h"

#define INCR_MAX_ORDER		19

/**
 * gf2_xform - Multiply a CRC state vector by a GF(2) shift matrix
 * @order: Selects the precomputed matrix M^(2^order).
 * @v: The 32-bit CRC state vector.
 *
 * Computes v * M^(2^order) using nibble (4-bit) indexed tables,
 * reducing the operation from 32 bit-level iterations to 8 lookups.
 */
static inline u32 gf2_xform(int order, u32 v)
{
	const u32 (*tab)[16] = crc32c_incr_nibble_table[order];

	return tab[0][v & 0xf] ^
	       tab[1][(v >> 4) & 0xf] ^
	       tab[2][(v >> 8) & 0xf] ^
	       tab[3][(v >> 12) & 0xf] ^
	       tab[4][(v >> 16) & 0xf] ^
	       tab[5][(v >> 20) & 0xf] ^
	       tab[6][(v >> 24) & 0xf] ^
	       tab[7][(v >> 28) & 0xf];
}

/**
 * crc32c_incr_get_ones_delta - Compute CRC of an all-ones bit sequence
 * @num_bits: Length of the all-ones sequence.
 *
 * Returns CRC(0, [111...1] of length num_bits).  Decomposes num_bits
 * into powers of 2 (MSB-first) and combines using:
 *   CRC(A || B) = shift(CRC(A), len(B)) ^ CRC(B)
 *
 * This requires only (popcount - 1) gf2_xform calls, each doing
 * 8 table lookups.
 *
 * Caller must ensure num_bits <= (1UL << INCR_MAX_ORDER).
 */
static u32 crc32c_incr_get_ones_delta(size_t num_bits)
{
	u32 delta;
	int n;

	if (!num_bits)
		return 0;

	/* Initialize with the highest power-of-2 block */
	n = __fls(num_bits);
	delta = crc32c_incr_ones_lookup[n];
	num_bits ^= (1UL << n);

	/* Concatenate remaining blocks from high to low */
	while (num_bits) {
		n = __fls(num_bits);
		delta = gf2_xform(n, delta);
		delta ^= crc32c_incr_ones_lookup[n];
		num_bits ^= (1UL << n);
	}
	return delta;
}

/**
 * gf2_shift_crc - Shift a CRC state by @trailing_bits zero-bit positions
 * @crc: The CRC state vector.
 * @trailing_bits: Number of zero bits to shift through.
 *
 * Equivalent to appending @trailing_bits zero bits to the data stream
 * and continuing the CRC computation.  Decomposes trailing_bits into
 * powers of 2 and applies the corresponding precomputed matrices.
 */
static u32 gf2_shift_crc(u32 crc, size_t trailing_bits)
{
	int n;

	for (n = 0; trailing_bits > 0 && n < INCR_MAX_ORDER; n++) {
		if (trailing_bits & 1)
			crc = gf2_xform(n, crc);
		trailing_bits >>= 1;
	}
	return crc;
}

/* See full kernel-doc in include/linux/crc32.h */
u32 crc32c_flip_range(u32 old_crc, u32 total_bits,
		      u32 bit_off, u32 nbits)
{
	u32 delta, trailing_bits;

	if (!nbits)
		return old_crc;

	/*
	 * total_bits must not exceed 2^INCR_MAX_ORDER bits (64KB).
	 * bit_off + nbits must not exceed total_bits.
	 */
	if (WARN_ON_ONCE(total_bits > (1UL << INCR_MAX_ORDER)))
		return old_crc;
	if (WARN_ON_ONCE(bit_off + nbits > total_bits))
		return old_crc;

	trailing_bits = total_bits - (bit_off + nbits);

	/* 1. Calculate CRC of the flip-mask (all 1s of length nbits) */
	delta = crc32c_incr_get_ones_delta(nbits);

	/* 2. Shift the mask-CRC to the correct bit position */
	delta = gf2_shift_crc(delta, trailing_bits);

	/* 3. Apply the delta to the existing CRC */
	return old_crc ^ delta;
}
EXPORT_SYMBOL(crc32c_flip_range);
