// SPDX-License-Identifier: GPL-2.0-only
/*
 * ARM Mali-C720 ISP Driver - Image signal processor
 *
 * Copyright (C) 2026 Arm Ltd
 */

#include <linux/bitmap.h>
#include <linux/bitops.h>
#include <linux/cleanup.h>
#include <linux/crc-ccitt.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/iopoll.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/timekeeping.h>
#include <linux/wordpart.h>

#include "mali-c720-common.h"

#define MALI_C720_CRC16_MCRF4XX_INIT 0xffff

/* Allow for a worst case of 1FPS for input enable/disable timeouts */
#define INPUT_POLL_TIMEOUT 1000000
#define INPUT_POLL_INTERVAL 1000

struct mali_c720_context *mali_c720_get_active_context(struct mali_c720 *mali_c720)
{
	return &mali_c720->context;
}

static u32 mali_c720_ctx_mgr_buffer_base(const struct mali_c720_registers *regs,
					 const struct mali_c720_buffer_pool *pool,
					 unsigned int index)
{
	const struct mali_c720_cm_buffer_regs *buffer = regs->cm->buffer;
	unsigned int descriptor_id = pool->pool_id + index;

	if (pool->type == MALI_C720_BUF_TYPE_RAW)
		return mali_c720_group_base(&buffer->raw_group, descriptor_id);

	return mali_c720_group_base(&buffer->out_group, descriptor_id);
}

static void mali_c720_ctx_mgr_input_command(struct mali_c720 *mali_c720, u32 base,
					    u32 command)
{
	const struct mali_c720_cm_input_regs *input = mali_c720->registers.cm->input;
	u32 val;

	mali_c720_group_write(mali_c720, base, &input->command, command);
	mali_c720_group_read(mali_c720, base, &input->status_arch);
	udelay(1);

	if (command != MALI_C720_INPUT_COMMAND_CANCEL)
		return;

	/* Wait for the input FIFO and writer to drain during cancellation. */
	(void)poll_timeout_us(val = mali_c720_group_read(mali_c720, base, &input->status_arch),
			      val == MALI_C720_INPUT_DISABLED, 10, 1000, false);
}

static void mali_c720_ctx_mgr_buffer_request(struct mali_c720 *mali_c720, u32 base, u32 state)
{
	const struct mali_c720_cm_buffer_regs *buffer = mali_c720->registers.cm->buffer;
	u32 val;

	/* Readback and a small delay to ensure state transition completes */
	mali_c720_group_write(mali_c720, base, &buffer->state_request, state);
	mali_c720_group_read(mali_c720, base, &buffer->state);
	udelay(1);

	if (state != MALI_C720_BUFFER_STATE_CANCELLED)
		return;

	/* Wait for outstanding DMA transactions during cancellation */
	(void)poll_timeout_us_atomic(val = mali_c720_group_read(mali_c720, base, &buffer->state),
				     val == MALI_C720_BUFFER_STATE_CANCELLED, 10, 1000, false);
}

static void mali_c720_ctx_mgr_set_slot_enabled(struct mali_c720_context *ctx, bool enable)
{
	struct mali_c720 *mali_c720 = ctx->mali_c720;
	const struct mali_c720_cm_job_control_regs *job_control =
		mali_c720->registers.cm->job_control;
	const struct mali_c720_cm_slot_regs *slot = mali_c720->registers.cm->slot;
	u32 val = enable ? MALI_C720_SCH_COMMAND_ENABLE_SLOT :
		      MALI_C720_SCH_COMMAND_DISABLE_SLOT;
	val |= ctx->id << mali_c720_reg_shift(&job_control->sch_command_slot);

	mali_c720_write_raw(mali_c720, &job_control->sch_command, val);
	mali_c720_group_read(mali_c720, mali_c720_group_base(&slot->group, ctx->id), &slot->status);
	udelay(1);
}

static void mali_c720_ctx_mgr_cancel_slot(struct mali_c720_context *ctx)
{
	struct mali_c720 *mali_c720 = ctx->mali_c720;
	const struct mali_c720_cm_job_control_regs *job_control =
		mali_c720->registers.cm->job_control;
	const struct mali_c720_cm_slot_regs *slot = mali_c720->registers.cm->slot;
	u32 val = MALI_C720_SCH_COMMAND_CANCEL_SLOT;

	val |= ctx->id << mali_c720_reg_shift(&job_control->sch_command_slot);

	mali_c720_write_raw(mali_c720, &job_control->sch_command, val);
	mali_c720_group_read(mali_c720, mali_c720_group_base(&slot->group, ctx->id), &slot->status);
	udelay(1);
}

static void mali_c720_ctx_mgr_stop_slot(struct mali_c720_context *ctx)
{
	struct mali_c720 *mali_c720 = ctx->mali_c720;
	const struct mali_c720_cm_slot_regs *slot = mali_c720->registers.cm->slot;
	u32 slot_addr = mali_c720_group_base(&slot->group, ctx->id);
	u32 val;
	int ret;

	mali_c720_ctx_mgr_set_slot_enabled(ctx, false);

	/* Stop arbitration and allow an already selected frame to finish. */
	ret = poll_timeout_us(val = mali_c720_group_read(mali_c720, slot_addr, &slot->status),
			      val == MALI_C720_SLOT_DISABLED || val == MALI_C720_SLOT_ERROR,
			      5000, 1000000, false);
	if (!ret && val == MALI_C720_SLOT_DISABLED)
		return;

	dev_warn(mali_c720->dev, "Slot %u failed to disable (state %u), cancelling\n",
		 ctx->id, val);
	mali_c720_ctx_mgr_cancel_slot(ctx);

	/* Cancellation must drain the buffer agents and CDMA before cleanup. */
	(void)poll_timeout_us(val = mali_c720_group_read(mali_c720, slot_addr, &slot->status),
			      val == MALI_C720_SLOT_DISABLED, 100, 1000, false);
}

static void mali_c720_ctx_mgr_cdma_request(struct mali_c720 *mali_c720, u32 base, u32 state)
{
	const struct mali_c720_cm_cdma_slot_regs *cdma_slot = mali_c720->registers.cm->cdma_slot;

	mali_c720_group_write(mali_c720, base, &cdma_slot->cdma_state_request, state);
	mali_c720_group_read(mali_c720, base, &cdma_slot->cdma_state);
	udelay(1);
}

static void mali_c720_ctx_mgr_set_scheduler_enabled(struct mali_c720 *mali_c720, bool enable)
{
	const struct mali_c720_cm_job_control_regs *job_control =
		mali_c720->registers.cm->job_control;
	u32 command = enable ? MALI_C720_SCH_COMMAND_ENABLE : MALI_C720_SCH_COMMAND_DISABLE;

	mali_c720_write(mali_c720, &job_control->sch_command, command);
	mali_c720_read(mali_c720, &job_control->sch_status_arch);
	udelay(1);
}

static inline unsigned int mali_c720_reg_width(u32 mask)
{
	if (!mask)
		return 0;

	return __fls(mask) - __ffs(mask) + 1;
}

static u16 mali_c720_crc16_mcrf4xx_bits(u16 crc, u32 data, unsigned int bits)
{
	unsigned int bit;

	for (bit = 0; bit + 8 <= bits; bit += 8)
		crc = crc_ccitt_byte(crc, (data >> bit) & 0xff);

	for (; bit < bits; bit++) {
		if ((crc ^ (data >> bit)) & 0x1)
			crc = (crc >> 1) ^ 0x8408;
		else
			crc >>= 1;
	}

	return crc;
}

static u16 mali_c720_crc16_mcrf4xx_reg(u16 crc, u32 data, u32 mask)
{
	return mali_c720_crc16_mcrf4xx_bits(crc, data,
					    mali_c720_reg_width(mask));
}

static u16 mali_c720_ctx_mgr_calc_cdma_desc_crc(struct mali_c720_context *ctx)
{
	const struct mali_c720_cm_cdma_slot_regs *cdma_slot =
		ctx->mali_c720->registers.cm->cdma_slot;
	struct mali_c720_cdma_desc *desc = &ctx->cdma_desc;
	u16 crc = MALI_C720_CRC16_MCRF4XX_INIT;

	crc = mali_c720_crc16_mcrf4xx_bits(crc, 0, 4);
	crc = mali_c720_crc16_mcrf4xx_bits(crc, 0, 2);
	crc = mali_c720_crc16_mcrf4xx_bits(crc, 0, 2);
	crc = mali_c720_crc16_mcrf4xx_reg(crc, desc->infinite,
					  mali_c720_reg_mask(&cdma_slot->infinite));
	crc = mali_c720_crc16_mcrf4xx_bits(crc, 0, 1);
	crc = mali_c720_crc16_mcrf4xx_reg(crc, desc->nonsecure,
					  mali_c720_reg_mask(&cdma_slot->nonsecure));
	crc = mali_c720_crc16_mcrf4xx_reg(crc, desc->stream_id,
					  mali_c720_reg_mask(&cdma_slot->stream_id));
	crc = mali_c720_crc16_mcrf4xx_bits(crc, 0, 16);
	crc = mali_c720_crc16_mcrf4xx_reg(crc, desc->transfer_size,
					  mali_c720_reg_mask(&cdma_slot->transfer_size));
	crc = mali_c720_crc16_mcrf4xx_reg(crc, desc->units_low,
					  mali_c720_reg_mask(&cdma_slot->units_low));
	crc = mali_c720_crc16_mcrf4xx_reg(crc, desc->units_high,
					  mali_c720_reg_mask(&cdma_slot->units_high));
	crc = mali_c720_crc16_mcrf4xx_reg(crc, desc->base_address_low,
					  mali_c720_reg_mask(&cdma_slot->base_address_low));
	crc = mali_c720_crc16_mcrf4xx_reg(crc, desc->base_address_high,
					  mali_c720_reg_mask(&cdma_slot->base_address_high));
	crc = mali_c720_crc16_mcrf4xx_reg(crc, desc->ld_sg_count,
					  mali_c720_reg_mask(&cdma_slot->ld_sg_count));
	crc = mali_c720_crc16_mcrf4xx_bits(crc, 0, 2);
	crc = mali_c720_crc16_mcrf4xx_reg(crc, desc->st_sg_count,
					  mali_c720_reg_mask(&cdma_slot->st_sg_count));
	crc = mali_c720_crc16_mcrf4xx_bits(crc, 0, 18);
	crc = mali_c720_crc16_mcrf4xx_reg(crc, desc->alt_transfer_size,
					  mali_c720_reg_mask(&cdma_slot->alt_transfer_size));
	crc = mali_c720_crc16_mcrf4xx_reg(crc, desc->alt_base_address_low,
					  mali_c720_reg_mask(&cdma_slot->alt_base_address_low));
	crc = mali_c720_crc16_mcrf4xx_reg(crc, desc->alt_base_address_high,
					  mali_c720_reg_mask(&cdma_slot->alt_base_address_high));

	return crc;
}

