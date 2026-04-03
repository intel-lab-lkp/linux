// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * AMD Address Translation Library
 *
 * map.c : Functions to read and decode DRAM address maps for Hygon
 *
 * Author: AichunShi <shiaichun@open-hieco.net>
 */

#include "../internal.h"

static int hygon_df_get_intlv_mode(struct addr_ctx *ctx)
{
	ctx->map.intlv_mode = FIELD_GET(HYGON_DF1_INTLV_NUM_CHAN, ctx->map.base);

	if (df_cfg.rev == HYGON_DF1 && ctx->map.intlv_mode == 2)
		ctx->map.intlv_mode = HYGON_DF1_3CHAN;

	if (ctx->map.intlv_mode == 8)
		ctx->map.intlv_mode = DF2_2CHAN_HASH;

	if (ctx->map.intlv_mode != NONE &&
	    ctx->map.intlv_mode != NOHASH_2CHAN &&
		ctx->map.intlv_mode != HYGON_DF1_3CHAN &&
	    ctx->map.intlv_mode != NOHASH_4CHAN &&
	    ctx->map.intlv_mode != NOHASH_8CHAN &&
	    ctx->map.intlv_mode != NOHASH_16CHAN &&
	    ctx->map.intlv_mode != DF2_2CHAN_HASH)
		return -EINVAL;

	return 0;
}

static u64 hygon_get_hi_addr_offset(u32 reg_dram_offset)
{
	u8 shift = DF_DRAM_BASE_LIMIT_LSB;
	u64 hi_addr_offset;

	switch (df_cfg.rev) {
	case HYGON_DF1:
		hi_addr_offset = FIELD_GET(HYGON_DF1_HI_ADDR_OFFSET, reg_dram_offset);
		break;
	default:
		hi_addr_offset = 0;
		atl_debug_on_bad_df_rev();
	}

	return hi_addr_offset << shift;
}

/*
 * Returns:	0 if offset is disabled.
 *		1 if offset is enabled.
 *		-EINVAL on error.
 */
static int hygon_get_dram_offset(struct addr_ctx *ctx, u64 *norm_offset)
{
	u32 reg_dram_offset;
	u8 map_num;

	/* Should not be called for map 0. */
	if (!ctx->map.num) {
		atl_debug(ctx, "Trying to find DRAM offset for map 0");
		return -EINVAL;
	}

	/*
	 * DramOffset registers don't exist for map 0, so the base register
	 * actually refers to map 1.
	 * Adjust the map_num for the register offsets.
	 */
	map_num = ctx->map.num - 1;

	if (df_cfg.rev >= HYGON_DF1) {
		/* Read D18F0x214 (DramOffset) */
		if (df_indirect_read_instance(ctx->node_id, 0, 0x214 + (4 * map_num),
					      ctx->inst_id, &reg_dram_offset))
			return -EINVAL;
	} else {
		return -EINVAL;
	}

	if (!FIELD_GET(DF_HI_ADDR_OFFSET_EN, reg_dram_offset))
		return 0;

	*norm_offset = hygon_get_hi_addr_offset(reg_dram_offset);

	return 1;
}

#define HYGON_DF1_CHAN_ADDR_SEL	BIT(24)
#define HYGON_DF1_CHAN_HASH_ENABLE	BIT(23)
#define HYGON_DF1_DDR5_ENABLE	BIT(19)
static int hygon_df1_get_dram_addr_map(struct addr_ctx *ctx)
{
	if (df2_get_dram_addr_map(ctx))
		return -EINVAL;

	/* Read D18F2x48 */
	if (df_indirect_read_instance(ctx->node_id, 2, 0x48,
				      ctx->inst_id, &ctx->map.intlv))
		return -EINVAL;

	ctx->chan_intlv_hygon.chan_addr_sel =
		FIELD_GET(HYGON_DF1_CHAN_ADDR_SEL, ctx->map.intlv);
	ctx->chan_intlv_hygon.chan_hash_enable =
		FIELD_GET(HYGON_DF1_CHAN_HASH_ENABLE, ctx->map.intlv);
	ctx->chan_intlv_hygon.ddr5_enable =
		FIELD_GET(HYGON_DF1_DDR5_ENABLE, ctx->map.intlv);

	if (ctx->chan_intlv_hygon.ddr5_enable) {
		u64 low_addr, high_addr;

		ctx->chan_intlv_hygon.start_bit = ctx->chan_intlv_hygon.chan_addr_sel ? 8 : 7;
		low_addr = ctx->ret_addr & GENMASK_ULL(ctx->chan_intlv_hygon.start_bit - 1, 0);
		high_addr = (ctx->ret_addr & GENMASK_ULL(63, ctx->chan_intlv_hygon.start_bit)) << 1;
		ctx->ret_addr = high_addr | low_addr;
	}

	return 0;
}

static int hygon_get_dram_addr_map(struct addr_ctx *ctx)
{
	switch (df_cfg.rev) {
	case HYGON_DF1:	return hygon_df1_get_dram_addr_map(ctx);
	default:
			atl_debug_on_bad_df_rev();
			return -EINVAL;
	}
}

static int hygon_get_coh_st_fabric_id(struct addr_ctx *ctx)
{
	u32 reg;

	/* Read D18F0x50 (FabricBlockInstanceInformation3). */
	if (df_indirect_read_instance(ctx->node_id, 0, 0x50, ctx->inst_id, &reg))
		return -EINVAL;

	switch (df_cfg.rev) {
	case HYGON_DF1:
		ctx->coh_st_fabric_id = FIELD_GET(HYGON_DF1_COH_ST_FABRIC_ID, reg);
		break;
	default:
		atl_debug_on_bad_df_rev();
		return -EINVAL;
	}

	return 0;
}

