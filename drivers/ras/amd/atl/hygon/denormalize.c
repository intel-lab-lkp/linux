// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * AMD Address Translation Library
 *
 * denormalize.c : Functions to account for interleaving bits for Hygon
 *
 * Author: AichunShi <shiaichun@open-hieco.net>
 */

#include "../internal.h"

static u16 hygon_get_dst_fabric_id(struct addr_ctx *ctx)
{
	switch (df_cfg.rev) {
	case HYGON_DF1:
		return FIELD_GET(HYGON_DF1_DST_FABRIC_ID, ctx->map.limit);
	default:
		atl_debug_on_bad_df_rev();
		return 0;
	}
}

static u64 hygon_make_space_for_coh_st_id_df1_3chan(struct addr_ctx *ctx)
{
	return ctx->ret_addr & GENMASK_ULL(ctx->map.intlv_bit_pos - 1, 0);
}

static u64 hygon_make_space_for_coh_st_id(struct addr_ctx *ctx)
{
	switch (ctx->map.intlv_mode) {
	case NOHASH_2CHAN:
	case NOHASH_4CHAN:
	case NOHASH_8CHAN:
	case NOHASH_16CHAN:
	case NOHASH_32CHAN:
	case DF2_2CHAN_HASH:
		return make_space_for_coh_st_id_at_intlv_bit(ctx);

	case HYGON_DF1_3CHAN:
		return hygon_make_space_for_coh_st_id_df1_3chan(ctx);

	default:
		atl_debug_on_bad_intlv_mode(ctx);
		return ~0ULL;
	}
}

static u16 hygon_get_coh_st_id_df(struct addr_ctx *ctx)
{
	u8 num_socket_intlv_bits = ilog2(ctx->map.num_intlv_sockets);
	u8 num_die_intlv_bits = ilog2(ctx->map.num_intlv_dies);
	u16 coh_st_id = ctx->coh_st_fabric_id;
	u16 mask;
	u8 intlv_bit_pos = ctx->map.intlv_bit_pos;
	u8 num_intlv_bits = order_base_2(ctx->map.num_intlv_chan);

	if (ctx->map.intlv_mode == HYGON_DF1_3CHAN) {
		u8 cs_offset;
		u16 dst_fabric_id = hygon_get_dst_fabric_id(ctx);
		u64 temp_addr_x;

		cs_offset = (ctx->coh_st_fabric_id & 0x3) - (dst_fabric_id & 0x3);
		if (cs_offset > 3) {
			atl_debug(ctx,
				  "Invalid cs_offset: 0x%x coh_st_fabric_id: 0x%x dst_fabric_id: 0x%x.\n",
				  cs_offset, ctx->coh_st_fabric_id, dst_fabric_id);
			return ~0;
		}

		temp_addr_x = (ctx->ret_addr & GENMASK_ULL(63, intlv_bit_pos)) >>
				intlv_bit_pos;
		temp_addr_x = temp_addr_x * 3 + cs_offset;
		ctx->ret_addr = temp_addr_x;

		coh_st_id = temp_addr_x & GENMASK_ULL(num_intlv_bits - 1, 0);
	} else {
		mask = GENMASK(num_intlv_bits - 1, 0);
		coh_st_id &= mask;
	}

	/* Die interleave bits */
	if (num_die_intlv_bits) {
		u16 die_bits;

		mask = GENMASK(num_die_intlv_bits - 1, 0);
		die_bits = ctx->coh_st_fabric_id & df_cfg.die_id_mask;
		die_bits >>= df_cfg.die_id_shift;
		die_bits -= 4;

		coh_st_id |= (die_bits & mask) << num_intlv_bits;
		num_intlv_bits += num_die_intlv_bits;
	}

	/* Socket interleave bits */
	if (num_socket_intlv_bits) {
		u16 socket_bits;

		mask = GENMASK(num_socket_intlv_bits - 1, 0);
		socket_bits = ctx->coh_st_fabric_id & df_cfg.socket_id_mask;
		socket_bits >>= df_cfg.socket_id_shift;

		coh_st_id |= (socket_bits & mask) << num_intlv_bits;
	}

	ctx->coh_st_fabric_id = coh_st_id;

	return coh_st_id;
}

static u16 hygon_calculate_coh_st_id(struct addr_ctx *ctx)
{
	switch (ctx->map.intlv_mode) {
	case NOHASH_2CHAN:
	case HYGON_DF1_3CHAN:
	case NOHASH_4CHAN:
	case NOHASH_8CHAN:
	case NOHASH_16CHAN:
	case NOHASH_32CHAN:
	case DF2_2CHAN_HASH:
		return hygon_get_coh_st_id_df(ctx);

	default:
		atl_debug_on_bad_intlv_mode(ctx);
		return ~0;
	}
}

static u64 hygon_insert_coh_st_id_df1_3chan(struct addr_ctx *ctx, u64 denorm_addr, u16 coh_st_id)
{
	u8 num_intlv_bits_chan = order_base_2(ctx->map.num_intlv_chan);
	u64 temp_addr = ((ctx->ret_addr >> num_intlv_bits_chan) << num_intlv_bits_chan) | coh_st_id;

	return denorm_addr | (temp_addr << ctx->map.intlv_bit_pos);
}

static u64 hygon_insert_coh_st_id(struct addr_ctx *ctx, u64 denorm_addr, u16 coh_st_id)
{
	switch (ctx->map.intlv_mode) {
	case NOHASH_2CHAN:
	case NOHASH_4CHAN:
	case NOHASH_8CHAN:
	case NOHASH_16CHAN:
	case NOHASH_32CHAN:
	case DF2_2CHAN_HASH:
		return insert_coh_st_id_at_intlv_bit(ctx, denorm_addr, coh_st_id);

	case HYGON_DF1_3CHAN:
		return hygon_insert_coh_st_id_df1_3chan(ctx, denorm_addr, coh_st_id);

	default:
		atl_debug_on_bad_intlv_mode(ctx);
		return ~0ULL;
	}
}

static int hygon_denorm_addr_common(struct addr_ctx *ctx)
{
	u64 denorm_addr;
	u16 coh_st_id;

	denorm_addr = hygon_make_space_for_coh_st_id(ctx);
	coh_st_id = hygon_calculate_coh_st_id(ctx);
	ctx->ret_addr = hygon_insert_coh_st_id(ctx, denorm_addr, coh_st_id);
	return 0;
}

int hygon_denormalize_address(struct addr_ctx *ctx)
{
	switch (ctx->map.intlv_mode) {
	case NONE:
		return 0;

	default:
		return hygon_denorm_addr_common(ctx);
	}
}
