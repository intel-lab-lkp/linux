/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * ARM Mali-C720 ISP Driver - Image signal processor
 *
 * Copyright (C) 2026 Arm Ltd
 */

#ifndef MALI_C720_REGISTER_ACCESS_H
#define MALI_C720_REGISTER_ACCESS_H

#include <linux/types.h>

struct mali_c720;
struct mali_c720_context;
struct mali_c720_reg_field;

#define MALI_C720_REG_MAX_SHIFT		31

enum mali_c720_irq_type {
	MALI_C720_IRQ_RAWSTAT = 0x4,
	MALI_C720_IRQ_MASK = 0x8,
	MALI_C720_IRQ_STATUS = 0xc,
};

/* HW register access */
u32 mali_c720_read_raw(struct mali_c720 *mali_c720, u32 addr);
u32 mali_c720_read(struct mali_c720 *mali_c720, const struct mali_c720_reg_field *field);
void mali_c720_write(struct mali_c720 *mali_c720, const struct mali_c720_reg_field *field, u32 val);
void mali_c720_write_raw(struct mali_c720 *mali_c720, const struct mali_c720_reg_field *field,
			 u32 val);

/* Group register access */
u32 mali_c720_group_read(struct mali_c720 *mali_c720, u32 base,
			 const struct mali_c720_reg_field *field);
void mali_c720_group_write(struct mali_c720 *mali_c720, u32 base,
			   const struct mali_c720_reg_field *field, u32 val);

/* IRQ register access */
u32 mali_c720_irq_read(struct mali_c720 *mali_c720, const struct mali_c720_reg_field *field,
		       enum mali_c720_irq_type type);
void mali_c720_irq_write(struct mali_c720 *mali_c720, const struct mali_c720_reg_field *field,
			 enum mali_c720_irq_type type, u32 val);

/* SW register access */
u32 mali_c720_ctx_read(struct mali_c720_context *ctx, const struct mali_c720_reg_field *field);
void mali_c720_ctx_write(struct mali_c720_context *ctx, const struct mali_c720_reg_field *field,
			 u32 val);
u32 mali_c720_ctx_group_read(struct mali_c720_context *ctx, u32 base,
			     const struct mali_c720_reg_field *field);
void mali_c720_ctx_group_write(struct mali_c720_context *ctx, u32 base,
			       const struct mali_c720_reg_field *field, u32 val);
u32 mali_c720_ctx_read_raw(struct mali_c720_context *ctx, u32 addr);
void mali_c720_ctx_write_raw(struct mali_c720_context *ctx, u32 addr, u32 val);

#endif /* MALI_C720_REGISTER_ACCESS_H */