static u16 mali_c720_ctx_mgr_calc_slot_desc_crc(struct mali_c720_context *ctx)
{
	const struct mali_c720_cm_slot_regs *slot = ctx->mali_c720->registers.cm->slot;
	struct mali_c720_slot_desc *desc = &ctx->slot_desc;
	u16 crc = MALI_C720_CRC16_MCRF4XX_INIT;

	crc = mali_c720_crc16_mcrf4xx_reg(crc, desc->mode,
					  mali_c720_reg_mask(&slot->mode));
	crc = mali_c720_crc16_mcrf4xx_reg(crc, desc->timeout_enable,
					  mali_c720_reg_mask(&slot->timeout_enable));
	crc = mali_c720_crc16_mcrf4xx_bits(crc, 0, 4);
	crc = mali_c720_crc16_mcrf4xx_reg(crc, desc->schedule_priority,
					  mali_c720_reg_mask(&slot->schedule_priority));
	crc = mali_c720_crc16_mcrf4xx_bits(crc, 0, 1);
	crc = mali_c720_crc16_mcrf4xx_reg(crc, desc->nonsecure,
					  mali_c720_reg_mask(&slot->nonsecure));
	crc = mali_c720_crc16_mcrf4xx_reg(crc, desc->stream_id,
					  mali_c720_reg_mask(&slot->stream_id));
	crc = mali_c720_crc16_mcrf4xx_bits(crc, 0, 16);
	crc = mali_c720_crc16_mcrf4xx_reg(crc, desc->delay_mant,
					  mali_c720_reg_mask(&slot->delay_mant));
	crc = mali_c720_crc16_mcrf4xx_reg(crc, desc->delay_exp,
					  mali_c720_reg_mask(&slot->delay_exp));
	crc = mali_c720_crc16_mcrf4xx_reg(crc, desc->slot_event,
					  mali_c720_reg_mask(&slot->slot_event));
	crc = mali_c720_crc16_mcrf4xx_bits(crc, 0, 12);
	crc = mali_c720_crc16_mcrf4xx_reg(crc, desc->slot_input_0,
					  mali_c720_reg_mask(&slot->slot_input_0));
	crc = mali_c720_crc16_mcrf4xx_reg(crc, desc->slot_input_1,
					  mali_c720_reg_mask(&slot->slot_input_1));
	crc = mali_c720_crc16_mcrf4xx_bits(crc, 0, 16);
	crc = mali_c720_crc16_mcrf4xx_reg(crc, desc->timeout_mant,
					  mali_c720_reg_mask(&slot->timeout_mant));
	crc = mali_c720_crc16_mcrf4xx_reg(crc, desc->timeout_exp,
					  mali_c720_reg_mask(&slot->timeout_exp));
	crc = mali_c720_crc16_mcrf4xx_bits(crc, 0, 16);
	crc = mali_c720_crc16_mcrf4xx_reg(crc, desc->pool_roi0,
					  mali_c720_reg_mask(&slot->pool_roi0));
	crc = mali_c720_crc16_mcrf4xx_reg(crc, desc->pool_roi1,
					  mali_c720_reg_mask(&slot->pool_roi1));
	crc = mali_c720_crc16_mcrf4xx_reg(crc, desc->pool_roi2,
					  mali_c720_reg_mask(&slot->pool_roi2));
	crc = mali_c720_crc16_mcrf4xx_reg(crc, desc->pool_raw,
					  mali_c720_reg_mask(&slot->pool_raw));
	crc = mali_c720_crc16_mcrf4xx_reg(crc, desc->pool_scaler0_rgb,
					  mali_c720_reg_mask(&slot->pool_scaler0_rgb));
	crc = mali_c720_crc16_mcrf4xx_reg(crc, desc->pool_scaler0_yuv,
					  mali_c720_reg_mask(&slot->pool_scaler0_yuv));
	crc = mali_c720_crc16_mcrf4xx_reg(crc, desc->pool_scaler1_rgb,
					  mali_c720_reg_mask(&slot->pool_scaler1_rgb));
	crc = mali_c720_crc16_mcrf4xx_reg(crc, desc->pool_scaler1_yuv,
					  mali_c720_reg_mask(&slot->pool_scaler1_yuv));
	crc = mali_c720_crc16_mcrf4xx_reg(crc, desc->pool_scaler2_rgb,
					  mali_c720_reg_mask(&slot->pool_scaler2_rgb));
	crc = mali_c720_crc16_mcrf4xx_reg(crc, desc->pool_scaler2_yuv,
					  mali_c720_reg_mask(&slot->pool_scaler2_yuv));
	crc = mali_c720_crc16_mcrf4xx_reg(crc, desc->pool_scaler3_rgb,
					  mali_c720_reg_mask(&slot->pool_scaler3_rgb));
	crc = mali_c720_crc16_mcrf4xx_reg(crc, desc->pool_scaler3_yuv,
					  mali_c720_reg_mask(&slot->pool_scaler3_yuv));
	crc = mali_c720_crc16_mcrf4xx_reg(crc, desc->pool_gdc_main,
					  mali_c720_reg_mask(&slot->pool_gdc_main));
	crc = mali_c720_crc16_mcrf4xx_reg(crc, desc->pool_gdc_roi0,
					  mali_c720_reg_mask(&slot->pool_gdc_roi0));
	crc = mali_c720_crc16_mcrf4xx_reg(crc, desc->pool_gdc_roi1,
					  mali_c720_reg_mask(&slot->pool_gdc_roi1));
	crc = mali_c720_crc16_mcrf4xx_reg(crc, desc->pool_gdc_roi2,
					  mali_c720_reg_mask(&slot->pool_gdc_roi2));
	crc = mali_c720_crc16_mcrf4xx_reg(crc, desc->pool_pyramid,
					  mali_c720_reg_mask(&slot->pool_pyramid));
	crc = mali_c720_crc16_mcrf4xx_reg(crc, desc->pool_consistency,
					  mali_c720_reg_mask(&slot->pool_consistency));
	crc = mali_c720_crc16_mcrf4xx_bits(crc, 0, 16);

	return crc;
}

static u16 mali_c720_ctx_mgr_calc_buffer_desc_crc(const struct mali_c720_registers *regs,
						  struct mali_c720_buffer_pool *pool,
						  unsigned int index)
{
	const struct mali_c720_cm_buffer_regs *buffer = regs->cm->buffer;
	struct mali_c720_buffer_desc *desc = &pool->desc[index];
	u16 crc = MALI_C720_CRC16_MCRF4XX_INIT;

	crc = mali_c720_crc16_mcrf4xx_bits(crc, 0, 32);
	crc = mali_c720_crc16_mcrf4xx_reg(crc, desc->data_format,
					  mali_c720_reg_mask(&buffer->data_format));
	crc = mali_c720_crc16_mcrf4xx_reg(crc, desc->data_align,
					  mali_c720_reg_mask(&buffer->data_align));
	crc = mali_c720_crc16_mcrf4xx_reg(crc, desc->nonsecure,
					  mali_c720_reg_mask(&buffer->nonsecure));
	crc = mali_c720_crc16_mcrf4xx_reg(crc, desc->stream_id,
					  mali_c720_reg_mask(&buffer->stream_id));
	crc = mali_c720_crc16_mcrf4xx_reg(crc, desc->data_layout,
					  mali_c720_reg_mask(&buffer->data_layout));
	crc = mali_c720_crc16_mcrf4xx_reg(crc, desc->crc_check,
					  mali_c720_reg_mask(&buffer->crc_check));
	crc = mali_c720_crc16_mcrf4xx_reg(crc, desc->pool,
					  mali_c720_reg_mask(&buffer->pool));
	crc = mali_c720_crc16_mcrf4xx_reg(crc, desc->mode,
					  mali_c720_reg_mask(&buffer->mode));
	crc = mali_c720_crc16_mcrf4xx_reg(crc, desc->block_size,
					  mali_c720_reg_mask(&buffer->block_size));
	crc = mali_c720_crc16_mcrf4xx_bits(crc, 0, 8);
	crc = mali_c720_crc16_mcrf4xx_reg(crc, desc->base_address_low,
					  mali_c720_reg_mask(&buffer->base_address_low));
	crc = mali_c720_crc16_mcrf4xx_reg(crc, desc->base_address_high,
					  mali_c720_reg_mask(&buffer->base_address_high));
	crc = mali_c720_crc16_mcrf4xx_reg(crc, desc->buffer_size,
					  mali_c720_reg_mask(&buffer->size));
	crc = mali_c720_crc16_mcrf4xx_reg(crc, desc->plane2_offset,
					  mali_c720_reg_mask(&buffer->plane2_offset));
	crc = mali_c720_crc16_mcrf4xx_reg(crc, desc->plane3_offset,
					  mali_c720_reg_mask(&buffer->plane3_offset));
	crc = mali_c720_crc16_mcrf4xx_reg(crc, desc->plane4_offset,
					  mali_c720_reg_mask(&buffer->plane4_offset));
	crc = mali_c720_crc16_mcrf4xx_reg(crc, desc->line1_offset,
					  mali_c720_reg_mask(&buffer->line1_offset));
	crc = mali_c720_crc16_mcrf4xx_reg(crc, desc->line2_offset,
					  mali_c720_reg_mask(&buffer->line2_offset));
	crc = mali_c720_crc16_mcrf4xx_reg(crc, desc->line3_offset,
					  mali_c720_reg_mask(&buffer->line3_offset));
	crc = mali_c720_crc16_mcrf4xx_reg(crc, desc->line4_offset,
					  mali_c720_reg_mask(&buffer->line4_offset));
	crc = mali_c720_crc16_mcrf4xx_reg(crc, desc->width,
					  mali_c720_reg_mask(&buffer->width));
	crc = mali_c720_crc16_mcrf4xx_reg(crc, desc->height,
					  mali_c720_reg_mask(&buffer->height));
	crc = mali_c720_crc16_mcrf4xx_reg(crc, desc->colorspace,
					  mali_c720_reg_mask(&buffer->colorspace));
	crc = mali_c720_crc16_mcrf4xx_reg(crc, desc->subsampling,
					  mali_c720_reg_mask(&buffer->subsampling));
	crc = mali_c720_crc16_mcrf4xx_bits(crc, 0, 2);
	crc = mali_c720_crc16_mcrf4xx_reg(crc, desc->component_width,
					  mali_c720_reg_mask(&buffer->component_width));
	crc = mali_c720_crc16_mcrf4xx_reg(crc, desc->msb_align,
					  mali_c720_reg_mask(&buffer->msb_align));
	crc = mali_c720_crc16_mcrf4xx_reg(crc, desc->phase,
					  mali_c720_reg_mask(&buffer->phase));
	crc = mali_c720_crc16_mcrf4xx_bits(crc, 0, 1);
	crc = mali_c720_crc16_mcrf4xx_reg(crc, desc->height_modulo,
					  mali_c720_reg_mask(&buffer->height_modulo));
	crc = mali_c720_crc16_mcrf4xx_reg(crc, desc->compression,
					  mali_c720_reg_mask(&buffer->compression));
	crc = mali_c720_crc16_mcrf4xx_reg(crc, desc->compression23,
					  mali_c720_reg_mask(&buffer->compression23));

	return crc;
}

