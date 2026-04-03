// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * AMD Address Translation Library
 *
 * dehash.c : Functions to account for hashing bits for Hygon
 *
 * Author: AichunShi <shiaichun@open-hieco.net>
 */

#include "../internal.h"

static int hygon_df1_ddr5_dehash_addr(struct addr_ctx *ctx)
{
	u8 chan_hash_enable = ctx->chan_intlv_hygon.chan_hash_enable;
	u8 sub_channel = ctx->chan_intlv_hygon.sub_channel;
	u8 start_bit = ctx->chan_intlv_hygon.start_bit;
	u8 hashed_bit;

	if (chan_hash_enable) {
		hashed_bit =	(ctx->ret_addr >> 12) ^
				(ctx->ret_addr >> 21) ^
				(ctx->ret_addr >> 30) ^
				sub_channel;
		hashed_bit &= BIT(0);
		ctx->ret_addr |= hashed_bit << start_bit;
	} else {
		ctx->ret_addr |= sub_channel << start_bit;
	}

	return 0;
}

static int hygon_df_2chan_dehash_addr(struct addr_ctx *ctx)
{
	u8 hashed_bit;
	u8 intlv_bit_pos = ctx->map.intlv_bit_pos;

	hashed_bit =	(ctx->ret_addr >> 12) ^
		(ctx->ret_addr >> 18) ^
		(ctx->ret_addr >> 21) ^
		(ctx->ret_addr >> 30) ^
		ctx->coh_st_fabric_id;

	hashed_bit &= BIT(0);
	if (hashed_bit != ((ctx->ret_addr >> intlv_bit_pos) & BIT(0)))
		ctx->ret_addr ^= BIT(intlv_bit_pos);

	return 0;
}

static int hygon_df2_4chan_dehash_addr(struct addr_ctx *ctx)
{
	u8 hashed_bit;
	u8 intlv_bit_pos = ctx->map.intlv_bit_pos;

	hashed_bit =	(ctx->ret_addr >> 12) ^
			(ctx->ret_addr >> 18) ^
			(ctx->ret_addr >> 21) ^
			(ctx->ret_addr >> 30) ^
			ctx->coh_st_fabric_id;

	hashed_bit &= 0x3;
	if (hashed_bit != ((ctx->ret_addr >> intlv_bit_pos) & 0x3))
		ctx->ret_addr = (ctx->ret_addr & ~((u64)3 << intlv_bit_pos)) |
				(hashed_bit << intlv_bit_pos);

	return 0;
}

int hygon_dehash_address(struct addr_ctx *ctx)
{
	switch (ctx->map.intlv_mode) {
	/* No hashing cases. */
	case NONE:
	case NOHASH_2CHAN:
	case HYGON_DF1_3CHAN:
	case NOHASH_4CHAN:
	case NOHASH_8CHAN:
	case NOHASH_16CHAN:
	case NOHASH_32CHAN:
		break;

	case DF2_2CHAN_HASH:
		hygon_df_2chan_dehash_addr(ctx);
		break;

	case HYGON_DF2_4CHAN_HASH:
		hygon_df2_4chan_dehash_addr(ctx);
		break;

	default:
		atl_debug_on_bad_intlv_mode(ctx);
		return -EINVAL;
	}

	if (df_cfg.rev == HYGON_DF1 && ctx->chan_intlv_hygon.ddr5_enable)
		hygon_df1_ddr5_dehash_addr(ctx);

	return 0;
}
