// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * AMD Address Translation Library
 *
 * core.c : Module init and base translation functions
 *
 * Copyright (c) 2023, Advanced Micro Devices, Inc.
 * All Rights Reserved.
 *
 * Author: Yazen Ghannam <Yazen.Ghannam@amd.com>
 */

#include <linux/module.h>

#include "internal.h"

struct df_config df_cfg __read_mostly;

static int addr_over_limit(struct addr_ctx *ctx)
{
	u64 dram_limit_addr;

	if (df_cfg.rev >= DF4)
		dram_limit_addr  = FIELD_GET(DF4_DRAM_LIMIT_ADDR, ctx->map.limit);
	else
		dram_limit_addr  = FIELD_GET(DF2_DRAM_LIMIT_ADDR, ctx->map.limit);

	dram_limit_addr <<= DF_DRAM_BASE_LIMIT_LSB;
	dram_limit_addr |= GENMASK(DF_DRAM_BASE_LIMIT_LSB - 1, 0);

	/* Is calculated system address above DRAM limit address? */
	if (ctx->ret_addr > dram_limit_addr)
		return -EINVAL;

	return 0;
}

static bool legacy_hole_en(struct addr_ctx *ctx)
{
	u32 reg = ctx->map.base;

	if (df_cfg.rev >= DF4)
		reg = ctx->map.ctl;

	return FIELD_GET(DF_LEGACY_MMIO_HOLE_EN, reg);
}

static int add_legacy_hole(struct addr_ctx *ctx)
{
	u32 dram_hole_base;
	u8 func = 0;

	if (!legacy_hole_en(ctx))
		return 0;

	if (df_cfg.rev >= DF4)
		func = 7;

	if (df_indirect_read_broadcast(ctx->node_id, func, 0x104, &dram_hole_base))
		return -EINVAL;

	dram_hole_base &= DF_DRAM_HOLE_BASE_MASK;

	if (ctx->ret_addr >= dram_hole_base)
		ctx->ret_addr += (BIT_ULL(32) - dram_hole_base);

	return 0;
}

static u64 get_base_addr(struct addr_ctx *ctx)
{
	u64 base_addr;

	if (df_cfg.rev >= DF4)
		base_addr = FIELD_GET(DF4_BASE_ADDR, ctx->map.base);
	else
		base_addr = FIELD_GET(DF2_BASE_ADDR, ctx->map.base);

	return base_addr << DF_DRAM_BASE_LIMIT_LSB;
}

static int add_base_and_hole(struct addr_ctx *ctx)
{
	ctx->ret_addr += get_base_addr(ctx);

	if (add_legacy_hole(ctx))
		return -EINVAL;

	return 0;
}

static bool late_hole_remove(struct addr_ctx *ctx)
{
	if (df_cfg.rev == DF3p5)
		return true;

	if (df_cfg.rev == DF4)
		return true;

	if (ctx->map.intlv_mode == DF3_6CHAN)
		return true;

	return false;
}

int norm_to_sys_addr(u8 socket_id, u8 die_id, u8 cs_inst_id, u64 *addr)
{
	struct addr_ctx ctx;

	if (df_cfg.rev == UNKNOWN)
		return -EINVAL;

	memset(&ctx, 0, sizeof(ctx));

	/* We start from the normalized address */
	ctx.ret_addr = *addr;
	ctx.inst_id = cs_inst_id;

	if (determine_node_id(&ctx, socket_id, die_id)) {
		pr_warn("Failed to determine Node ID");
		return -EINVAL;
	}

	if (get_address_map(&ctx)) {
		pr_warn("Failed to get address maps");
		return -EINVAL;
	}

	if (denormalize_address(&ctx)) {
		pr_warn("Failed to denormalize address");
		return -EINVAL;
	}

	if (!late_hole_remove(&ctx) && add_base_and_hole(&ctx)) {
		pr_warn("Failed to add DRAM base address and hole");
		return -EINVAL;
	}

	if (dehash_address(&ctx)) {
		pr_warn("Failed to dehash address");
		return -EINVAL;
	}

	if (late_hole_remove(&ctx) && add_base_and_hole(&ctx)) {
		pr_warn("Failed to add DRAM base address and hole");
		return -EINVAL;
	}

	if (addr_over_limit(&ctx)) {
		pr_warn("Calculated address is over limit");
		return -EINVAL;
	}

	*addr = ctx.ret_addr;
	return 0;
}

static void check_for_legacy_df_access(void)
{
	/*
	 * All Zen-based systems before Family 19h use the legacy
	 * DF Indirect Access (FICAA/FICAD) offsets.
	 */
	if (boot_cpu_data.x86 < 0x19) {
		df_cfg.flags.legacy_ficaa = true;
		return;
	}

	/* All systems after Family 19h use the current offsets. */
	if (boot_cpu_data.x86 > 0x19)
		return;

	/* Some Family 19h systems use the legacy offsets. */
	switch (boot_cpu_data.x86_model) {
	case 0x00 ... 0x0f:
	case 0x20 ... 0x5f:
	       df_cfg.flags.legacy_ficaa = true;
	}
}

static int __init amd_atl_init(void)
{
	if (boot_cpu_data.x86_vendor != X86_VENDOR_AMD)
		return -ENODEV;

	check_for_legacy_df_access();

	/*
	 * Not sure if this should return an error code.
	 * That may prevent other modules from loading.
	 *
	 * For now, don't fail out. The translation function
	 * will do a check and return early if the DF revision
	 * is not set.
	 */
	if (get_df_system_info()) {
		pr_warn("Failed to get DF information");
		df_cfg.rev = UNKNOWN;
	}

	pr_info("AMD Address Translation Library initialized");
	return 0;
}

static void __exit amd_atl_exit(void)
{
}

module_init(amd_atl_init);
module_exit(amd_atl_exit);

MODULE_LICENSE("GPL");
