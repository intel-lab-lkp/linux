// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * AMD Address Translation Library
 *
 * core.c : Base translation functions for Hygon
 *
 * Author: AichunShi <shiaichun@open-hieco.net>
 */

#include "../internal.h"

unsigned long hygon_norm_to_sys_addr(u8 socket_id, u8 die_id, u8 coh_st_inst_id,
				     u8 sub_channel, unsigned long addr)
{
	struct addr_ctx ctx;

	if (df_cfg.rev == UNKNOWN)
		return -EINVAL;

	memset(&ctx, 0, sizeof(ctx));

	/* Start from the normalized address */
	ctx.ret_addr = addr;
	ctx.node_id = die_id;
	ctx.inst_id = coh_st_inst_id;

	ctx.inputs.norm_addr = addr;
	ctx.inputs.socket_id = socket_id;
	ctx.inputs.die_id = die_id;
	ctx.inputs.coh_st_inst_id = coh_st_inst_id;

	ctx.chan_intlv_hygon.sub_channel = sub_channel;

	if (legacy_hole_en(&ctx) && !df_cfg.dram_hole_base)
		return -EINVAL;

	if (hygon_get_address_map(&ctx))
		return -EINVAL;

	if (hygon_denormalize_address(&ctx))
		return -EINVAL;

	ctx.ret_addr = add_base_and_hole(&ctx, ctx.ret_addr);

	if (hygon_dehash_address(&ctx))
		return -EINVAL;

	if (addr_over_limit(&ctx))
		return -EINVAL;

	return ctx.ret_addr;
}
