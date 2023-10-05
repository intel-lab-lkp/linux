// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * AMD Address Translation Library
 *
 * dehash.c : Functions to account for hashing bits
 *
 * Copyright (c) 2023, Advanced Micro Devices, Inc.
 * All Rights Reserved.
 *
 * Author: Yazen Ghannam <Yazen.Ghannam@amd.com>
 */

#include "internal.h"

static int df2_dehash_addr(struct addr_ctx *ctx)
{
	u8 hashed_bit, intlv_bit;

	/* Assert that interleave bit is 8 or 9. */
	if (ctx->map.intlv_bit_pos != 8 && ctx->map.intlv_bit_pos != 9) {
		pr_warn("%s: Invalid interleave bit: %u",
			__func__, ctx->map.intlv_bit_pos);
		return -EINVAL;
	}

	/* Assert that die and socket interleaving are disabled. */
	if (ctx->map.num_intlv_dies > 1) {
		pr_warn("%s: Invalid number of interleave dies: %u",
			__func__, ctx->map.num_intlv_dies);
		return -EINVAL;
	}

	if (ctx->map.num_intlv_sockets > 1) {
		pr_warn("%s: Invalid number of interleave sockets: %u",
			__func__, ctx->map.num_intlv_sockets);
		return -EINVAL;
	}

	if (ctx->map.intlv_bit_pos == 8)
		intlv_bit = FIELD_GET(BIT_ULL(8), ctx->ret_addr);
	else
		intlv_bit = FIELD_GET(BIT_ULL(9), ctx->ret_addr);

	hashed_bit = intlv_bit;
	hashed_bit ^= FIELD_GET(BIT_ULL(12), ctx->ret_addr);
	hashed_bit ^= FIELD_GET(BIT_ULL(18), ctx->ret_addr);
	hashed_bit ^= FIELD_GET(BIT_ULL(21), ctx->ret_addr);
	hashed_bit ^= FIELD_GET(BIT_ULL(30), ctx->ret_addr);

	if (hashed_bit != intlv_bit) {
		if (ctx->map.intlv_bit_pos == 8)
			ctx->ret_addr ^= BIT_ULL(8);
		else
			ctx->ret_addr ^= BIT_ULL(9);
	}

	return 0;
}

static int df3_dehash_addr(struct addr_ctx *ctx)
{
	bool hash_ctl_64k, hash_ctl_2M, hash_ctl_1G;
	u8 hashed_bit, intlv_bit;

	/* Assert that interleave bit is 8 or 9. */
	if (ctx->map.intlv_bit_pos != 8 && ctx->map.intlv_bit_pos != 9) {
		pr_warn("%s: Invalid interleave bit: %u",
			__func__, ctx->map.intlv_bit_pos);
		return -EINVAL;
	}

	/* Assert that die and socket interleaving are disabled. */
	if (ctx->map.num_intlv_dies > 1) {
		pr_warn("%s: Invalid number of interleave dies: %u",
			__func__, ctx->map.num_intlv_dies);
		return -EINVAL;
	}

	if (ctx->map.num_intlv_sockets > 1) {
		pr_warn("%s: Invalid number of interleave sockets: %u",
			__func__, ctx->map.num_intlv_sockets);
		return -EINVAL;
	}

	hash_ctl_64k	= FIELD_GET(DF3_HASH_CTL_64K, ctx->map.ctl);
	hash_ctl_2M	= FIELD_GET(DF3_HASH_CTL_2M, ctx->map.ctl);
	hash_ctl_1G	= FIELD_GET(DF3_HASH_CTL_1G, ctx->map.ctl);

	if (ctx->map.intlv_bit_pos == 8)
		intlv_bit = FIELD_GET(BIT_ULL(8), ctx->ret_addr);
	else
		intlv_bit = FIELD_GET(BIT_ULL(9), ctx->ret_addr);

	hashed_bit = intlv_bit;
	hashed_bit ^= FIELD_GET(BIT_ULL(14), ctx->ret_addr);
	hashed_bit ^= FIELD_GET(BIT_ULL(18), ctx->ret_addr) & hash_ctl_64k;
	hashed_bit ^= FIELD_GET(BIT_ULL(23), ctx->ret_addr) & hash_ctl_2M;
	hashed_bit ^= FIELD_GET(BIT_ULL(32), ctx->ret_addr) & hash_ctl_1G;

	if (hashed_bit != intlv_bit) {
		if (ctx->map.intlv_bit_pos == 8)
			ctx->ret_addr ^= BIT_ULL(8);
		else
			ctx->ret_addr ^= BIT_ULL(9);
	}

	/* Calculation complete for 2 channels. Continue for 4 and 8 channels. */
	if (ctx->map.intlv_mode == DF3_COD4_2CHAN_HASH)
		return 0;

	intlv_bit = FIELD_GET(BIT_ULL(12), ctx->ret_addr);

	hashed_bit = intlv_bit;
	hashed_bit ^= FIELD_GET(BIT_ULL(16), ctx->ret_addr) & hash_ctl_64k;
	hashed_bit ^= FIELD_GET(BIT_ULL(21), ctx->ret_addr) & hash_ctl_2M;
	hashed_bit ^= FIELD_GET(BIT_ULL(30), ctx->ret_addr) & hash_ctl_1G;

	if (hashed_bit != intlv_bit)
		ctx->ret_addr ^= BIT_ULL(12);

	/* Calculation complete for 4 channels. Continue for 8 channels. */
	if (ctx->map.intlv_mode == DF3_COD2_4CHAN_HASH)
		return 0;

	intlv_bit = FIELD_GET(BIT_ULL(13), ctx->ret_addr);

	hashed_bit = intlv_bit;
	hashed_bit ^= FIELD_GET(BIT_ULL(17), ctx->ret_addr) & hash_ctl_64k;
	hashed_bit ^= FIELD_GET(BIT_ULL(22), ctx->ret_addr) & hash_ctl_2M;
	hashed_bit ^= FIELD_GET(BIT_ULL(31), ctx->ret_addr) & hash_ctl_1G;

	if (hashed_bit != intlv_bit)
		ctx->ret_addr ^= BIT_ULL(13);

	return 0;
}