static void mali_c720_prepare_input_fifos(struct mali_c720 *mali_c720)
{
	const struct mali_c720_cm_fifo_regs *input_fifo = mali_c720->registers.cm->input_fifo;
	const struct mali_c720_cm_fifo_regs *reader_fifo = mali_c720->registers.cm->reader_fifo;
	const struct mali_c720_cm_temper_fifo_regs *temper_fifo =
		mali_c720->registers.cm->temper_fifo;
	struct mali_c720_ctx_mgr *ctx_mgr = &mali_c720->ctx_mgr;
	u32 fifo_base_addr, fifo_size;
	unsigned int fifo_users, i;

	fifo_users = ctx_mgr->num_virtual_inputs + 4; /* +4 for frame reader and temper FIFOs */
	fifo_size = ctx_mgr->input_fifo_size / fifo_users;

	fifo_base_addr = 0;
	for (i = 0; i < ctx_mgr->num_virtual_inputs; i++) {
		mali_c720_group_write(mali_c720, mali_c720_group_base(&input_fifo->group, i),
				      &input_fifo->base_address, fifo_base_addr);
		mali_c720_group_write(mali_c720, mali_c720_group_base(&input_fifo->group, i),
				      &input_fifo->size, fifo_size);

		fifo_base_addr += fifo_size;
	}

	mali_c720_group_write(mali_c720, mali_c720_group_base(&reader_fifo->group, 0),
			      &reader_fifo->base_address, fifo_base_addr);
	mali_c720_group_write(mali_c720, mali_c720_group_base(&reader_fifo->group, 0),
			      &reader_fifo->size, fifo_size);
	fifo_base_addr += fifo_size;

	mali_c720_group_write(mali_c720, mali_c720_group_base(&reader_fifo->group, 1),
			      &reader_fifo->base_address, fifo_base_addr);
	mali_c720_group_write(mali_c720, mali_c720_group_base(&reader_fifo->group, 1),
			      &reader_fifo->size, fifo_size);
	fifo_base_addr += fifo_size;

	mali_c720_write(mali_c720, &temper_fifo->w_fifo_base_address, fifo_base_addr);
	mali_c720_write(mali_c720, &temper_fifo->w_fifo_size, fifo_size);
	fifo_base_addr += fifo_size;

	mali_c720_write(mali_c720, &temper_fifo->r_fifo_base_address, fifo_base_addr);
	mali_c720_write(mali_c720, &temper_fifo->r_fifo_size, fifo_size);
}

static unsigned int mali_c720_ctx_mgr_get_output_max_fifos(unsigned int output)
{
	return output < MALI_C720_MAX_NUM_CAP_DEVS ? 3 : 0;
}

static u32 mali_c720_ctx_mgr_get_output_fifo_base(const struct mali_c720_registers *regs,
						  unsigned int output, unsigned int fifo)
{
	const struct mali_c720_cm_load_out_fifo_regs *load_out_fifo = regs->cm->load_out_fifo;

	switch (output) {
	case MALI_C720_CAP_DEV_FR0_RGB:
		return mali_c720_group_base(&load_out_fifo->scaler_group, fifo);
	case MALI_C720_CAP_DEV_FR0_YUV:
		return mali_c720_group_base(&load_out_fifo->scaler_group, 3 + fifo);
	default:
		WARN_ON_ONCE(1);
		return 0;
	}
}

static void mali_c720_ctx_mgr_prepare_output_fifos(struct mali_c720_context *ctx)
{
	struct mali_c720 *mali_c720 = ctx->mali_c720;
	const struct mali_c720_registers *regs = &mali_c720->registers;
	const struct mali_c720_cm_load_out_fifo_regs *load_out_fifo = regs->cm->load_out_fifo;
	struct mali_c720_ctx_mgr *ctx_mgr = &mali_c720->ctx_mgr;
	unsigned int i, j;
	u32 fifo_base_addr = 0;
	u32 fifo_size;
	unsigned int active_fifos[MALI_C720_MAX_NUM_CAP_DEVS];
	unsigned int total_fifos = 0;

	/*
	 * Distribute the output FIFO size among the active outputs,
	 * depending on FIFOs required by each output.
	 */

	memcpy(active_fifos, ctx->active_fifos_by_output, sizeof(active_fifos));

	for (i = 0; i < MALI_C720_MAX_NUM_CAP_DEVS; i++) {
		unsigned int max_fifos = mali_c720_ctx_mgr_get_output_max_fifos(i);

		if (WARN_ON_ONCE(active_fifos[i] > max_fifos))
			active_fifos[i] = max_fifos;

		total_fifos += active_fifos[i];
	}

	fifo_size = total_fifos ? ctx_mgr->output_fifo_size / total_fifos : 0;

	for (i = 0; i < MALI_C720_MAX_NUM_CAP_DEVS; i++) {
		unsigned int max_fifos = mali_c720_ctx_mgr_get_output_max_fifos(i);

		for (j = 0; j < max_fifos; j++) {
			u32 fifo_base = mali_c720_ctx_mgr_get_output_fifo_base(regs, i, j);
			u32 base_addr = 0;
			u32 size = 0;

			if (!fifo_base)
				continue;

			if (j < active_fifos[i]) {
				base_addr = fifo_base_addr;
				size = fifo_size;
				fifo_base_addr += fifo_size;
			}

			mali_c720_ctx_group_write(ctx, fifo_base,
						  &load_out_fifo->base_address,
						  base_addr);
			mali_c720_ctx_group_write(ctx, fifo_base,
						  &load_out_fifo->size,
						  size);
		}
	}
}

static void mali_c720_ctx_mgr_write_buffer_desc(struct mali_c720_context *ctx,
						struct mali_c720_buffer_pool *pool,
						unsigned int index)
{
	struct mali_c720 *mali_c720 = ctx->mali_c720;
	const struct mali_c720_registers *regs = &mali_c720->registers;
	const struct mali_c720_cm_buffer_regs *buffer = regs->cm->buffer;
	struct mali_c720_buffer_desc *desc;
	u32 buffer_addr = mali_c720_ctx_mgr_buffer_base(regs, pool, index);

	desc = &pool->desc[index];
	desc->crc = mali_c720_ctx_mgr_calc_buffer_desc_crc(regs, pool, index);

	mali_c720_group_write(mali_c720, buffer_addr,
			      &buffer->nonsecure, desc->nonsecure);
	mali_c720_group_write(mali_c720, buffer_addr,
			      &buffer->stream_id, desc->stream_id);
	mali_c720_group_write(mali_c720, buffer_addr,
			      &buffer->crc_check, desc->crc_check);
	mali_c720_group_write(mali_c720, buffer_addr,
			      &buffer->pool, desc->pool);
	mali_c720_group_write(mali_c720, buffer_addr,
			      &buffer->mode, desc->mode);
	mali_c720_group_write(mali_c720, buffer_addr,
			      &buffer->block_size, desc->block_size);
	mali_c720_group_write(mali_c720, buffer_addr,
			      &buffer->base_address_low, desc->base_address_low);
	mali_c720_group_write(mali_c720, buffer_addr,
			      &buffer->base_address_high, desc->base_address_high);
	mali_c720_group_write(mali_c720, buffer_addr,
			      &buffer->size, desc->buffer_size);
	mali_c720_group_write(mali_c720, buffer_addr,
			      &buffer->plane2_offset, desc->plane2_offset);
	mali_c720_group_write(mali_c720, buffer_addr,
			      &buffer->plane3_offset, desc->plane3_offset);
	mali_c720_group_write(mali_c720, buffer_addr,
			      &buffer->plane4_offset, desc->plane4_offset);
	mali_c720_group_write(mali_c720, buffer_addr,
			      &buffer->line1_offset, desc->line1_offset);
	mali_c720_group_write(mali_c720, buffer_addr,
			      &buffer->line2_offset, desc->line2_offset);
	mali_c720_group_write(mali_c720, buffer_addr,
			      &buffer->line3_offset, desc->line3_offset);
	mali_c720_group_write(mali_c720, buffer_addr,
			      &buffer->line4_offset, desc->line4_offset);
	mali_c720_group_write(mali_c720, buffer_addr,
			      &buffer->data_format, desc->data_format);
	mali_c720_group_write(mali_c720, buffer_addr,
			      &buffer->data_align, desc->data_align);
	mali_c720_group_write(mali_c720, buffer_addr,
			      &buffer->data_layout, desc->data_layout);
	mali_c720_group_write(mali_c720, buffer_addr,
			      &buffer->width, desc->width);
	mali_c720_group_write(mali_c720, buffer_addr,
			      &buffer->height, desc->height);
	mali_c720_group_write(mali_c720, buffer_addr,
			      &buffer->colorspace, desc->colorspace);
	mali_c720_group_write(mali_c720, buffer_addr,
			      &buffer->subsampling, desc->subsampling);
	mali_c720_group_write(mali_c720, buffer_addr,
			      &buffer->component_width, desc->component_width);
	mali_c720_group_write(mali_c720, buffer_addr,
			      &buffer->msb_align, desc->msb_align);
	mali_c720_group_write(mali_c720, buffer_addr,
			      &buffer->phase, desc->phase);
	mali_c720_group_write(mali_c720, buffer_addr,
			      &buffer->height_modulo, desc->height_modulo);
	mali_c720_group_write(mali_c720, buffer_addr,
			      &buffer->compression, desc->compression);
	mali_c720_group_write(mali_c720, buffer_addr,
			      &buffer->compression23, desc->compression23);
	mali_c720_group_write(mali_c720, buffer_addr,
			      &buffer->descriptor_crc, desc->crc);
}

static void mali_c720_ctx_mgr_update_buffer_desc(struct mali_c720_context *ctx,
						 struct mali_c720_buffer_pool *pool,
						 unsigned int index)
{
	struct mali_c720 *mali_c720 = ctx->mali_c720;
	const struct mali_c720_registers *regs = &mali_c720->registers;
	const struct mali_c720_cm_buffer_regs *buffer = regs->cm->buffer;
	struct mali_c720_buffer_desc *desc;
	u32 buffer_addr = mali_c720_ctx_mgr_buffer_base(regs, pool, index);

	desc = &pool->desc[index];
	desc->crc = mali_c720_ctx_mgr_calc_buffer_desc_crc(regs, pool, index);

	mali_c720_group_write(mali_c720, buffer_addr,
			      &buffer->base_address_low,
			      desc->base_address_low);
	mali_c720_group_write(mali_c720, buffer_addr,
			      &buffer->base_address_high,
			      desc->base_address_high);

	mali_c720_group_write(mali_c720, buffer_addr,
			      &buffer->descriptor_crc, desc->crc);
}