static int hygon_find_normalized_offset(struct addr_ctx *ctx, u64 *norm_offset)
{
	u64 last_offset = 0;
	int ret;

	for (ctx->map.num = 1; ctx->map.num < df_cfg.num_coh_st_maps; ctx->map.num++) {
		ret = hygon_get_dram_offset(ctx, norm_offset);
		if (ret < 0)
			return ret;

		/* Continue search if this map's offset is not enabled. */
		if (!ret)
			continue;

		/* Enabled offsets should never be 0. */
		if (*norm_offset == 0) {
			atl_debug(ctx, "Enabled map %u offset is 0", ctx->map.num);
			return -EINVAL;
		}

		/* Offsets should always increase from one map to the next. */
		if (*norm_offset <= last_offset) {
			atl_debug(ctx, "Map %u offset (0x%016llx) <= previous (0x%016llx)",
				  ctx->map.num, *norm_offset, last_offset);
			return -EINVAL;
		}

		/* Match if this map's offset is less than the current calculated address. */
		if (ctx->ret_addr >= *norm_offset)
			break;

		last_offset = *norm_offset;
	}

	/*
	 * Finished search without finding a match.
	 * Reset to map 0 and no offset.
	 */
	if (ctx->map.num >= df_cfg.num_coh_st_maps) {
		ctx->map.num = 0;
		*norm_offset = 0;
	}

	return 0;
}

static int hygon_get_address_map_common(struct addr_ctx *ctx)
{
	u64 norm_offset = 0;

	if (hygon_get_coh_st_fabric_id(ctx))
		return -EINVAL;

	if (hygon_find_normalized_offset(ctx, &norm_offset))
		return -EINVAL;

	ctx->ret_addr -= norm_offset;

	if (hygon_get_dram_addr_map(ctx))
		return -EINVAL;

	if (!valid_map(ctx))
		return -EINVAL;

	return 0;
}

static u8 hygon_get_num_intlv_chan(struct addr_ctx *ctx)
{
	switch (ctx->map.intlv_mode) {
	case NONE:
		return 1;
	case NOHASH_2CHAN:
		return 2;
	case HYGON_DF1_3CHAN:
		return 3;
	case NOHASH_4CHAN:
		return 4;
	case NOHASH_8CHAN:
		return 8;
	case NOHASH_16CHAN:
		return 16;
	case NOHASH_32CHAN:
		return 32;
	default:
		atl_debug_on_bad_intlv_mode(ctx);
		return 0;
	}
}

static void hygon_calculate_intlv_bits(struct addr_ctx *ctx)
{
	ctx->map.num_intlv_chan = hygon_get_num_intlv_chan(ctx);

	ctx->map.total_intlv_chan = ctx->map.num_intlv_chan;
	ctx->map.total_intlv_chan *= ctx->map.num_intlv_dies;
	ctx->map.total_intlv_chan *= ctx->map.num_intlv_sockets;

	/*
	 * Get the number of bits needed to cover this many channels.
	 * order_base_2() rounds up automatically.
	 */
	ctx->map.total_intlv_bits = order_base_2(ctx->map.total_intlv_chan);
}

static u8 hygon_get_intlv_bit_pos(struct addr_ctx *ctx)
{
	u8 addr_sel = 0;

	switch (df_cfg.rev) {
	case HYGON_DF1:
		addr_sel = FIELD_GET(HYGON_DF1_INTLV_ADDR_SEL, ctx->map.base);
		break;
	default:
		atl_debug_on_bad_df_rev();
		break;
	}

	/* Add '8' to get the 'interleave bit position'. */
	return addr_sel + 8;
}

static u8 hygon_get_num_intlv_dies(struct addr_ctx *ctx)
{
	u8 dies = 0;

	switch (df_cfg.rev) {
	case HYGON_DF1:
		dies = FIELD_GET(HYGON_DF1_INTLV_NUM_DIES, ctx->map.limit);
		break;
	default:
		atl_debug_on_bad_df_rev();
		break;
	}

	/* Register value is log2, e.g. 0 -> 1 die, 1 -> 2 dies, etc. */
	return 1 << dies;
}

static u8 hygon_get_num_intlv_sockets(struct addr_ctx *ctx)
{
	u8 sockets = 0;

	switch (df_cfg.rev) {
	case HYGON_DF1:
		sockets = FIELD_GET(HYGON_DF1_INTLV_NUM_SOCKETS, ctx->map.base);
		break;
	default:
		atl_debug_on_bad_df_rev();
		break;
	}

	/* Register value is log2, e.g. 0 -> 1 sockets, 1 -> 2 sockets, etc. */
	return 1 << sockets;
}

static int hygon_get_global_map_data(struct addr_ctx *ctx)
{
	if (hygon_df_get_intlv_mode(ctx))
		return -EINVAL;

	ctx->map.intlv_bit_pos		= hygon_get_intlv_bit_pos(ctx);
	ctx->map.num_intlv_dies		= hygon_get_num_intlv_dies(ctx);
	ctx->map.num_intlv_sockets	= hygon_get_num_intlv_sockets(ctx);
	hygon_calculate_intlv_bits(ctx);

	return 0;
}

int hygon_get_address_map(struct addr_ctx *ctx)
{
	int ret;

	ret = hygon_get_address_map_common(ctx);
	if (ret)
		return ret;

	ret = hygon_get_global_map_data(ctx);
	if (ret)
		return ret;

	dump_address_map(&ctx->map);

	return ret;
}