static int df3_6chan_dehash_addr(struct addr_ctx *ctx)
{
	u8 intlv_bit_pos = ctx->map.intlv_bit_pos;
	u8 hashed_bit, intlv_bit, num_intlv_bits;
	bool hash_ctl_2M, hash_ctl_1G;

	if (ctx->map.intlv_mode != DF3_6CHAN)
		return -EINVAL;

	num_intlv_bits = ilog2(ctx->map.num_intlv_chan) + 1;

	hash_ctl_2M	= FIELD_GET(DF3_HASH_CTL_2M, ctx->map.ctl);
	hash_ctl_1G	= FIELD_GET(DF3_HASH_CTL_1G, ctx->map.ctl);

	intlv_bit = atl_get_bit(intlv_bit_pos, ctx->ret_addr);

	hashed_bit = intlv_bit;
	hashed_bit ^= atl_get_bit((intlv_bit_pos + num_intlv_bits), ctx->ret_addr);
	hashed_bit ^= FIELD_GET(BIT_ULL(23), ctx->ret_addr) & hash_ctl_2M;
	hashed_bit ^= FIELD_GET(BIT_ULL(32), ctx->ret_addr) & hash_ctl_1G;

	if (hashed_bit != intlv_bit)
		ctx->ret_addr ^= BIT_ULL(intlv_bit_pos);

	intlv_bit_pos++;
	intlv_bit = atl_get_bit(intlv_bit_pos, ctx->ret_addr);

	hashed_bit = intlv_bit;
	hashed_bit ^= FIELD_GET(BIT_ULL(21), ctx->ret_addr) & hash_ctl_2M;
	hashed_bit ^= FIELD_GET(BIT_ULL(30), ctx->ret_addr) & hash_ctl_1G;

	if (hashed_bit != intlv_bit)
		ctx->ret_addr ^= BIT_ULL(intlv_bit_pos);

	intlv_bit_pos++;
	intlv_bit = atl_get_bit(intlv_bit_pos, ctx->ret_addr);

	hashed_bit = intlv_bit;
	hashed_bit ^= FIELD_GET(BIT_ULL(22), ctx->ret_addr) & hash_ctl_2M;
	hashed_bit ^= FIELD_GET(BIT_ULL(31), ctx->ret_addr) & hash_ctl_1G;

	if (hashed_bit != intlv_bit)
		ctx->ret_addr ^= BIT_ULL(intlv_bit_pos);

	return 0;
}

