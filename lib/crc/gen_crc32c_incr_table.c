// SPDX-License-Identifier: GPL-2.0
/*
 * Generate GF(2) nibble-based lookup tables for incremental CRC32c updates.
 * MAX_ORDER 19 supports up to 64KB buffers (2^19 bits = 524288 bits).
 *
 * Instead of storing raw 32x32 bit matrices (32 rows per order),
 * we precompute nibble (4-bit) indexed tables.  This reduces gf2_xform
 * to 8 table lookups instead of 32 branchless mask-and-XOR iterations.
 *
 * Memory layout:
 * - crc32c_incr_nibble_table[19][8][16]: 19 * 8 * 16 * 4 = 9728 bytes
 * - crc32c_incr_ones_lookup[20]:         20 * 4          = 80   bytes
 * Total: ~9.8KB (fits comfortably in L1 cache)
 *
 * Copyright (C) 2026 Alibaba Inc.
 */

#include <stdio.h>
#include <inttypes.h>

#include "../../include/linux/crc32poly.h"

#define CRC32C_INCR_MAX_ORDER	19
#define NIBBLES_PER_U32		8

static uint32_t bit_matrix[CRC32C_INCR_MAX_ORDER][32];
static uint32_t nibble_table[CRC32C_INCR_MAX_ORDER][NIBBLES_PER_U32][16];
static uint32_t ones_lookup[CRC32C_INCR_MAX_ORDER + 1];

static void crc32c_incr_init(void)
{
	int n, i, k, v;

	/*
	 * Step 1: Build the order-0 matrix M, where M[i] is the CRC
	 * state after shifting basis vector e_i by one bit position.
	 */
	for (i = 0; i < 32; i++) {
		uint32_t r = 1U << i;

		bit_matrix[0][i] = (r & 1) ?
			(r >> 1) ^ CRC32C_POLY_LE : (r >> 1);
	}

	/* Step 2: M^(2^n) = (M^(2^(n-1)))^2 via matrix squaring */
	for (n = 1; n < CRC32C_INCR_MAX_ORDER; n++) {
		for (i = 0; i < 32; i++) {
			uint32_t r = bit_matrix[n - 1][i];
			uint32_t res = 0;

			for (k = 0; k < 32; k++) {
				if (r & (1U << k))
					res ^= bit_matrix[n - 1][k];
			}
			bit_matrix[n][i] = res;
		}
	}

	/* Step 3: Convert bit matrices to nibble-indexed lookup tables */
	for (n = 0; n < CRC32C_INCR_MAX_ORDER; n++) {
		for (i = 0; i < NIBBLES_PER_U32; i++) {
			nibble_table[n][i][0] = 0;
			for (v = 1; v < 16; v++) {
				uint32_t res = 0;

				for (k = 0; k < 4; k++) {
					if (v & (1 << k))
						res ^= bit_matrix[n][i * 4 + k];
				}
				nibble_table[n][i][v] = res;
			}
		}
	}

	/*
	 * Step 4: ones_lookup[n] = CRC(0, all-ones of 2^n bits).
	 * Uses CRC(A||B) = shift(CRC(A), len(B)) ^ CRC(B) to double
	 * the length at each step.  ones_lookup[0] = CRC of a single
	 * 1-bit, which equals the generator polynomial.
	 */
	ones_lookup[0] = CRC32C_POLY_LE;

	for (n = 1; n <= CRC32C_INCR_MAX_ORDER; n++) {
		uint32_t low = ones_lookup[n - 1];
		uint32_t high = 0;

		for (k = 0; k < 32; k++) {
			if (low & (1U << k))
				high ^= bit_matrix[n - 1][k];
		}
		ones_lookup[n] = low ^ high;
	}
}

int main(int argc, char **argv)
{
	int n, i, v;

	crc32c_incr_init();

	printf("/* this file is generated - do not edit */\n\n");

	printf("static const u32 crc32c_incr_nibble_table[%d][%d][16] = {\n",
	       CRC32C_INCR_MAX_ORDER, NIBBLES_PER_U32);
	for (n = 0; n < CRC32C_INCR_MAX_ORDER; n++) {
		printf("\t{\n");
		for (i = 0; i < NIBBLES_PER_U32; i++) {
			printf("\t\t{\n");
			for (v = 0; v < 16; v += 4) {
				printf("\t\t\t0x%08x, 0x%08x, 0x%08x, 0x%08x,\n",
				       nibble_table[n][i][v],
				       nibble_table[n][i][v + 1],
				       nibble_table[n][i][v + 2],
				       nibble_table[n][i][v + 3]);
			}
			printf("\t\t},\n");
		}
		printf("\t},\n");
	}
	printf("};\n\n");

	printf("static const u32 crc32c_incr_ones_lookup[%d] = {\n",
	       CRC32C_INCR_MAX_ORDER + 1);
	for (n = 0; n <= CRC32C_INCR_MAX_ORDER; n += 4) {
		int remaining = CRC32C_INCR_MAX_ORDER + 1 - n;

		if (remaining >= 4) {
			printf("\t0x%08x, 0x%08x, 0x%08x, 0x%08x,\n",
			       ones_lookup[n], ones_lookup[n + 1],
			       ones_lookup[n + 2], ones_lookup[n + 3]);
		} else {
			printf("\t");
			for (i = 0; i < remaining; i++)
				printf("0x%08x, ", ones_lookup[n + i]);
			printf("\n");
		}
	}
	printf("};\n");

	return 0;
}
