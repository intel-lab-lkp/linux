/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * ARM Mali-C720 ISP Driver - Image signal processor
 *
 * Copyright (C) 2026 Arm Ltd
 */

#ifndef MALI_C720_REGISTERS_H
#define MALI_C720_REGISTERS_H

#include <linux/types.h>

enum mali_c720_version_id;

struct mali_c720_top_regs;
struct mali_c720_ctx_mgr_regs;
struct mali_c720_modules_regs;

struct mali_c720_reg_field {
	u32 offset;
	u32 mask;
	u8 shift;
};

struct mali_c720_id_regs {
	struct mali_c720_reg_field pid4;
	struct mali_c720_reg_field pid4_des_2;
	struct mali_c720_reg_field pid0;
	struct mali_c720_reg_field pid0_part_0;
	struct mali_c720_reg_field pid1;
	struct mali_c720_reg_field pid1_part_1;
	struct mali_c720_reg_field pid1_des_0;
	struct mali_c720_reg_field pid2;
	struct mali_c720_reg_field pid2_des_1;
	struct mali_c720_reg_field pid2_revision;
	struct mali_c720_reg_field pid3;
	struct mali_c720_reg_field pid3_cmod;
	struct mali_c720_reg_field pid3_revand;
	struct mali_c720_reg_field cid0_prmbl_0;
	struct mali_c720_reg_field cid1_prmbl_1;
	struct mali_c720_reg_field cid1_class;
	struct mali_c720_reg_field cid2_prmbl_2;
	struct mali_c720_reg_field cid3_prmbl_3;
};

struct mali_c720_group_regs {
	u32 offset;
	u32 size;
};

struct mali_c720_registers {
	const struct mali_c720_id_regs *id;
	const struct mali_c720_top_regs *top;
	const struct mali_c720_ctx_mgr_regs *cm;
	const struct mali_c720_modules_regs *modules;
	u32 context_offset;
	u32 context_size;
};

const struct mali_c720_id_regs *mali_c720_get_id_regs(void);
int mali_c720_get_registers(enum mali_c720_version_id version,
			    struct mali_c720_registers *regs);

static inline u32 mali_c720_reg_addr(u32 base, const struct mali_c720_reg_field *field)
{
	return base + field->offset;
}

static inline u32 mali_c720_reg_mask(const struct mali_c720_reg_field *field)
{
	return field->mask;
}

static inline u8 mali_c720_reg_shift(const struct mali_c720_reg_field *field)
{
	return field->shift;
}

static inline u32 mali_c720_group_base(const struct mali_c720_group_regs *group, unsigned int index)
{
	return group->offset + index * group->size;
}

#endif /* MALI_C720_REGISTERS_H */