static void mali_c720_ctx_mgr_write_cdma_desc(struct mali_c720_context *ctx)
{
	struct mali_c720 *mali_c720 = ctx->mali_c720;
	const struct mali_c720_cm_cdma_slot_regs *cdma_slot = mali_c720->registers.cm->cdma_slot;
	struct mali_c720_cdma_desc *desc = &ctx->cdma_desc;
	u32 cdma_slot_addr = mali_c720_group_base(&cdma_slot->group, ctx->id);

	desc->crc = mali_c720_ctx_mgr_calc_cdma_desc_crc(ctx);

	mali_c720_group_write(mali_c720, cdma_slot_addr,
			      &cdma_slot->infinite, desc->infinite);
	mali_c720_group_write(mali_c720, cdma_slot_addr,
			      &cdma_slot->nonsecure, desc->nonsecure);
	mali_c720_group_write(mali_c720, cdma_slot_addr,
			      &cdma_slot->stream_id, desc->stream_id);
	mali_c720_group_write(mali_c720, cdma_slot_addr,
			      &cdma_slot->units_low, desc->units_low);
	mali_c720_group_write(mali_c720, cdma_slot_addr,
			      &cdma_slot->units_high, desc->units_high);
	mali_c720_group_write(mali_c720, cdma_slot_addr,
			      &cdma_slot->ld_sg_count, desc->ld_sg_count);
	mali_c720_group_write(mali_c720, cdma_slot_addr,
			      &cdma_slot->transfer_size, desc->transfer_size);
	mali_c720_group_write(mali_c720, cdma_slot_addr,
			      &cdma_slot->alt_transfer_size, desc->alt_transfer_size);
	mali_c720_group_write(mali_c720, cdma_slot_addr,
			      &cdma_slot->base_address_low, desc->base_address_low);
	mali_c720_group_write(mali_c720, cdma_slot_addr,
			      &cdma_slot->base_address_high, desc->base_address_high);
	mali_c720_group_write(mali_c720, cdma_slot_addr,
			      &cdma_slot->alt_base_address_low, desc->alt_base_address_low);
	mali_c720_group_write(mali_c720, cdma_slot_addr,
			      &cdma_slot->alt_base_address_high, desc->alt_base_address_high);
	mali_c720_group_write(mali_c720, cdma_slot_addr,
			      &cdma_slot->st_sg_count, desc->st_sg_count);
	mali_c720_group_write(mali_c720, cdma_slot_addr,
			      &cdma_slot->crc, desc->crc);
}

static void mali_c720_ctx_mgr_write_slot_desc(struct mali_c720_context *ctx)
{
	struct mali_c720 *mali_c720 = ctx->mali_c720;
	const struct mali_c720_cm_slot_regs *slot = mali_c720->registers.cm->slot;
	u32 slot_addr = mali_c720_group_base(&slot->group, ctx->id);

	ctx->slot_desc.crc = mali_c720_ctx_mgr_calc_slot_desc_crc(ctx);

	mali_c720_group_write(mali_c720, slot_addr,
			      &slot->mode, ctx->slot_desc.mode);
	mali_c720_group_write(mali_c720, slot_addr,
			      &slot->timeout_enable, ctx->slot_desc.timeout_enable);
	mali_c720_group_write(mali_c720, slot_addr,
			      &slot->timeout_mant, ctx->slot_desc.timeout_mant);
	mali_c720_group_write(mali_c720, slot_addr,
			      &slot->timeout_exp, ctx->slot_desc.timeout_exp);
	mali_c720_group_write(mali_c720, slot_addr,
			      &slot->schedule_priority, ctx->slot_desc.schedule_priority);
	mali_c720_group_write(mali_c720, slot_addr,
			      &slot->nonsecure, ctx->slot_desc.nonsecure);
	mali_c720_group_write(mali_c720, slot_addr,
			      &slot->stream_id, ctx->slot_desc.stream_id);
	mali_c720_group_write(mali_c720, slot_addr,
			      &slot->delay_mant, ctx->slot_desc.delay_mant);
	mali_c720_group_write(mali_c720, slot_addr,
			      &slot->delay_exp, ctx->slot_desc.delay_exp);
	mali_c720_group_write(mali_c720, slot_addr,
			      &slot->slot_event, ctx->slot_desc.slot_event);
	mali_c720_group_write(mali_c720, slot_addr,
			      &slot->slot_input_0, ctx->slot_desc.slot_input_0);
	mali_c720_group_write(mali_c720, slot_addr,
			      &slot->slot_input_1, ctx->slot_desc.slot_input_1);
	mali_c720_group_write(mali_c720, slot_addr,
			      &slot->pool_roi0, ctx->slot_desc.pool_roi0);
	mali_c720_group_write(mali_c720, slot_addr,
			      &slot->pool_roi1, ctx->slot_desc.pool_roi1);
	mali_c720_group_write(mali_c720, slot_addr,
			      &slot->pool_roi2, ctx->slot_desc.pool_roi2);
	mali_c720_group_write(mali_c720, slot_addr,
			      &slot->pool_raw, ctx->slot_desc.pool_raw);
	mali_c720_group_write(mali_c720, slot_addr,
			      &slot->pool_scaler0_rgb, ctx->slot_desc.pool_scaler0_rgb);
	mali_c720_group_write(mali_c720, slot_addr,
			      &slot->pool_scaler0_yuv, ctx->slot_desc.pool_scaler0_yuv);
	mali_c720_group_write(mali_c720, slot_addr,
			      &slot->pool_scaler1_rgb, ctx->slot_desc.pool_scaler1_rgb);
	mali_c720_group_write(mali_c720, slot_addr,
			      &slot->pool_scaler1_yuv, ctx->slot_desc.pool_scaler1_yuv);
	mali_c720_group_write(mali_c720, slot_addr,
			      &slot->pool_scaler2_rgb, ctx->slot_desc.pool_scaler2_rgb);
	mali_c720_group_write(mali_c720, slot_addr,
			      &slot->pool_scaler2_yuv, ctx->slot_desc.pool_scaler2_yuv);
	mali_c720_group_write(mali_c720, slot_addr,
			      &slot->pool_scaler3_rgb, ctx->slot_desc.pool_scaler3_rgb);
	mali_c720_group_write(mali_c720, slot_addr,
			      &slot->pool_scaler3_yuv, ctx->slot_desc.pool_scaler3_yuv);
	mali_c720_group_write(mali_c720, slot_addr,
			      &slot->pool_gdc_main, ctx->slot_desc.pool_gdc_main);
	mali_c720_group_write(mali_c720, slot_addr,
			      &slot->pool_gdc_roi0, ctx->slot_desc.pool_gdc_roi0);
	mali_c720_group_write(mali_c720, slot_addr,
			      &slot->pool_gdc_roi1, ctx->slot_desc.pool_gdc_roi1);
	mali_c720_group_write(mali_c720, slot_addr,
			      &slot->pool_gdc_roi2, ctx->slot_desc.pool_gdc_roi2);
	mali_c720_group_write(mali_c720, slot_addr,
			      &slot->pool_pyramid, ctx->slot_desc.pool_pyramid);
	mali_c720_group_write(mali_c720, slot_addr,
			      &slot->pool_consistency, ctx->slot_desc.pool_consistency);
	mali_c720_group_write(mali_c720, slot_addr,
			      &slot->crc, ctx->slot_desc.crc);
}

static void mali_c720_ctx_mgr_update_slot_desc(struct mali_c720_context *ctx)
{
	struct mali_c720 *mali_c720 = ctx->mali_c720;
	const struct mali_c720_cm_slot_regs *slot = mali_c720->registers.cm->slot;
	u32 slot_addr = mali_c720_group_base(&slot->group, ctx->id);

	ctx->slot_desc.crc = mali_c720_ctx_mgr_calc_slot_desc_crc(ctx);

	/*
	 * A running slot must be stopped before modifying descriptor fields
	 * to prevent CRC mismatch faults from being raised.
	 */
	mali_c720_ctx_mgr_set_slot_enabled(ctx, false);

	mali_c720_group_write(mali_c720, slot_addr,
			      &slot->pool_roi0, ctx->slot_desc.pool_roi0);
	mali_c720_group_write(mali_c720, slot_addr,
			      &slot->pool_roi1, ctx->slot_desc.pool_roi1);
	mali_c720_group_write(mali_c720, slot_addr,
			      &slot->pool_roi2, ctx->slot_desc.pool_roi2);
	mali_c720_group_write(mali_c720, slot_addr,
			      &slot->pool_raw, ctx->slot_desc.pool_raw);
	mali_c720_group_write(mali_c720, slot_addr,
			      &slot->pool_scaler0_rgb, ctx->slot_desc.pool_scaler0_rgb);
	mali_c720_group_write(mali_c720, slot_addr,
			      &slot->pool_scaler0_yuv, ctx->slot_desc.pool_scaler0_yuv);
	mali_c720_group_write(mali_c720, slot_addr,
			      &slot->pool_scaler1_rgb, ctx->slot_desc.pool_scaler1_rgb);
	mali_c720_group_write(mali_c720, slot_addr,
			      &slot->pool_scaler1_yuv, ctx->slot_desc.pool_scaler1_yuv);
	mali_c720_group_write(mali_c720, slot_addr,
			      &slot->pool_scaler2_rgb, ctx->slot_desc.pool_scaler2_rgb);
	mali_c720_group_write(mali_c720, slot_addr,
			      &slot->pool_scaler2_yuv, ctx->slot_desc.pool_scaler2_yuv);
	mali_c720_group_write(mali_c720, slot_addr,
			      &slot->pool_scaler3_rgb, ctx->slot_desc.pool_scaler3_rgb);
	mali_c720_group_write(mali_c720, slot_addr,
			      &slot->pool_scaler3_yuv, ctx->slot_desc.pool_scaler3_yuv);
	mali_c720_group_write(mali_c720, slot_addr,
			      &slot->pool_gdc_main, ctx->slot_desc.pool_gdc_main);
	mali_c720_group_write(mali_c720, slot_addr,
			      &slot->pool_gdc_roi0, ctx->slot_desc.pool_gdc_roi0);
	mali_c720_group_write(mali_c720, slot_addr,
			      &slot->pool_gdc_roi1, ctx->slot_desc.pool_gdc_roi1);
	mali_c720_group_write(mali_c720, slot_addr,
			      &slot->pool_gdc_roi2, ctx->slot_desc.pool_gdc_roi2);
	mali_c720_group_write(mali_c720, slot_addr,
			      &slot->pool_pyramid, ctx->slot_desc.pool_pyramid);
	mali_c720_group_write(mali_c720, slot_addr,
			      &slot->pool_consistency, ctx->slot_desc.pool_consistency);
	mali_c720_group_write(mali_c720, slot_addr,
			      &slot->crc, ctx->slot_desc.crc);

	mali_c720_ctx_mgr_set_slot_enabled(ctx, true);
}

static int mali_c720_ctx_mgr_start_input_port(struct mali_c720 *mali_c720,
					      struct mali_c720_input_cfg *info)
{
	const struct mali_c720_cm_input_regs *input = mali_c720->registers.cm->input;
	u32 input_addr = mali_c720_group_base(&input->group, info->id);
	u32 val;
	int ret;