static int df4_dehash_addr(struct addr_ctx *ctx)
{
	bool hash_ctl_64k, hash_ctl_2M, hash_ctl_1G;
	u8 hashed_bit, intlv_bit;

	/* Assert that interleave bit is 8. */
	if (ctx->map.intlv_bit_pos != 8) {
		pr_warn("%s: Invalid interleave bit: %u",
			__func__, ctx->map.intlv_bit_pos);
		return -EINVAL;
	}

	/* Assert that die interleaving is disabled. */
	if (ctx->map.num_intlv_dies > 1) {
		pr_warn("%s: Invalid number of interleave dies: %u",
			__func__, ctx->map.num_intlv_dies);
		return -EINVAL;
	}

	/* Assert that no more than 2 sockets are interleaved. */
	if (ctx->map.num_intlv_sockets > 2) {
		pr_warn("%s: Invalid number of interleave sockets: %u",
			__func__, ctx->map.num_intlv_sockets);
		return -EINVAL;
	}

	hash_ctl_64k	= FIELD_GET(DF4_HASH_CTL_64K, ctx->map.ctl);
	hash_ctl_2M	= FIELD_GET(DF4_HASH_CTL_2M, ctx->map.ctl);
	hash_ctl_1G	= FIELD_GET(DF4_HASH_CTL_1G, ctx->map.ctl);

	intlv_bit = FIELD_GET(BIT_ULL(8), ctx->ret_addr);

	hashed_bit = intlv_bit;
	hashed_bit ^= FIELD_GET(BIT_ULL(16), ctx->ret_addr) & hash_ctl_64k;
	hashed_bit ^= FIELD_GET(BIT_ULL(21), ctx->ret_addr) & hash_ctl_2M;
	hashed_bit ^= FIELD_GET(BIT_ULL(30), ctx->ret_addr) & hash_ctl_1G;

	if (ctx->map.num_intlv_sockets == 1)
		hashed_bit ^= FIELD_GET(BIT_ULL(14), ctx->ret_addr);

	if (hashed_bit != intlv_bit)
		ctx->ret_addr ^= BIT_ULL(8);

	/*
	 * Hashing is possible with socket interleaving, so check the total number
	 * of channels in the system rather than DRAM map interleaving mode.
	 *
	 * Calculation complete for 2 channels. Continue for 4, 8, and 16 channels.
	 */
	if (ctx->map.total_intlv_chan <= 2)
		return 0;

	intlv_bit = FIELD_GET(BIT_ULL(12), ctx->ret_addr);

	hashed_bit = intlv_bit;
	hashed_bit ^= FIELD_GET(BIT_ULL(17), ctx->ret_addr) & hash_ctl_64k;
	hashed_bit ^= FIELD_GET(BIT_ULL(22), ctx->ret_addr) & hash_ctl_2M;
	hashed_bit ^= FIELD_GET(BIT_ULL(31), ctx->ret_addr) & hash_ctl_1G;

	if (hashed_bit != intlv_bit)
		ctx->ret_addr ^= BIT_ULL(12);

	/* Calculation complete for 4 channels. Continue for 8 and 16 channels. */
	if (ctx->map.total_intlv_chan <= 4)
		return 0;

	intlv_bit = FIELD_GET(BIT_ULL(13), ctx->ret_addr);

	hashed_bit = intlv_bit;
	hashed_bit ^= FIELD_GET(BIT_ULL(18), ctx->ret_addr) & hash_ctl_64k;
	hashed_bit ^= FIELD_GET(BIT_ULL(23), ctx->ret_addr) & hash_ctl_2M;
	hashed_bit ^= FIELD_GET(BIT_ULL(32), ctx->ret_addr) & hash_ctl_1G;

	if (hashed_bit != intlv_bit)
		ctx->ret_addr ^= BIT_ULL(13);

	/* Calculation complete for 8 channels. Continue for 16 channels. */
	if (ctx->map.total_intlv_chan <= 8)
		return 0;

	intlv_bit = FIELD_GET(BIT_ULL(14), ctx->ret_addr);

	hashed_bit = intlv_bit;
	hashed_bit ^= FIELD_GET(BIT_ULL(19), ctx->ret_addr) & hash_ctl_64k;
	hashed_bit ^= FIELD_GET(BIT_ULL(24), ctx->ret_addr) & hash_ctl_2M;
	hashed_bit ^= FIELD_GET(BIT_ULL(33), ctx->ret_addr) & hash_ctl_1G;

	if (hashed_bit != intlv_bit)
		ctx->ret_addr ^= BIT_ULL(14);

	return 0;
}

