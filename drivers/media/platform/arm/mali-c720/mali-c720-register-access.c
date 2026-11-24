// SPDX-License-Identifier: GPL-2.0-only
/*
 * ARM Mali-C720 ISP Driver - Image signal processor
 *
 * Copyright (C) 2026 Arm Ltd
 */

#include <linux/align.h>

#include "mali-c720-common.h"

static __le32 *mali_c720_ctx_reg_addr(struct mali_c720_context *ctx,
				      unsigned int addr)
{
	return (__le32 *)(ctx->regs.base + addr);
}

/*
 * System IO
 *
 * The Mali-C720 has a shared global register space for settings such as
 * context manager, faults, interrupts, etc. Each context then has a DMA
 * allocated configuration space for pipeline registers. The ISP has a
 * configuration DMA (CDMA) engine that will automatically load the context
 * configuration space upon frame start. In both cases we can write to these
 * normally, though care should be taken with the context space to ensure any
 * access does not coincide with a frame start, causing CDMA to read a
 * partially-applied configuration.
 */

static int mali_c720_mmio_check_addr(struct mali_c720 *mali_c720, u32 addr)
{
	int ret;

	ret = WARN_ON_ONCE(!IS_ALIGNED(addr, sizeof(u32)));
	if (ret)
		return ret;

	ret = WARN_ON_ONCE((u64)addr + sizeof(u32) > mali_c720->mmio_size);
	if (ret)
		return ret;

	ret = WARN_ON_ONCE(mali_c720->registers.context_offset &&
			   (u64)addr + sizeof(u32) > mali_c720->registers.context_offset);
	if (ret)
		return ret;

	return 0;
}

u32 mali_c720_read_raw(struct mali_c720 *mali_c720, u32 addr)
{
	if (mali_c720_mmio_check_addr(mali_c720, addr))
		return 0;

	return readl(mali_c720->base + addr);
}

static void mali_c720_write_reg_raw(struct mali_c720 *mali_c720, u32 addr, u32 val)
{
	if (mali_c720_mmio_check_addr(mali_c720, addr))
		return;

	writel(val, mali_c720->base + addr);
}

u32 mali_c720_group_read(struct mali_c720 *mali_c720, u32 base,
			 const struct mali_c720_reg_field *field)
{
	if (WARN_ON_ONCE(field->shift > MALI_C720_REG_MAX_SHIFT))
		return 0;

	return (mali_c720_read_raw(mali_c720, base + field->offset) & field->mask) >>
	       field->shift;
}

void mali_c720_group_write(struct mali_c720 *mali_c720, u32 base,
			   const struct mali_c720_reg_field *field, u32 val)
{
	u32 addr = base + field->offset;
	u32 reg;

	if (WARN_ON_ONCE(field->shift > MALI_C720_REG_MAX_SHIFT))
		return;
	if (mali_c720_mmio_check_addr(mali_c720, addr))
		return;

	reg = readl(mali_c720->base + addr) & ~field->mask;
	reg |= (val << field->shift) & field->mask;
	writel(reg, mali_c720->base + addr);
}

u32 mali_c720_read(struct mali_c720 *mali_c720, const struct mali_c720_reg_field *field)
{
	return mali_c720_group_read(mali_c720, 0, field);
}

void mali_c720_write(struct mali_c720 *mali_c720, const struct mali_c720_reg_field *field, u32 val)
{
	mali_c720_group_write(mali_c720, 0, field, val);
}

void mali_c720_write_raw(struct mali_c720 *mali_c720, const struct mali_c720_reg_field *field,
			 u32 val)
{
	mali_c720_write_reg_raw(mali_c720, field->offset, val);
}

u32 mali_c720_irq_read(struct mali_c720 *mali_c720, const struct mali_c720_reg_field *field,
		       enum mali_c720_irq_type type)
{
	return mali_c720_group_read(mali_c720, type, field);
}

void mali_c720_irq_write(struct mali_c720 *mali_c720, const struct mali_c720_reg_field *field,
			 enum mali_c720_irq_type type, u32 val)
{
	if (WARN_ON_ONCE(field->shift > MALI_C720_REG_MAX_SHIFT))
		return;

	/* RAWSTAT is write-one-to-clear: acknowledge only the selected bits. */
	if (type == MALI_C720_IRQ_RAWSTAT)
		mali_c720_write_reg_raw(mali_c720, field->offset + type,
					(val << field->shift) & field->mask);
	else
		mali_c720_group_write(mali_c720, type, field, val);
}

static int mali_c720_ctx_check_addr(struct mali_c720_context *ctx, u32 addr)
{
	int ret;

	ret = WARN_ON_ONCE(!IS_ALIGNED(addr, sizeof(__le32)));
	if (ret)
		return ret;

	ret = WARN_ON_ONCE(addr < ctx->mali_c720->registers.context_offset ||
			   (u64)addr + sizeof(__le32) > ctx->regs.size);
	if (ret)
		return ret;

	return 0;
}

u32 mali_c720_ctx_read_raw(struct mali_c720_context *ctx, u32 addr)
{
	__le32 *ptr;

	if (mali_c720_ctx_check_addr(ctx, addr))
		return 0;

	ptr = mali_c720_ctx_reg_addr(ctx, addr);
	return le32_to_cpu(*ptr);
}

void mali_c720_ctx_write_raw(struct mali_c720_context *ctx, u32 addr, u32 val)
{
	__le32 *ptr;

	if (mali_c720_ctx_check_addr(ctx, addr))
		return;

	ptr = mali_c720_ctx_reg_addr(ctx, addr);

	*ptr = cpu_to_le32(val);
}

u32 mali_c720_ctx_group_read(struct mali_c720_context *ctx, u32 base,
			     const struct mali_c720_reg_field *field)
{
	if (WARN_ON_ONCE(field->shift > MALI_C720_REG_MAX_SHIFT))
		return 0;

	return (mali_c720_ctx_read_raw(ctx, base + field->offset) & field->mask) >> field->shift;
}

void mali_c720_ctx_group_write(struct mali_c720_context *ctx, u32 base,
			       const struct mali_c720_reg_field *field, u32 val)
{
	u32 addr = base + field->offset;
	__le32 *ptr;
	u32 reg;

	if (WARN_ON_ONCE(field->shift > MALI_C720_REG_MAX_SHIFT))
		return;
	if (mali_c720_ctx_check_addr(ctx, addr))
		return;

	ptr = mali_c720_ctx_reg_addr(ctx, addr);

	reg = le32_to_cpu(*ptr) & ~field->mask;
	reg |= (val << field->shift) & field->mask;
	*ptr = cpu_to_le32(reg);
}

u32 mali_c720_ctx_read(struct mali_c720_context *ctx, const struct mali_c720_reg_field *field)
{
	return mali_c720_ctx_group_read(ctx, 0, field);
}

void mali_c720_ctx_write(struct mali_c720_context *ctx, const struct mali_c720_reg_field *field,
			 u32 val)
{
	mali_c720_ctx_group_write(ctx, 0, field, val);
}