	mali_c720_group_write(mali_c720, input_addr,
			      &input->pixel_size, info->bit_depth);
	mali_c720_group_write(mali_c720, input_addr,
			      &input->data_format, 1);
	mali_c720_group_write(mali_c720, input_addr,
			      &input->nonsecure, 1);
	mali_c720_group_write(mali_c720, input_addr,
			      &input->stream_id, 0);
	mali_c720_group_write(mali_c720, input_addr,
			      &input->data_type1_enable, 0);
	mali_c720_group_write(mali_c720, input_addr,
			      &input->data_type2_enable, 0);
	mali_c720_group_write(mali_c720, input_addr,
			      &input->crop_enable, 0);
	mali_c720_group_write(mali_c720, input_addr,
			      &input->drop_oldest, 0);
	mali_c720_group_write(mali_c720, input_addr,
			      &input->flow_control, 0);
	mali_c720_group_write(mali_c720, input_addr,
			      &input->drop_if_error, 0);
	mali_c720_group_write(mali_c720, input_addr,
			      &input->check_parity, 0);
	mali_c720_group_write(mali_c720, input_addr,
			      &input->buffer_pool, info->pool_id);
	mali_c720_group_write(mali_c720, input_addr,
			      &input->data_type1, 0);
	mali_c720_group_write(mali_c720, input_addr,
			      &input->data_type2, 0);
	mali_c720_group_write(mali_c720, input_addr,
			      &input->start_x, 0);
	mali_c720_group_write(mali_c720, input_addr,
			      &input->start_y, 0);
	mali_c720_group_write(mali_c720, input_addr,
			      &input->size_x, 0);
	mali_c720_group_write(mali_c720, input_addr,
			      &input->size_y, 0);
	mali_c720_group_write(mali_c720, input_addr,
			      &input->write_threshold, 65535);
	mali_c720_group_write(mali_c720, input_addr,
			      &input->write_threshold_sec, 65535);

	mali_c720_ctx_mgr_input_command(mali_c720, input_addr,
					MALI_C720_INPUT_COMMAND_RESET_COUNTER);
	mali_c720_ctx_mgr_input_command(mali_c720, input_addr, MALI_C720_INPUT_COMMAND_ENABLE);

	ret = poll_timeout_us(val = mali_c720_group_read(mali_c720, input_addr,
							 &input->status_arch),
			      val == MALI_C720_INPUT_ENABLED,
			      INPUT_POLL_INTERVAL, INPUT_POLL_TIMEOUT, false);
	if (ret)
		goto err_timeout;

	info->started = true;

	return 0;

err_timeout:
	dev_err(mali_c720->dev, "Timed out enabling input %u (state %u)\n", info->id, val);
	mali_c720_ctx_mgr_input_command(mali_c720, input_addr, MALI_C720_INPUT_COMMAND_CANCEL);
	mali_c720_ctx_mgr_input_command(mali_c720, input_addr, MALI_C720_INPUT_COMMAND_FLUSH);
	mali_c720_group_write(mali_c720, input_addr,
			      &input->buffer_pool, 0xff);

	return ret;
}

static void mali_c720_ctx_mgr_stop_input_port(struct mali_c720 *mali_c720,
					      struct mali_c720_input_cfg *info)
{
	const struct mali_c720_cm_buffer_regs *buffer = mali_c720->registers.cm->buffer;
	const struct mali_c720_cm_input_regs *input = mali_c720->registers.cm->input;
	struct mali_c720_ctx_mgr *ctx_mgr = &mali_c720->ctx_mgr;
	u32 buffer_addr = mali_c720_group_base(&buffer->raw_group, ctx_mgr->reuse_buffer);
	u32 input_addr = mali_c720_group_base(&input->group, info->id);
	u32 val;
	int ret;

	if (!info->started)
		return;

	mali_c720_ctx_mgr_input_command(mali_c720, input_addr, MALI_C720_INPUT_COMMAND_DISABLE);
	ret = poll_timeout_us(val = mali_c720_group_read(mali_c720, input_addr,
							 &input->status_arch),
			      val == MALI_C720_INPUT_DISABLED,
			      INPUT_POLL_INTERVAL, INPUT_POLL_TIMEOUT, false);
	if (ret) {
		/* Force the input gate closed if the source does not reach vertical blank. */
		mali_c720_ctx_mgr_input_command(mali_c720, input_addr,
						MALI_C720_INPUT_COMMAND_CANCEL);
	}

	mali_c720_ctx_mgr_input_command(mali_c720, input_addr, MALI_C720_INPUT_COMMAND_FLUSH);

	/*
	 * The following is a workaround for a hardware bug that prevents
	 * safe reuse of raw buffer descriptors. It applies to HW versions
	 * r0p0 and r0p1.
	 */
	mali_c720_ctx_mgr_buffer_request(mali_c720, buffer_addr,
					 MALI_C720_BUFFER_STATE_HW_EMPTY);

	mali_c720_group_write(mali_c720, input_addr, &input->buffer_pool, ctx_mgr->reuse_buffer);
	mali_c720_ctx_mgr_input_command(mali_c720, input_addr, MALI_C720_INPUT_COMMAND_ENABLE);

	/*
	 * We require the input to fully enable to ensure proper reuse. Enablement
	 * completes once it sees vertical blanking on its source. Upon reaching
	 * this state, the reuse buffer will enter an error state which we will reset
	 * during the buffer cancel operation.
	 */
	ret = poll_timeout_us(val = mali_c720_group_read(mali_c720, buffer_addr, &buffer->state),
			      val == MALI_C720_BUFFER_STATE_HW_ERROR,
			      INPUT_POLL_INTERVAL, INPUT_POLL_TIMEOUT, false);
	if (ret)
		dev_warn(mali_c720->dev,
			 "Input %u descriptor reuse workaround timed out (buffer state %u)\n",
			 info->id, val);

	mali_c720_ctx_mgr_input_command(mali_c720, input_addr, MALI_C720_INPUT_COMMAND_CANCEL);
	mali_c720_ctx_mgr_input_command(mali_c720, input_addr, MALI_C720_INPUT_COMMAND_FLUSH);

	mali_c720_group_write(mali_c720, input_addr, &input->buffer_pool, 0xff);

	mali_c720_ctx_mgr_buffer_request(mali_c720, buffer_addr,
					 MALI_C720_BUFFER_STATE_CANCELLED);
	mali_c720_ctx_mgr_buffer_request(mali_c720, buffer_addr,
					 MALI_C720_BUFFER_STATE_UNINITIALIZED);

	info->started = false;
}

int mali_c720_ctx_mgr_start_context_inputs(struct mali_c720_context *ctx)
{
	struct mali_c720 *mali_c720 = ctx->mali_c720;

	lockdep_assert_held(&mali_c720->device_lock);

	return mali_c720_ctx_mgr_start_input_port(mali_c720, &ctx->input_cfg);
}

void mali_c720_ctx_mgr_stop_context_inputs(struct mali_c720_context *ctx)
{
	struct mali_c720 *mali_c720 = ctx->mali_c720;
	struct mali_c720_ctx_mgr *ctx_mgr = &mali_c720->ctx_mgr;
	struct mali_c720_input_cfg *input_cfg = &ctx->input_cfg;

	lockdep_assert_held(&mali_c720->device_lock);

	mali_c720_ctx_mgr_stop_input_port(mali_c720, input_cfg);

	guard(spinlock)(&ctx_mgr->input_lock);
	guard(spinlock)(&ctx->state_lock);

	ctx_mgr->input_context_map[input_cfg->id] = 0xff;
	memset(&ctx->input_cfg, 0, sizeof(ctx->input_cfg));
	ctx->slot_desc.slot_input_0 = 255;
	ctx->slot_desc.slot_input_1 = 255;
}

static void mali_c720_ctx_mgr_assign_context_output(struct mali_c720_context *ctx,
						    struct mali_c720_buffer_pool *pool,
						    int enable)
{
	struct mali_c720 *mali_c720 = ctx->mali_c720;
	unsigned int pool_id = enable ? pool->pool_id : 255;
	u8 *slot_output = NULL;

	switch (pool->sub_type) {
	case MALI_C720_CAP_DEV_FR0_RGB:
		slot_output = &ctx->slot_desc.pool_scaler0_rgb;
		break;
	case MALI_C720_CAP_DEV_FR0_YUV:
		slot_output = &ctx->slot_desc.pool_scaler0_yuv;
		break;
	default:
		dev_warn_once(mali_c720->dev,
			      "Invalid output pool subtype %u\n", pool->sub_type);
		return;
	}

	if (slot_output && *slot_output != pool_id) {
		*slot_output = (u8)pool_id;
		ctx->active_fifos_by_output[pool->sub_type] =
			enable ? pool->buffer_cfg->num_fifos : 0;
		ctx->slot_desc_updated = true;
	}
}

static void
mali_c720_ctx_mgr_relinquish_buffer_control(struct mali_c720_context *ctx,
					    struct mali_c720_buffer_pool *pool,
					    unsigned int index)
{
	struct mali_c720 *mali_c720 = ctx->mali_c720;
	const struct mali_c720_registers *regs = &mali_c720->registers;
	u32 buffer_addr = mali_c720_ctx_mgr_buffer_base(regs, pool, index);
	u32 buffer_state;

	/* Return control of the buffer back to HW */
	buffer_state = MALI_C720_BUFFER_STATE_HW_EMPTY;

	mali_c720_ctx_mgr_buffer_request(mali_c720, buffer_addr, buffer_state);
}

static int mali_c720_ctx_mgr_alloc_buffer_pool(unsigned long *buffers,
					       unsigned int num_buffers,
					       unsigned int num_required)
{
	unsigned long start;

	if (num_required == 0 || num_required > num_buffers)
		return -EINVAL;

	start = bitmap_find_next_zero_area(buffers,
					   num_buffers,
					   0,
					   num_required,
					   0);

	if (start >= num_buffers)
		return -ENOSPC;

	bitmap_set(buffers, (unsigned int)start, num_required);

	return (int)start;
}

static int
mali_c720_ctx_mgr_release_buffer_descriptors(struct mali_c720 *mali_c720,
					     struct mali_c720_buffer_pool *pool)
{
	struct mali_c720_ctx_mgr *ctx_mgr = &mali_c720->ctx_mgr;
	unsigned int num_descriptors;
	unsigned long *descriptors;

	switch (pool->type) {
	case MALI_C720_BUF_TYPE_RAW:
		descriptors = ctx_mgr->raw_buffer_descriptors;
		num_descriptors = ctx_mgr->num_raw_buffer_descriptors;
		break;
	case MALI_C720_BUF_TYPE_OUT:
		descriptors = ctx_mgr->out_buffer_descriptors;
		num_descriptors = ctx_mgr->num_out_buffer_descriptors;
		break;
	default:
		WARN_ON_ONCE(1);
		return -EINVAL;
	}

	if (pool->pool_id >= num_descriptors ||
	    pool->num_buffers > num_descriptors - pool->pool_id) {
		WARN_ON_ONCE(1);
		return -EINVAL;
	}

	bitmap_clear(descriptors, pool->pool_id, pool->num_buffers);

	return 0;
}

int mali_c720_ctx_mgr_request_buffer_pool(struct mali_c720_context *ctx,
					  const struct mali_c720_buffer_request_info *info,
					  struct mali_c720_buffer_pool *pool)
{
	struct mali_c720 *mali_c720 = ctx->mali_c720;
	const struct mali_c720_registers *regs = &mali_c720->registers;
	struct mali_c720_ctx_mgr *ctx_mgr = &mali_c720->ctx_mgr;
	const struct mali_c720_buffer_cfg *cfg = info->buffer_cfg;
	size_t plane_offset[MALI_C720_MAX_BUFFER_PLANES] = {0};
	u32 buffer_addr, buffer_mode;
	unsigned int i;
	int ret;