static int df4p5_dehash_addr(struct addr_ctx *ctx)
{
	bool hash_ctl_64k, hash_ctl_2M, hash_ctl_1G, hash_ctl_1T;
	u8 hashed_bit, intlv_bit;
	u64 rehash_vector;

	/* Assert that interleave bit is 8. */
	if (ctx->map.intlv_bit_pos != 8) {
		pr_warn("%s: Invalid interleave bit: %u",
			__func__, ctx->map.intlv_bit_pos);
		return -EINVAL;
	}

	/* Assert that die interleaving is disabled. */
	if (ctx->map.num_intlv_dies > 1) {
		pr_warn("%s: Invalid number of interleave dies: %u",
			__func__, ctx->map.num_intlv_dies);
		return -EINVAL;
	}

	/* Assert that no more than 2 sockets are interleaved. */
	if (ctx->map.num_intlv_sockets > 2) {
		pr_warn("%s: Invalid number of interleave sockets: %u",
			__func__, ctx->map.num_intlv_sockets);
		return -EINVAL;
	}

	hash_ctl_64k	= FIELD_GET(DF4_HASH_CTL_64K, ctx->map.ctl);
	hash_ctl_2M	= FIELD_GET(DF4_HASH_CTL_2M, ctx->map.ctl);
	hash_ctl_1G	= FIELD_GET(DF4_HASH_CTL_1G, ctx->map.ctl);
	hash_ctl_1T	= FIELD_GET(DF4_HASH_CTL_1T, ctx->map.ctl);

	/*
	 * Generate a unique address to determine which bits
	 * need to be dehashed.
	 *
	 * Start with a contiguous bitmask for the total
	 * number of channels starting at bit 8.
	 *
	 * Then make a gap in the proper place based on
	 * interleave mode.
	 */
	rehash_vector = ctx->map.total_intlv_chan - 1;
	rehash_vector <<= 8;

	if (ctx->map.intlv_mode == DF4p5_NPS2_4CHAN_1K_HASH ||
	    ctx->map.intlv_mode == DF4p5_NPS1_8CHAN_1K_HASH ||
	    ctx->map.intlv_mode == DF4p5_NPS1_16CHAN_1K_HASH)
		rehash_vector = expand_bits(10, 2, rehash_vector);
	else
		rehash_vector = expand_bits(9, 3, rehash_vector);

	if (rehash_vector & BIT_ULL(8)) {
		intlv_bit = FIELD_GET(BIT_ULL(8), ctx->ret_addr);

		hashed_bit = intlv_bit;
		hashed_bit ^= FIELD_GET(BIT_ULL(16), ctx->ret_addr) & hash_ctl_64k;
		hashed_bit ^= FIELD_GET(BIT_ULL(21), ctx->ret_addr) & hash_ctl_2M;
		hashed_bit ^= FIELD_GET(BIT_ULL(30), ctx->ret_addr) & hash_ctl_1G;
		hashed_bit ^= FIELD_GET(BIT_ULL(40), ctx->ret_addr) & hash_ctl_1T;

		if (hashed_bit != intlv_bit)
			ctx->ret_addr ^= BIT_ULL(8);
	}

	if (rehash_vector & BIT_ULL(9)) {
		intlv_bit = FIELD_GET(BIT_ULL(9), ctx->ret_addr);

		hashed_bit = intlv_bit;
		hashed_bit ^= FIELD_GET(BIT_ULL(17), ctx->ret_addr) & hash_ctl_64k;
		hashed_bit ^= FIELD_GET(BIT_ULL(22), ctx->ret_addr) & hash_ctl_2M;
		hashed_bit ^= FIELD_GET(BIT_ULL(31), ctx->ret_addr) & hash_ctl_1G;
		hashed_bit ^= FIELD_GET(BIT_ULL(41), ctx->ret_addr) & hash_ctl_1T;

		if (hashed_bit != intlv_bit)
			ctx->ret_addr ^= BIT_ULL(9);
	}

	if (rehash_vector & BIT_ULL(12)) {
		intlv_bit = FIELD_GET(BIT_ULL(12), ctx->ret_addr);

		hashed_bit = intlv_bit;
		hashed_bit ^= FIELD_GET(BIT_ULL(18), ctx->ret_addr) & hash_ctl_64k;
		hashed_bit ^= FIELD_GET(BIT_ULL(23), ctx->ret_addr) & hash_ctl_2M;
		hashed_bit ^= FIELD_GET(BIT_ULL(32), ctx->ret_addr) & hash_ctl_1G;
		hashed_bit ^= FIELD_GET(BIT_ULL(42), ctx->ret_addr) & hash_ctl_1T;

		if (hashed_bit != intlv_bit)
			ctx->ret_addr ^= BIT_ULL(12);
	}

	if (rehash_vector & BIT_ULL(13)) {
		intlv_bit = FIELD_GET(BIT_ULL(13), ctx->ret_addr);

		hashed_bit = intlv_bit;
		hashed_bit ^= FIELD_GET(BIT_ULL(19), ctx->ret_addr) & hash_ctl_64k;
		hashed_bit ^= FIELD_GET(BIT_ULL(24), ctx->ret_addr) & hash_ctl_2M;
		hashed_bit ^= FIELD_GET(BIT_ULL(33), ctx->ret_addr) & hash_ctl_1G;
		hashed_bit ^= FIELD_GET(BIT_ULL(43), ctx->ret_addr) & hash_ctl_1T;

		if (hashed_bit != intlv_bit)
			ctx->ret_addr ^= BIT_ULL(13);
	}

	if (rehash_vector & BIT_ULL(14)) {
		intlv_bit = FIELD_GET(BIT_ULL(14), ctx->ret_addr);

		hashed_bit = intlv_bit;
		hashed_bit ^= FIELD_GET(BIT_ULL(20), ctx->ret_addr) & hash_ctl_64k;
		hashed_bit ^= FIELD_GET(BIT_ULL(25), ctx->ret_addr) & hash_ctl_2M;
		hashed_bit ^= FIELD_GET(BIT_ULL(34), ctx->ret_addr) & hash_ctl_1G;
		hashed_bit ^= FIELD_GET(BIT_ULL(44), ctx->ret_addr) & hash_ctl_1T;

		if (hashed_bit != intlv_bit)
			ctx->ret_addr ^= BIT_ULL(14);
	}

	return 0;
}

