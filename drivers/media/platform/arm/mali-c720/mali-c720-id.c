// SPDX-License-Identifier: GPL-2.0-only
/*
 * ARM Mali-C720 ISP Driver - Image signal processor
 *
 * Copyright (C) 2026 Arm Ltd
 */

#include <linux/array_size.h>
#include <linux/bitfield.h>
#include <linux/device.h>

#include "mali-c720-common.h"

#define MALI_C720_ID_PART 0x560
#define MALI_C720_ID_DESIGNER_ARM 0x43b
#define MALI_C720_ID_PREAMBLE 0xb105000d
#define MALI_C720_ID_CLASS 0xe

struct mali_c720_id_registers {
	u32 pid0;
	u32 pid1;
	u32 pid2;
	u32 pid3;
	u32 pid4;
	u32 preamble;
	u32 class;
};

static const struct mali_c720_hw_version
mali_c720_supported_hw_versions[MALI_C720_VERSION_ID_MAX] = {
	[MALI_C720_VERSION_ID_R0_P0] = {
		.name = "Mali-C720 r0p0",
		.id = MALI_C720_VERSION_ID_R0_P0,
		.part = MALI_C720_ID_PART,
		.revision = 0x0,
		.revand = 0x0,
		.cmod = 0x0,
	},
	[MALI_C720_VERSION_ID_R0_P1] = {
		.name = "Mali-C720 r0p1",
		.id = MALI_C720_VERSION_ID_R0_P1,
		.part = MALI_C720_ID_PART,
		.revision = 0x1,
		.revand = 0x0,
		.cmod = 0x0,
	},
};

static void mali_c720_read_id_registers(struct mali_c720 *mali_c720,
					struct mali_c720_id_registers *id)
{
	const struct mali_c720_id_regs *id_regs = mali_c720->registers.id;

	id->pid0 = mali_c720_read(mali_c720, &id_regs->pid0);
	id->pid1 = mali_c720_read(mali_c720, &id_regs->pid1);
	id->pid2 = mali_c720_read(mali_c720, &id_regs->pid2);
	id->pid3 = mali_c720_read(mali_c720, &id_regs->pid3);
	id->pid4 = mali_c720_read(mali_c720, &id_regs->pid4);

	/* Assemble the component preamble, keeping the class separate. */
	id->preamble = mali_c720_read(mali_c720, &id_regs->cid0_prmbl_0);
	id->preamble |= mali_c720_read(mali_c720, &id_regs->cid1_prmbl_1) << 8;
	id->preamble |= mali_c720_read(mali_c720, &id_regs->cid2_prmbl_2) << 16;
	id->preamble |= mali_c720_read(mali_c720, &id_regs->cid3_prmbl_3) << 24;
	id->class = mali_c720_read(mali_c720, &id_regs->cid1_class);
}

static u16 mali_c720_id_designer(const struct mali_c720_id_regs *id_regs,
				 const struct mali_c720_id_registers *id)
{
	return (u16)(field_get(mali_c720_reg_mask(&id_regs->pid1_des_0), id->pid1) |
		     (field_get(mali_c720_reg_mask(&id_regs->pid2_des_1), id->pid2) << 4) |
		     (field_get(mali_c720_reg_mask(&id_regs->pid4_des_2), id->pid4) << 8));
}

static u16 mali_c720_id_part(const struct mali_c720_id_regs *id_regs,
			     const struct mali_c720_id_registers *id)
{
	return (u16)(field_get(mali_c720_reg_mask(&id_regs->pid0_part_0), id->pid0) |
		     (field_get(mali_c720_reg_mask(&id_regs->pid1_part_1), id->pid1) << 8));
}

static u8 mali_c720_id_revision(const struct mali_c720_id_regs *id_regs,
				const struct mali_c720_id_registers *id)
{
	return (u8)field_get(mali_c720_reg_mask(&id_regs->pid2_revision), id->pid2);
}

static u8 mali_c720_id_revand(const struct mali_c720_id_regs *id_regs,
			      const struct mali_c720_id_registers *id)
{
	return (u8)field_get(mali_c720_reg_mask(&id_regs->pid3_revand), id->pid3);
}

static u8 mali_c720_id_cmod(const struct mali_c720_id_regs *id_regs,
			    const struct mali_c720_id_registers *id)
{
	return (u8)field_get(mali_c720_reg_mask(&id_regs->pid3_cmod), id->pid3);
}

static const struct mali_c720_hw_version *
mali_c720_find_hw_version(const struct mali_c720_id_regs *id_regs,
			  const struct mali_c720_id_registers *id)
{
	u16 part = mali_c720_id_part(id_regs, id);
	u8 revision = mali_c720_id_revision(id_regs, id);
	u8 revand = mali_c720_id_revand(id_regs, id);
	u8 cmod = mali_c720_id_cmod(id_regs, id);
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(mali_c720_supported_hw_versions); i++) {
		const struct mali_c720_hw_version *version =
			&mali_c720_supported_hw_versions[i];

		if (version->part == part &&
		    version->revision == revision &&
		    version->revand == revand &&
		    version->cmod == cmod)
			return version;
	}

	return NULL;
}

int mali_c720_identify_hw(struct mali_c720 *mali_c720)
{
	const struct mali_c720_id_regs *id_regs = mali_c720->registers.id;
	struct mali_c720_id_registers id;
	const struct mali_c720_hw_version *version;
	u16 part, designer;
	u8 revision, revand, cmod;

	mali_c720_read_id_registers(mali_c720, &id);

	designer = mali_c720_id_designer(id_regs, &id);
	if (designer != MALI_C720_ID_DESIGNER_ARM)
		return dev_err_probe(mali_c720->dev, -ENODEV,
				     "unsupported ISP designer ID: %#x\n", designer);

	if (id.preamble != MALI_C720_ID_PREAMBLE)
		return dev_err_probe(mali_c720->dev, -ENODEV,
				     "unsupported ISP component preamble: %#x\n", id.preamble);

	if (id.class != MALI_C720_ID_CLASS)
		return dev_err_probe(mali_c720->dev, -ENODEV,
				     "unsupported ISP component class: %#x\n", id.class);

	version = mali_c720_find_hw_version(id_regs, &id);
	if (!version) {
		part = mali_c720_id_part(id_regs, &id);
		revision = mali_c720_id_revision(id_regs, &id);
		revand = mali_c720_id_revand(id_regs, &id);
		cmod = mali_c720_id_cmod(id_regs, &id);

		return dev_err_probe(mali_c720->dev, -ENODEV,
				     "unsupported ISP version: part=%#x revision=%u revand=%u cmod=%u\n",
				     part, revision, revand, cmod);
	}

	mali_c720->hw_version = version;

	dev_info(mali_c720->dev, "detected %s\n", version->name);

	return 0;
}