	lockdep_assert_held(&mali_c720->device_lock);

	switch (info->type) {
	case MALI_C720_BUF_TYPE_RAW:
		if (WARN_ONCE(info->num_raw_buffers > ARRAY_SIZE(pool->desc),
			      "Invalid raw buffer count %u\n",
			      info->num_raw_buffers))
			return -EINVAL;

		ret = mali_c720_ctx_mgr_alloc_buffer_pool(ctx_mgr->raw_buffer_descriptors,
							  ctx_mgr->num_raw_buffer_descriptors,
							  info->num_raw_buffers);
		if (ret < 0)
			return ret;

		pool->width = info->width;
		pool->height = info->height;
		pool->type = info->type;
		pool->sub_type = info->sub_type;
		pool->num_buffers = info->num_raw_buffers;
		pool->pool_id = (u8)ret;

		buffer_mode = MALI_C720_RAW_BUFFER_MODE_NORMAL;

		break;
	case MALI_C720_BUF_TYPE_OUT:
		if (WARN_ONCE(info->num_out_planes > ARRAY_SIZE(plane_offset),
			      "Invalid output plane count %u\n",
			      info->num_out_planes))
			return -EINVAL;

		ret = mali_c720_ctx_mgr_alloc_buffer_pool(ctx_mgr->out_buffer_descriptors,
							  ctx_mgr->num_out_buffer_descriptors,
							  1);
		if (ret < 0)
			return ret;

		pool->width = info->width;
		pool->height = info->height;
		pool->type = info->type;
		pool->sub_type = info->sub_type;
		pool->num_buffers = 1;
		pool->pool_id = (u8)ret;

		buffer_mode = MALI_C720_OUT_BUFFER_MODE_NORMAL;

		break;
	default:
		WARN_ON_ONCE(1);
		return -EINVAL;
	}

	pool->buffer_size = 0;
	pool->buffer_cfg = cfg;
	memset(pool->desc, 0, sizeof(pool->desc));

	if (pool->type == MALI_C720_BUF_TYPE_OUT) {
		for (i = 0; i < info->num_out_planes; i++) {
			plane_offset[i] = pool->buffer_size;
			pool->buffer_size += info->plane_size[i];
		}
	} else {
		pool->buffer_size = info->plane_size[0];
	}

	for (i = 0; i < pool->num_buffers; i++) {
		buffer_addr = mali_c720_ctx_mgr_buffer_base(regs, pool, i);

		/* Initialize all buffers under SW control */
		mali_c720_ctx_mgr_buffer_request(mali_c720, buffer_addr,
						 MALI_C720_BUFFER_STATE_SW_EMPTY);

		pool->desc[i].nonsecure = 1;
		pool->desc[i].stream_id = 0;
		pool->desc[i].crc_check = 0;
		pool->desc[i].pool = (i + 1 < pool->num_buffers) ? 1 : 0;
		pool->desc[i].mode = (u8)buffer_mode;
		pool->desc[i].block_size = 0;
		pool->desc[i].buffer_size = (u32)pool->buffer_size;
		pool->desc[i].plane2_offset = (u32)plane_offset[1];
		pool->desc[i].plane3_offset = (u32)plane_offset[2];
		pool->desc[i].plane4_offset = (u32)plane_offset[3];
		pool->desc[i].line1_offset = (u32)info->line_offset[0];
		pool->desc[i].line2_offset = (u32)info->line_offset[1];
		pool->desc[i].line3_offset = (u32)info->line_offset[2];
		pool->desc[i].line4_offset = (u32)info->line_offset[3];
		pool->desc[i].data_format = (u8)cfg->data_format;
		pool->desc[i].data_align = (u8)cfg->data_align;
		pool->desc[i].data_layout = (u8)cfg->data_layout;
		pool->desc[i].width = (u16)info->width;
		pool->desc[i].height = (u16)info->height;
		pool->desc[i].height_modulo = 0;
		pool->desc[i].compression = 0;
		pool->desc[i].compression23 = 0;

		mali_c720_ctx_mgr_write_buffer_desc(ctx, pool, i);
	}

	return 0;
}

void mali_c720_ctx_mgr_release_buffer_pool(struct mali_c720_context *ctx,
					   struct mali_c720_buffer_pool *pool)
{
	struct mali_c720 *mali_c720 = ctx->mali_c720;
	const struct mali_c720_registers *regs = &mali_c720->registers;
	u32 buffer_addr;
	unsigned int i;

	lockdep_assert_held(&mali_c720->device_lock);

	/*
	 * Raw buffers must be explicitly cancelled to return them to SW control
	 * before being unassigned. Output buffers should typically be under SW
	 * control at this point already, but it is safe to cancel them here as well.
	 */
	for (i = 0; i < pool->num_buffers; i++) {
		buffer_addr = mali_c720_ctx_mgr_buffer_base(regs, pool, i);

		mali_c720_ctx_mgr_buffer_request(mali_c720, buffer_addr,
						 MALI_C720_BUFFER_STATE_CANCELLED);
		mali_c720_ctx_mgr_buffer_request(mali_c720, buffer_addr,
						 MALI_C720_BUFFER_STATE_UNINITIALIZED);
	}

	mali_c720_ctx_mgr_release_buffer_descriptors(mali_c720, pool);
}

int mali_c720_ctx_mgr_check_output_buffer_status(struct mali_c720_context *ctx,
						 struct mali_c720_buffer_pool *pool)
{
	struct mali_c720 *mali_c720 = ctx->mali_c720;
	const struct mali_c720_registers *regs = &mali_c720->registers;
	const struct mali_c720_cm_buffer_regs *buffer = regs->cm->buffer;
	u32 buffer_addr = mali_c720_ctx_mgr_buffer_base(regs, pool, 0);
	u32 state;

	lockdep_assert_held(&ctx->state_lock);

	if (ctx->state == MALI_C720_SLOT_STATE_STOPPED)
		return 0;

	state = mali_c720_group_read(mali_c720, buffer_addr, &buffer->state);
	if (state == MALI_C720_BUFFER_STATE_SW_FULL)
		return 0;

	dev_warn_ratelimited(mali_c720->dev, "Output buffer %u failed (state %u), recovering\n",
			     pool->pool_id, state);

	mali_c720_ctx_mgr_buffer_request(mali_c720, buffer_addr,
					 MALI_C720_BUFFER_STATE_CANCELLED);
	mali_c720_ctx_mgr_buffer_request(mali_c720, buffer_addr,
					 MALI_C720_BUFFER_STATE_SW_EMPTY);

	return -EIO;
}

void mali_c720_ctx_mgr_update_buffer(struct mali_c720_context *ctx,
				     struct mali_c720_buffer_pool *pool,
				     dma_addr_t *addr,
				     unsigned int index)
{
	/*
	 * If we don't have a new address, clear the descriptor and leave it
	 * under SW control to ensure HW cannot write to an invalid address.
	 * For output buffers we will disable that particular output.
	 */
	if (!addr) {
		if (WARN_ONCE(pool->type != MALI_C720_BUF_TYPE_OUT,
			      "Cannot clear non-output buffer type %u\n",
			      pool->type))
			return;

		mali_c720_ctx_mgr_assign_context_output(ctx, pool, 0);

		pool->desc[index].base_address_low = 0;
		pool->desc[index].base_address_high = 0;

		mali_c720_ctx_mgr_update_buffer_desc(ctx, pool, index);

		return;
	}

	/* Once the buffer is updated, return control of it to the HW */
	if (pool->type == MALI_C720_BUF_TYPE_OUT)
		mali_c720_ctx_mgr_assign_context_output(ctx, pool, 1);

	pool->desc[index].base_address_low = lower_32_bits(*addr);
	pool->desc[index].base_address_high = upper_32_bits(*addr);

	mali_c720_ctx_mgr_update_buffer_desc(ctx, pool, index);
	mali_c720_ctx_mgr_relinquish_buffer_control(ctx, pool, index);
}

void mali_c720_ctx_mgr_prepare_frame(struct mali_c720_context *ctx)
{
	lockdep_assert_held(&ctx->state_lock);
	WARN_ON(ctx->state != MALI_C720_SLOT_STATE_STARTING &&
		ctx->state != MALI_C720_SLOT_STATE_STARTED);

	if (ctx->state == MALI_C720_SLOT_STATE_STARTING) {
		mali_c720_ctx_mgr_prepare_output_fifos(ctx);
		mali_c720_ctx_mgr_write_slot_desc(ctx);
		mali_c720_ctx_mgr_set_slot_enabled(ctx, true);

		ctx->state = MALI_C720_SLOT_STATE_STARTED;
	} else if (ctx->slot_desc_updated) {
		mali_c720_ctx_mgr_prepare_output_fifos(ctx);
		mali_c720_ctx_mgr_update_slot_desc(ctx);
	}
	ctx->slot_desc_updated = false;
}

void mali_c720_ctx_mgr_buffer_done(struct mali_c720_context *ctx, bool success)
{
	struct mali_c720 *mali_c720 = ctx->mali_c720;
	unsigned int i;

	lockdep_assert_held(&ctx->state_lock);

	for (i = 0; i < MALI_C720_MAX_NUM_CAP_DEVS; i++)
		mali_c720_cap_dev_buffer_done(&mali_c720->cap_devs[i], success);
}

void mali_c720_ctx_mgr_handle_sw_ready(struct mali_c720_context *ctx)
{
	struct mali_c720 *mali_c720 = ctx->mali_c720;
	const struct mali_c720_cm_cdma_slot_regs *cdma_slot = mali_c720->registers.cm->cdma_slot;

	mali_c720_ctx_mgr_cdma_request(mali_c720, mali_c720_group_base(&cdma_slot->group, ctx->id),
				       MALI_C720_SLOT_CDMA_STATE_HW_CONTROL);
}