int dehash_address(struct addr_ctx *ctx)
{
	switch (ctx->map.intlv_mode) {
	/* No hashing cases. */
	case NONE:
	case NOHASH_2CHAN:
	case NOHASH_4CHAN:
	case NOHASH_8CHAN:
	case NOHASH_16CHAN:
	case NOHASH_32CHAN:
	/* Hashing bits handled earlier during CS ID calculation. */
	case DF4_NPS4_3CHAN_HASH:
	case DF4_NPS2_5CHAN_HASH:
	case DF4_NPS2_6CHAN_HASH:
	case DF4_NPS1_10CHAN_HASH:
	case DF4_NPS1_12CHAN_HASH:
	case DF4p5_NPS2_6CHAN_1K_HASH:
	case DF4p5_NPS2_6CHAN_2K_HASH:
	case DF4p5_NPS1_10CHAN_1K_HASH:
	case DF4p5_NPS1_10CHAN_2K_HASH:
	case DF4p5_NPS1_12CHAN_1K_HASH:
	case DF4p5_NPS1_12CHAN_2K_HASH:
	case DF4p5_NPS0_24CHAN_1K_HASH:
	case DF4p5_NPS0_24CHAN_2K_HASH:
	/* No hash physical address bits, so nothing to do. */
	case DF4p5_NPS4_3CHAN_1K_HASH:
	case DF4p5_NPS4_3CHAN_2K_HASH:
	case DF4p5_NPS2_5CHAN_1K_HASH:
	case DF4p5_NPS2_5CHAN_2K_HASH:
		return 0;

	case DF2_2CHAN_HASH:
		return df2_dehash_addr(ctx);

	case DF3_COD4_2CHAN_HASH:
	case DF3_COD2_4CHAN_HASH:
	case DF3_COD1_8CHAN_HASH:
		return df3_dehash_addr(ctx);

	case DF3_6CHAN:
		return df3_6chan_dehash_addr(ctx);

	case DF4_NPS4_2CHAN_HASH:
	case DF4_NPS2_4CHAN_HASH:
	case DF4_NPS1_8CHAN_HASH:
		return df4_dehash_addr(ctx);

	case DF4p5_NPS4_2CHAN_1K_HASH:
	case DF4p5_NPS4_2CHAN_2K_HASH:
	case DF4p5_NPS2_4CHAN_2K_HASH:
	case DF4p5_NPS2_4CHAN_1K_HASH:
	case DF4p5_NPS1_8CHAN_1K_HASH:
	case DF4p5_NPS1_8CHAN_2K_HASH:
	case DF4p5_NPS1_16CHAN_1K_HASH:
	case DF4p5_NPS1_16CHAN_2K_HASH:
		return df4p5_dehash_addr(ctx);

	default:
		ATL_BAD_INTLV_MODE(ctx->map.intlv_mode);
		return -EINVAL;
	}
}
