// SPDX-License-Identifier: GPL-2.0-only
/*
 * ARM Mali-C720 ISP Driver - Image signal processor
 *
 * Copyright (C) 2026 Arm Ltd
 */

#include <linux/bits.h>
#include <linux/errno.h>

#include "mali-c720-common.h"

static const struct mali_c720_id_regs id_regs = {
	.pid4 = { .offset = 0xfd0, .mask = GENMASK(31, 0), .shift = 0 },
	.pid4_des_2 = { .offset = 0xfd0, .mask = GENMASK(3, 0), .shift = 0 },
	.pid0 = { .offset = 0xfe0, .mask = GENMASK(31, 0), .shift = 0 },
	.pid0_part_0 = { .offset = 0xfe0, .mask = GENMASK(7, 0), .shift = 0 },
	.pid1 = { .offset = 0xfe4, .mask = GENMASK(31, 0), .shift = 0 },
	.pid1_part_1 = { .offset = 0xfe4, .mask = GENMASK(3, 0), .shift = 0 },
	.pid1_des_0 = { .offset = 0xfe4, .mask = GENMASK(7, 4), .shift = 4 },
	.pid2 = { .offset = 0xfe8, .mask = GENMASK(31, 0), .shift = 0 },
	.pid2_des_1 = { .offset = 0xfe8, .mask = GENMASK(2, 0), .shift = 0 },
	.pid2_revision = { .offset = 0xfe8, .mask = GENMASK(7, 4), .shift = 4 },
	.pid3 = { .offset = 0xfec, .mask = GENMASK(31, 0), .shift = 0 },
	.pid3_cmod = { .offset = 0xfec, .mask = GENMASK(3, 0), .shift = 0 },
	.pid3_revand = { .offset = 0xfec, .mask = GENMASK(7, 4), .shift = 4 },
	.cid0_prmbl_0 = { .offset = 0xff0, .mask = GENMASK(7, 0), .shift = 0 },
	.cid1_prmbl_1 = { .offset = 0xff4, .mask = GENMASK(3, 0), .shift = 0 },
	.cid1_class = { .offset = 0xff4, .mask = GENMASK(7, 4), .shift = 4 },
	.cid2_prmbl_2 = { .offset = 0xff8, .mask = GENMASK(7, 0), .shift = 0 },
	.cid3_prmbl_3 = { .offset = 0xffc, .mask = GENMASK(7, 0), .shift = 0 },
};

const struct mali_c720_id_regs *mali_c720_get_id_regs(void)
{
	return &id_regs;
}

int mali_c720_get_registers(struct mali_c720_registers *regs)
{
	*regs = (struct mali_c720_registers) {
		.id = &id_regs,
		.top = mali_c720_get_top_regs(),
		.cm = mali_c720_get_ctx_mgr_regs(),
		.modules = mali_c720_get_modules_regs(),
		.context_offset = 0x80000,
		.context_size = 0x100000,
	};
	return 0;
}