void mali_c720_ctx_mgr_configure_context(struct mali_c720_context *ctx,
					 const struct mali_c720_top_cfg *cfg)
{
	struct mali_c720 *mali_c720 = ctx->mali_c720;
	const struct mali_c720_registers *regs = &mali_c720->registers;
	const struct mali_c720_pipeline_regs *pipeline = regs->top->pipeline;
	dma_addr_t ctx_regs_addr = ctx->regs.dma_handle;

	mali_c720_ctx_group_write(ctx, mali_c720->top_base,
				  &pipeline->active_width, cfg->width);
	mali_c720_ctx_group_write(ctx, mali_c720->top_base,
				  &pipeline->active_height, cfg->height);
	mali_c720_ctx_group_write(ctx, mali_c720->top_base,
				  &pipeline->rggb_start, cfg->rggb_start);

	ctx->cdma_desc.infinite = 0;
	ctx->cdma_desc.nonsecure = 1;
	ctx->cdma_desc.stream_id = 0;
	ctx->cdma_desc.transfer_size = regs->context_size;
	ctx->cdma_desc.units_low = 0xfff03;
	ctx->cdma_desc.units_high = 0;
	ctx->cdma_desc.base_address_low = lower_32_bits(ctx_regs_addr);
	ctx->cdma_desc.base_address_high = upper_32_bits(ctx_regs_addr);
	ctx->cdma_desc.ld_sg_count = 0;
	ctx->cdma_desc.st_sg_count = 0;
	ctx->cdma_desc.alt_transfer_size = 0;
	ctx->cdma_desc.alt_base_address_low = 0;
	ctx->cdma_desc.alt_base_address_high = 0;

	ctx->slot_desc.mode = MALI_C720_SLOT_MODE_DISABLED;
	ctx->slot_desc.timeout_enable = 0;
	ctx->slot_desc.schedule_priority = 0;
	ctx->slot_desc.nonsecure = 1;
	ctx->slot_desc.stream_id = 0;
	ctx->slot_desc.delay_mant = 0;
	ctx->slot_desc.delay_exp = 0;
	ctx->slot_desc.slot_event = 0;
	ctx->slot_desc.slot_input_0 = 255;
	ctx->slot_desc.slot_input_1 = 255;
	ctx->slot_desc.timeout_mant = 0;
	ctx->slot_desc.timeout_exp = 0;
	ctx->slot_desc.pool_roi0 = 255;
	ctx->slot_desc.pool_roi1 = 255;
	ctx->slot_desc.pool_roi2 = 255;
	ctx->slot_desc.pool_raw = 255;
	ctx->slot_desc.pool_scaler0_rgb = 255;
	ctx->slot_desc.pool_scaler0_yuv = 255;
	ctx->slot_desc.pool_scaler1_rgb = 255;
	ctx->slot_desc.pool_scaler1_yuv = 255;
	ctx->slot_desc.pool_scaler2_rgb = 255;
	ctx->slot_desc.pool_scaler2_yuv = 255;
	ctx->slot_desc.pool_scaler3_rgb = 255;
	ctx->slot_desc.pool_scaler3_yuv = 255;
	ctx->slot_desc.pool_gdc_main = 255;
	ctx->slot_desc.pool_gdc_roi0 = 255;
	ctx->slot_desc.pool_gdc_roi1 = 255;
	ctx->slot_desc.pool_gdc_roi2 = 255;
	ctx->slot_desc.pool_pyramid = 255;
	ctx->slot_desc.pool_consistency = 255;
	memset(ctx->active_fifos_by_output, 0, sizeof(ctx->active_fifos_by_output));
	ctx->slot_desc_updated = false;

	mali_c720_ctx_mgr_write_cdma_desc(ctx);
}

void mali_c720_ctx_mgr_prepare_context(struct mali_c720_context *ctx,
				       const struct mali_c720_input_cfg *input_cfg)
{
	struct mali_c720 *mali_c720 = ctx->mali_c720;
	const struct mali_c720_cm_cdma_slot_regs *cdma_slot = mali_c720->registers.cm->cdma_slot;
	struct mali_c720_ctx_mgr *ctx_mgr = &mali_c720->ctx_mgr;
	u32 cdma_slot_addr = mali_c720_group_base(&cdma_slot->group, ctx->id);

	lockdep_assert_held(&mali_c720->device_lock);

	guard(spinlock)(&ctx_mgr->input_lock);
	guard(spinlock)(&ctx->state_lock);

	memset(ctx->frame_timestamps, 0, sizeof(ctx->frame_timestamps));
	ctx->input_frame_sequence = 0;
	ctx->frame_sequence = 0;
	ctx->timestamp = ktime_get_ns();

	ctx->input_cfg = *input_cfg;
	ctx->slot_desc.slot_input_0 = (u8)input_cfg->id;
	ctx->slot_desc.slot_input_1 = 255;

	ctx->input_cfg.started = false;
	ctx_mgr->input_context_map[input_cfg->id] = (u8)ctx->id;

	ctx->slot_desc.mode = MALI_C720_SLOT_MODE_TDMF_FLEX;
	ctx->slot_desc_updated = false;

	/* Defer frame scheduling until all video devices are started. */
	ctx->state = MALI_C720_SLOT_STATE_PREPARED;
	ctx->pipeline_busy = false;

	if (mali_c720->num_active_contexts++ == 0) {
		mali_c720_prepare_input_fifos(mali_c720);

		mali_c720_ctx_mgr_set_scheduler_enabled(mali_c720, true);
	}

	mali_c720_set_slot_interrupt(mali_c720, ctx->id, true);
	mali_c720_set_frontend_interrupts(ctx, true);

	mali_c720_ctx_mgr_cdma_request(mali_c720, cdma_slot_addr,
				       MALI_C720_SLOT_CDMA_STATE_CANCELLED);
}

void mali_c720_ctx_mgr_start_context(struct mali_c720_context *ctx)
{
	lockdep_assert_held(&ctx->mali_c720->device_lock);
	lockdep_assert_held(&ctx->state_lock);

	ctx->state = MALI_C720_SLOT_STATE_STARTING;
}

int mali_c720_ctx_mgr_check_context_status(struct mali_c720_context *ctx)
{
	struct mali_c720 *mali_c720 = ctx->mali_c720;
	const struct mali_c720_cm_slot_regs *slot = mali_c720->registers.cm->slot;
	const struct mali_c720_cm_buffer_regs *buffer = mali_c720->registers.cm->buffer;
	const struct mali_c720_cm_cdma_slot_regs *cdma_slot = mali_c720->registers.cm->cdma_slot;
	u32 slot_addr = mali_c720_group_base(&slot->group, ctx->id);
	u32 temper_addr = mali_c720_group_base(&buffer->temper_group, ctx->id);
	u32 state;

	lockdep_assert_held(&ctx->state_lock);

	state = mali_c720_group_read(mali_c720, slot_addr, &slot->status);
	if (state == MALI_C720_SLOT_ENABLED)
		return 0;

	dev_warn_ratelimited(mali_c720->dev, "Slot %u failed (state %u), recovering\n",
			     ctx->id, state);

	/* Slot error state can only be cleared by cancelling the slot. */
	mali_c720_ctx_mgr_cancel_slot(ctx);

	mali_c720_ctx_mgr_cdma_request(mali_c720, mali_c720_group_base(&cdma_slot->group, ctx->id),
				       MALI_C720_SLOT_CDMA_STATE_CANCELLED);

	if (mali_c720_group_read(mali_c720, temper_addr, &buffer->state) ==
	    MALI_C720_BUFFER_STATE_HW_ERROR) {
		mali_c720_ctx_mgr_buffer_request(mali_c720, temper_addr,
						 MALI_C720_BUFFER_STATE_CANCELLED);
		mali_c720_ctx_mgr_buffer_request(mali_c720, temper_addr,
						 MALI_C720_BUFFER_STATE_HW_EMPTY);
	}

	mali_c720_ctx_mgr_set_slot_enabled(ctx, true);

	/* Recovery prepares the next job; the failed frame must still be returned as an error. */
	return -EIO;
}

void mali_c720_ctx_mgr_stop_context(struct mali_c720_context *ctx)
{
	struct mali_c720 *mali_c720 = ctx->mali_c720;
	const struct mali_c720_cm_cdma_slot_regs *cdma_slot = mali_c720->registers.cm->cdma_slot;
	u32 cdma_slot_addr = mali_c720_group_base(&cdma_slot->group, ctx->id);

	lockdep_assert_held(&mali_c720->device_lock);

	scoped_guard(spinlock_irq, &ctx->state_lock)
		ctx->state = MALI_C720_SLOT_STATE_STOPPING;

	mali_c720_ctx_mgr_stop_slot(ctx);

	scoped_guard(spinlock_irq, &ctx->state_lock) {
		ctx->state = MALI_C720_SLOT_STATE_STOPPED;
		ctx->pipeline_busy = false;

		mali_c720_ctx_mgr_cdma_request(mali_c720, cdma_slot_addr,
					       MALI_C720_SLOT_CDMA_STATE_DISABLED);

		mali_c720_set_slot_interrupt(mali_c720, ctx->id, false);
		mali_c720_set_frontend_interrupts(ctx, false);

		if (--mali_c720->num_active_contexts == 0)
			mali_c720_ctx_mgr_set_scheduler_enabled(mali_c720, false);

		/* Retire in-flight buffers on all queues, including those still streaming. */
		mali_c720_ctx_mgr_buffer_done(ctx, false);

		ctx->slot_desc.mode = 0;
		ctx->slot_desc.slot_input_0 = 255;
		ctx->slot_desc.slot_input_1 = 255;
		ctx->slot_desc.pool_scaler0_rgb = 255;
		ctx->slot_desc.pool_scaler0_yuv = 255;
		ctx->slot_desc.pool_scaler1_rgb = 255;
		ctx->slot_desc.pool_scaler1_yuv = 255;
		ctx->slot_desc.pool_scaler2_rgb = 255;
		ctx->slot_desc.pool_scaler2_yuv = 255;
		ctx->slot_desc.pool_scaler3_rgb = 255;
		ctx->slot_desc.pool_scaler3_yuv = 255;
		ctx->slot_desc.pool_roi0 = 255;
		ctx->slot_desc.pool_roi1 = 255;
		ctx->slot_desc.pool_roi2 = 255;
		ctx->slot_desc.pool_raw = 255;
		ctx->slot_desc.pool_gdc_main = 255;
		ctx->slot_desc.pool_gdc_roi0 = 255;
		ctx->slot_desc.pool_gdc_roi1 = 255;
		ctx->slot_desc.pool_gdc_roi2 = 255;
		ctx->slot_desc.pool_pyramid = 255;
		ctx->slot_desc.pool_consistency = 255;
		memset(ctx->active_fifos_by_output, 0, sizeof(ctx->active_fifos_by_output));
		ctx->slot_desc_updated = false;
		mali_c720_ctx_mgr_write_slot_desc(ctx);
	}

	/* Drain queued IRQ events before the context can be restarted. */
	synchronize_irq(mali_c720->irq);
}

int mali_c720_alloc_context(struct mali_c720_context *ctx)
{
	struct mali_c720 *mali_c720 = ctx->mali_c720;
	const struct mali_c720_registers *regs = &mali_c720->registers;
	const struct mali_c720_pipeline_regs *pipeline = regs->top->pipeline;

	guard(mutex)(&ctx->regs.allocation_lock);

	if (refcount_inc_not_zero(&ctx->regs.users))
		return 0;

	ctx->regs.size = regs->context_size;
	ctx->regs.base = dma_alloc_coherent(mali_c720->dev,
					    ctx->regs.size,
					    &ctx->regs.dma_handle,
					    GFP_KERNEL);
	if (!ctx->regs.base)
		return -ENOMEM;

	memcpy(ctx->regs.base,
	       mali_c720->default_regs,
	       ctx->regs.size);

	/*
	 * Provide a default usecase for a reasonable ISP output without
	 * issuing any parameters buffers.
	 */
	mali_c720_ctx_group_write(ctx, mali_c720->top_base,
				  &pipeline->bypass_inp_frame_stitch, 1);
	mali_c720_ctx_group_write(ctx, mali_c720->top_base,
				  &pipeline->bypass_inp_decompander, 1);
	mali_c720_ctx_group_write(ctx, mali_c720->top_base,
				  &pipeline->bypass_inp_dgain, 0);
	mali_c720_ctx_group_write(ctx, mali_c720->top_base,
				  &pipeline->bypass_inp_square_root_a, 0);
	mali_c720_ctx_group_write(ctx, mali_c720->top_base,
				  &pipeline->bypass_inp_dpc, 1);
	mali_c720_ctx_group_write(ctx, mali_c720->top_base,
				  &pipeline->bypass_inp_remosaic, 1);
	mali_c720_ctx_group_write(ctx, mali_c720->top_base,
				  &pipeline->bypass_inp_cac, 1);
	mali_c720_ctx_group_write(ctx, mali_c720->top_base,
				  &pipeline->bypass_inp_temper, 1);
	mali_c720_ctx_group_write(ctx, mali_c720->top_base,
				  &pipeline->bypass_inp_sinter, 1);
	mali_c720_ctx_group_write(ctx, mali_c720->top_base,
				  &pipeline->bypass_hv_black_level, 0);
	mali_c720_ctx_group_write(ctx, mali_c720->top_base,
				  &pipeline->bypass_hv_wb_gain, 0);
	mali_c720_ctx_group_write(ctx, mali_c720->top_base,
				  &pipeline->bypass_hv_smart_shading, 1);
	mali_c720_ctx_group_write(ctx, mali_c720->top_base,
				  &pipeline->bypass_hv_raw_to_intensity, 0);
	mali_c720_ctx_group_write(ctx, mali_c720->top_base,
				  &pipeline->bypass_hv_dehaze, 1);
	mali_c720_ctx_group_write(ctx, mali_c720->top_base,
				  &pipeline->bypass_hv_dgain_iridix, 1);
	mali_c720_ctx_group_write(ctx, mali_c720->top_base,
				  &pipeline->bypass_hv_iridix, 1);
	mali_c720_ctx_group_write(ctx, mali_c720->top_base,
				  &pipeline->bypass_hv_square_a, 0);
	mali_c720_ctx_group_write(ctx, mali_c720->top_base,
				  &pipeline->bypass_hv_demosaic, 0);
	mali_c720_ctx_group_write(ctx, mali_c720->top_base,
				  &pipeline->bypass_hv_pf_correction, 1);
	mali_c720_ctx_group_write(ctx, mali_c720->top_base,
				  &pipeline->bypass_hv_square_b, 1);
	mali_c720_ctx_group_write(ctx, mali_c720->top_base,
				  &pipeline->bypass_hv_ccm, 1);
	mali_c720_ctx_group_write(ctx, mali_c720->top_base,
				  &pipeline->bypass_hv_square_root_b, 0);
	mali_c720_ctx_group_write(ctx, mali_c720->top_base,
				  &pipeline->bypass_hv_cnr, 1);
	mali_c720_ctx_group_write(ctx, mali_c720->top_base,
				  &pipeline->bypass_fr0_rgb_sharpening, 1);
	mali_c720_ctx_group_write(ctx, mali_c720->top_base,
				  &pipeline->bypass_fr0_gain_and_offset, 1);
	mali_c720_ctx_group_write(ctx, mali_c720->top_base,
				  &pipeline->bypass_fr0_gamma, 1);

	/*
	 * Bypass the FR0 crop and scaler by default. The FR0 subdevice
	 * will re-enable these as required.
	 */
	mali_c720_ctx_group_write(ctx, mali_c720->top_base,
				  &pipeline->bypass_fr0_rgb_scaler, 1);
	mali_c720_ctx_group_write(ctx, mali_c720->top_base,
				  &pipeline->bypass_fr0_crop, 1);

	refcount_set(&ctx->regs.users, 1);

	return 0;
}

void mali_c720_free_context(struct mali_c720_context *ctx)
{
	struct mali_c720 *mali_c720 = ctx->mali_c720;

	guard(mutex)(&ctx->regs.allocation_lock);

	if (!refcount_dec_and_test(&ctx->regs.users))
		return;

	dma_free_coherent(mali_c720->dev, ctx->regs.size, ctx->regs.base,
			  ctx->regs.dma_handle);
	ctx->regs.base = NULL;
	ctx->regs.dma_handle = 0;
}

int mali_c720_init_contexts(struct mali_c720 *mali_c720)
{
	const struct mali_c720_registers *regs = &mali_c720->registers;
	struct mali_c720_context *ctx;

	/* Allocate a region to store ISP default registers.
	 * These will be copied into each context upon stream start.
	 */

	mali_c720->default_regs = kvzalloc(regs->context_size, GFP_KERNEL);
	if (!mali_c720->default_regs)
		return -ENOMEM;

	memcpy_fromio(mali_c720->default_regs,
		      mali_c720->base,
		      regs->context_size);

	ctx = &mali_c720->context;
	ctx->mali_c720 = mali_c720;
	ctx->id = 0;
	ctx->state = MALI_C720_SLOT_STATE_STOPPED;

	spin_lock_init(&ctx->state_lock);
	spin_lock_init(&ctx->req_lock);
	mutex_init(&ctx->regs.allocation_lock);
	ctx->request_sequence = 0;
	refcount_set(&ctx->regs.users, 0);

	return 0;
}

static void mali_c720_ctx_mgr_init_reuse_buffer(struct mali_c720 *mali_c720)
{
	const struct mali_c720_cm_buffer_regs *buffer = mali_c720->registers.cm->buffer;
	struct mali_c720_ctx_mgr *ctx_mgr = &mali_c720->ctx_mgr;
	u32 buffer_addr;

	ctx_mgr->reuse_buffer = ctx_mgr->num_raw_buffer_descriptors - 1;
	__set_bit(ctx_mgr->reuse_buffer, ctx_mgr->raw_buffer_descriptors);

	/*
	 * Keep a standalone, zero-sized descriptor for input cleanup.
	 * This is required for the workaround for a hardware bug on
	 * r0p0 and r0p1 versions.
	 */
	buffer_addr = mali_c720_group_base(&buffer->raw_group, ctx_mgr->reuse_buffer);
	mali_c720_ctx_mgr_buffer_request(mali_c720, buffer_addr,
					 MALI_C720_BUFFER_STATE_SW_EMPTY);
	mali_c720_group_write(mali_c720, buffer_addr,
			      &buffer->nonsecure, 1);
	mali_c720_group_write(mali_c720, buffer_addr,
			      &buffer->stream_id, 0);
	mali_c720_group_write(mali_c720, buffer_addr,
			      &buffer->crc_check, 0);
	mali_c720_group_write(mali_c720, buffer_addr,
			      &buffer->pool, 0);
	mali_c720_group_write(mali_c720, buffer_addr,
			      &buffer->mode, MALI_C720_RAW_BUFFER_MODE_NORMAL);
	mali_c720_group_write(mali_c720, buffer_addr,
			      &buffer->width, 0);
	mali_c720_group_write(mali_c720, buffer_addr,
			      &buffer->height, 0);
	mali_c720_ctx_mgr_buffer_request(mali_c720, buffer_addr,
					 MALI_C720_BUFFER_STATE_UNINITIALIZED);
}

int mali_c720_init_ctx_mgr(struct mali_c720 *mali_c720)
{
	const struct mali_c720_cm_capabilities *caps = mali_c720->registers.cm->caps;
	struct mali_c720_ctx_mgr *ctx_mgr = &mali_c720->ctx_mgr;
	unsigned int num_slots;

	ctx_mgr->num_raw_buffer_descriptors = caps->num_raw_buffers;
	ctx_mgr->num_out_buffer_descriptors = caps->num_out_buffers;
	ctx_mgr->num_inputs = caps->num_inputs;
	ctx_mgr->num_input_streams = caps->num_input_streams;
	ctx_mgr->num_virtual_inputs =
		ctx_mgr->num_inputs * ctx_mgr->num_input_streams;
	ctx_mgr->input_fifo_size = caps->input_fifo_size;
	ctx_mgr->output_fifo_size = caps->output_fifo_size;

	if (!ctx_mgr->num_raw_buffer_descriptors) {
		dev_err(mali_c720->dev, "No raw buffer descriptors available\n");
		return -EINVAL;
	}

	if (ctx_mgr->num_raw_buffer_descriptors > MALI_C720_MAX_RAW_BUFFERS) {
		dev_dbg(mali_c720->dev, "RAW buffer count %u exceeds driver limit, truncating\n",
			ctx_mgr->num_raw_buffer_descriptors);
		ctx_mgr->num_raw_buffer_descriptors = MALI_C720_MAX_RAW_BUFFERS;
	}

	if (!ctx_mgr->num_out_buffer_descriptors) {
		dev_err(mali_c720->dev, "No output buffer descriptors available\n");
		return -EINVAL;
	}

	if (ctx_mgr->num_out_buffer_descriptors > MALI_C720_MAX_OUT_BUFFERS) {
		dev_dbg(mali_c720->dev, "Output buffer count %u exceeds driver limit, truncating\n",
			ctx_mgr->num_out_buffer_descriptors);
		ctx_mgr->num_out_buffer_descriptors = MALI_C720_MAX_OUT_BUFFERS;
	}

	if (!ctx_mgr->num_inputs) {
		dev_err(mali_c720->dev, "No input ports available\n");
		return -EINVAL;
	}

	if (!ctx_mgr->num_input_streams) {
		dev_err(mali_c720->dev, "No input streams available\n");
		return -EINVAL;
	}

	if (ctx_mgr->num_inputs > MALI_C720_MAX_INPUTS ||
	    ctx_mgr->num_input_streams > MALI_C720_MAX_STREAMS_PER_INPUT ||
	    ctx_mgr->num_virtual_inputs > MALI_C720_MAX_VIRTUAL_INPUTS) {
		dev_err(mali_c720->dev,
			"Input topology exceeds driver limits: %u inputs, %u streams per input\n",
			ctx_mgr->num_inputs, ctx_mgr->num_input_streams);
		return -EINVAL;
	}

	num_slots = caps->num_slots;
	if (num_slots <= MALI_C720_NUM_SAFETY_SLOTS) {
		dev_err(mali_c720->dev,
			"No non-safety slots available: %u slots, %u reserved\n",
			num_slots, MALI_C720_NUM_SAFETY_SLOTS);
		return -EINVAL;
	}

	/* Safety slots are not available for video contexts. */
	ctx_mgr->num_contexts = num_slots - MALI_C720_NUM_SAFETY_SLOTS;
	if (ctx_mgr->num_contexts > MALI_C720_MAX_CONTEXTS) {
		dev_err(mali_c720->dev,
			"Non-safety slot count %u exceeds driver limit %u\n",
			ctx_mgr->num_contexts, MALI_C720_MAX_CONTEXTS);
		return -EINVAL;
	}

	bitmap_zero(ctx_mgr->raw_buffer_descriptors, ctx_mgr->num_raw_buffer_descriptors);
	bitmap_zero(ctx_mgr->out_buffer_descriptors, ctx_mgr->num_out_buffer_descriptors);

	mali_c720_ctx_mgr_init_reuse_buffer(mali_c720);

	spin_lock_init(&ctx_mgr->input_lock);
	memset(ctx_mgr->input_context_map, 0xff, sizeof(ctx_mgr->input_context_map));

	return 0;
}
