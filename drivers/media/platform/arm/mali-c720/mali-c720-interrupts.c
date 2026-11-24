// SPDX-License-Identifier: GPL-2.0-only
/*
 * ARM Mali-C720 ISP Driver - Image signal processor
 *
 * Copyright (C) 2026 Arm Ltd
 */

#include <linux/cleanup.h>
#include <linux/device.h>
#include <linux/timekeeping.h>

#include "mali-c720-common.h"

static void mali_c720_isr_set_frontend_bits(struct mali_c720 *mali_c720,
					    struct mali_c720_interrupt_data *irq_data)
{
	const struct mali_c720_irq_regs *irq = mali_c720->registers.top->irq;
	const struct mali_c720_reg_field * const
		fields[MALI_C720_MAX_INPUTS][MALI_C720_MAX_STREAMS_PER_INPUT] = {
		{
			&irq->cm_input_0_stream_0_cmsi_active_video,
			&irq->cm_input_0_stream_1_cmsi_active_video,
			&irq->cm_input_0_stream_2_cmsi_active_video,
			&irq->cm_input_0_stream_3_cmsi_active_video,
		},
		{
			&irq->cm_input_1_stream_0_cmsi_active_video,
			&irq->cm_input_1_stream_1_cmsi_active_video,
			&irq->cm_input_1_stream_2_cmsi_active_video,
			&irq->cm_input_1_stream_3_cmsi_active_video,
		},
		{
			&irq->cm_input_2_stream_0_cmsi_active_video,
			&irq->cm_input_2_stream_1_cmsi_active_video,
			&irq->cm_input_2_stream_2_cmsi_active_video,
			&irq->cm_input_2_stream_3_cmsi_active_video,
		},
		{
			&irq->cm_input_3_stream_0_cmsi_active_video,
			&irq->cm_input_3_stream_1_cmsi_active_video,
			&irq->cm_input_3_stream_2_cmsi_active_video,
			&irq->cm_input_3_stream_3_cmsi_active_video,
		},
	};
	unsigned int num_streams = mali_c720->ctx_mgr.num_input_streams;
	unsigned int virtual_input_id;

	/* Virtual input ID = physical input * num_streams + stream. */
	for (virtual_input_id = 0; virtual_input_id < mali_c720->ctx_mgr.num_virtual_inputs;
	     virtual_input_id++) {
		unsigned int input = virtual_input_id / num_streams;
		unsigned int stream = virtual_input_id % num_streams;
		const struct mali_c720_reg_field *field = fields[input][stream];

		if (!mali_c720_irq_read(mali_c720, field, MALI_C720_IRQ_STATUS))
			continue;

		set_bit(virtual_input_id, &irq_data->input_frame_start);
		mali_c720_irq_write(mali_c720, field, MALI_C720_IRQ_RAWSTAT, 1);
	}
}

static void mali_c720_isr_set_backend_bits(struct mali_c720 *mali_c720,
					   struct mali_c720_interrupt_data *irq_data)
{
	const struct mali_c720_irq_regs *irq = mali_c720->registers.top->irq;
	const struct mali_c720_reg_field * const fields[MALI_C720_MAX_CONTEXTS] = {
		&irq->cm_status_slot_0,
		&irq->cm_status_slot_1,
		&irq->cm_status_slot_2,
		&irq->cm_status_slot_3,
		&irq->cm_status_slot_4,
		&irq->cm_status_slot_5,
		&irq->cm_status_slot_6,
		&irq->cm_status_slot_7,
		&irq->cm_status_slot_8,
		&irq->cm_status_slot_9,
		&irq->cm_status_slot_10,
		&irq->cm_status_slot_11,
		&irq->cm_status_slot_12,
		&irq->cm_status_slot_13,
		&irq->cm_status_slot_14,
		&irq->cm_status_slot_15,
	};
	unsigned int slot;

	for (slot = 0; slot < mali_c720->ctx_mgr.num_contexts; slot++) {
		if (!mali_c720_irq_read(mali_c720, fields[slot], MALI_C720_IRQ_STATUS))
			continue;

		set_bit(slot, &irq_data->frame_end);
		mali_c720_irq_write(mali_c720, fields[slot], MALI_C720_IRQ_RAWSTAT, 1);
	}
}

irqreturn_t mali_c720_isr_top(int irq, void *dev_id)
{
	struct device *dev = dev_id;
	struct mali_c720 *mali_c720 = dev_get_drvdata(dev);
	struct mali_c720_interrupt_data irq_data = {
		.timestamp = ktime_get_ns(),
	};

	mali_c720_isr_set_frontend_bits(mali_c720, &irq_data);

	/* Preserve all slots which completed before the interrupt was serviced. */
	mali_c720_isr_set_backend_bits(mali_c720, &irq_data);

	if (!irq_data.input_frame_start && !irq_data.frame_end)
		return IRQ_NONE;

	if (!kfifo_in_spinlocked(&mali_c720->irq_fifo, &irq_data, 1,
				 &mali_c720->irq_lock))
		dev_err_ratelimited(mali_c720->dev, "IRQ FIFO overflow\n");

	return IRQ_WAKE_THREAD;
}

static void mali_c720_handle_input_frame_start(struct mali_c720 *mali_c720,
					       unsigned int virtual_input_id,
					       u64 timestamp)
{
	struct mali_c720_context *ctx = mali_c720_get_active_context(mali_c720);
	const struct mali_c720_input_cfg *input_cfg = &ctx->input_cfg;
	struct mali_c720_ctx_mgr *ctx_mgr = &mali_c720->ctx_mgr;
	struct mali_c720_frame_timestamp *entry;
	unsigned int ctx_id;
	u32 sequence;

	guard(spinlock)(&ctx_mgr->input_lock);

	ctx_id = ctx_mgr->input_context_map[virtual_input_id];
	if (ctx_id == 0xFF)
		return;

	/* Only context 0 is currently supported. */
	if (WARN_ON_ONCE(ctx_id != ctx->id))
		return;

	guard(spinlock)(&ctx->state_lock);

	if (ctx->state == MALI_C720_SLOT_STATE_STOPPING ||
	    ctx->state == MALI_C720_SLOT_STATE_STOPPED)
		return;

	if (input_cfg->id != virtual_input_id) {
		dev_err(mali_c720->dev,
			"Received unexpected frame start for virtual input %u, expected %u\n",
			virtual_input_id, input_cfg->id);
		return;
	}

	sequence = ctx->input_frame_sequence++;

	entry = &ctx->frame_timestamps[sequence % MALI_C720_FRAME_TIMESTAMP_COUNT];
	entry->sequence = sequence;
	entry->timestamp = timestamp;
	entry->valid = true;

	mali_c720_isp_queue_event_sof(mali_c720, sequence);
}

static void mali_c720_update_frame_timestamp(struct mali_c720_context *ctx)
{
	const struct mali_c720_frame_timestamp *entry =
		&ctx->frame_timestamps[ctx->frame_sequence % MALI_C720_FRAME_TIMESTAMP_COUNT];

	if (entry->valid && entry->sequence == ctx->frame_sequence) {
		ctx->timestamp = entry->timestamp;
	} else {
		dev_warn_ratelimited(ctx->mali_c720->dev,
				     "Missing frontend timestamp for frame %u\n",
				     ctx->frame_sequence);
	}
}

static void mali_c720_handle_frame_end(struct mali_c720 *mali_c720,
				       unsigned int ctx_id)
{
	const struct mali_c720_pipeline_regs *pipeline = mali_c720->registers.top->pipeline;
	struct mali_c720_context *ctx = mali_c720_get_active_context(mali_c720);
	int ret;

	/* Only context 0 is currently supported. */
	if (WARN_ON_ONCE(ctx_id != 0))
		return;

	scoped_guard(spinlock, &ctx->state_lock) {
		if (ctx->state != MALI_C720_SLOT_STATE_STARTED)
			return;

		ret = mali_c720_ctx_mgr_check_context_status(ctx);
		if (!ret) {
			ctx->frame_sequence =
				mali_c720_ctx_group_read(ctx, mali_c720->top_base,
							 &pipeline->frame_count) - 1;

			mali_c720_update_frame_timestamp(ctx);
		}

		mali_c720_ctx_mgr_buffer_done(ctx, !ret);

		ctx->pipeline_busy = false;
	}

	mali_c720_try_schedule_frame(ctx);
}

irqreturn_t mali_c720_isr(int irq, void *dev_id)
{
	struct device *dev = dev_id;
	struct mali_c720 *mali_c720 = dev_get_drvdata(dev);
	struct mali_c720_interrupt_data irq_data;
	unsigned int bit;

	while (kfifo_out_spinlocked(&mali_c720->irq_fifo, &irq_data, 1,
				    &mali_c720->irq_lock)) {
		for_each_set_bit(bit, &irq_data.input_frame_start,
				 mali_c720->ctx_mgr.num_virtual_inputs)
			mali_c720_handle_input_frame_start(mali_c720, bit, irq_data.timestamp);

		for_each_set_bit(bit, &irq_data.frame_end, MALI_C720_MAX_CONTEXTS)
			mali_c720_handle_frame_end(mali_c720, bit);
	}

	return IRQ_HANDLED;
}

static void mali_c720_set_input_interrupt(struct mali_c720 *mali_c720,
					  unsigned int virtual_input_id, bool enable)
{
	const struct mali_c720_irq_regs *irq = mali_c720->registers.top->irq;
	unsigned int input = virtual_input_id / mali_c720->ctx_mgr.num_input_streams;
	unsigned int stream = virtual_input_id % mali_c720->ctx_mgr.num_input_streams;

	/*
	 * When enabling an interrupt, we should clear any pending interrupts
	 * before enabling the mask.
	 * When disabling an interrupt, we should disable the mask, then
	 * clear any pending interrupts.
	 * Interrupts are cleared by writing a 1 to the RAWSTAT bit.
	 */
	enum mali_c720_irq_type type0 = enable ? MALI_C720_IRQ_RAWSTAT : MALI_C720_IRQ_MASK;
	enum mali_c720_irq_type type1 = enable ? MALI_C720_IRQ_MASK : MALI_C720_IRQ_RAWSTAT;

	switch (input * MALI_C720_MAX_STREAMS_PER_INPUT + stream) {
	case 0:
		mali_c720_irq_write(mali_c720, &irq->cm_input_0_stream_0_cmsi_active_video, type0,
				    enable);
		mali_c720_irq_write(mali_c720, &irq->cm_input_0_stream_0_cmsi_active_video, type1,
				    1);
		break;
	case 1:
		mali_c720_irq_write(mali_c720, &irq->cm_input_0_stream_1_cmsi_active_video, type0,
				    enable);
		mali_c720_irq_write(mali_c720, &irq->cm_input_0_stream_1_cmsi_active_video, type1,
				    1);
		break;
	case 2:
		mali_c720_irq_write(mali_c720, &irq->cm_input_0_stream_2_cmsi_active_video, type0,
				    enable);
		mali_c720_irq_write(mali_c720, &irq->cm_input_0_stream_2_cmsi_active_video, type1,
				    1);
		break;
	case 3:
		mali_c720_irq_write(mali_c720, &irq->cm_input_0_stream_3_cmsi_active_video, type0,
				    enable);
		mali_c720_irq_write(mali_c720, &irq->cm_input_0_stream_3_cmsi_active_video, type1,
				    1);
		break;
	case 4:
		mali_c720_irq_write(mali_c720, &irq->cm_input_1_stream_0_cmsi_active_video, type0,
				    enable);
		mali_c720_irq_write(mali_c720, &irq->cm_input_1_stream_0_cmsi_active_video, type1,
				    1);
		break;
	case 5:
		mali_c720_irq_write(mali_c720, &irq->cm_input_1_stream_1_cmsi_active_video, type0,
				    enable);
		mali_c720_irq_write(mali_c720, &irq->cm_input_1_stream_1_cmsi_active_video, type1,
				    1);
		break;
	case 6:
		mali_c720_irq_write(mali_c720, &irq->cm_input_1_stream_2_cmsi_active_video, type0,
				    enable);
		mali_c720_irq_write(mali_c720, &irq->cm_input_1_stream_2_cmsi_active_video, type1,
				    1);
		break;
	case 7:
		mali_c720_irq_write(mali_c720, &irq->cm_input_1_stream_3_cmsi_active_video, type0,
				    enable);
		mali_c720_irq_write(mali_c720, &irq->cm_input_1_stream_3_cmsi_active_video, type1,
				    1);
		break;
	case 8:
		mali_c720_irq_write(mali_c720, &irq->cm_input_2_stream_0_cmsi_active_video, type0,
				    enable);
		mali_c720_irq_write(mali_c720, &irq->cm_input_2_stream_0_cmsi_active_video, type1,
				    1);
		break;
	case 9:
		mali_c720_irq_write(mali_c720, &irq->cm_input_2_stream_1_cmsi_active_video, type0,
				    enable);
		mali_c720_irq_write(mali_c720, &irq->cm_input_2_stream_1_cmsi_active_video, type1,
				    1);
		break;
	case 10:
		mali_c720_irq_write(mali_c720, &irq->cm_input_2_stream_2_cmsi_active_video, type0,
				    enable);
		mali_c720_irq_write(mali_c720, &irq->cm_input_2_stream_2_cmsi_active_video, type1,
				    1);
		break;
	case 11:
		mali_c720_irq_write(mali_c720, &irq->cm_input_2_stream_3_cmsi_active_video, type0,
				    enable);
		mali_c720_irq_write(mali_c720, &irq->cm_input_2_stream_3_cmsi_active_video, type1,
				    1);
		break;
	case 12:
		mali_c720_irq_write(mali_c720, &irq->cm_input_3_stream_0_cmsi_active_video, type0,
				    enable);
		mali_c720_irq_write(mali_c720, &irq->cm_input_3_stream_0_cmsi_active_video, type1,
				    1);
		break;
	case 13:
		mali_c720_irq_write(mali_c720, &irq->cm_input_3_stream_1_cmsi_active_video, type0,
				    enable);
		mali_c720_irq_write(mali_c720, &irq->cm_input_3_stream_1_cmsi_active_video, type1,
				    1);
		break;
	case 14:
		mali_c720_irq_write(mali_c720, &irq->cm_input_3_stream_2_cmsi_active_video, type0,
				    enable);
		mali_c720_irq_write(mali_c720, &irq->cm_input_3_stream_2_cmsi_active_video, type1,
				    1);
		break;
	case 15:
		mali_c720_irq_write(mali_c720, &irq->cm_input_3_stream_3_cmsi_active_video, type0,
				    enable);
		mali_c720_irq_write(mali_c720, &irq->cm_input_3_stream_3_cmsi_active_video, type1,
				    1);
		break;
	default:
		dev_warn_once(mali_c720->dev,
			      "Invalid virtual input interrupt id %u\n",
			      virtual_input_id);
		return;
	}
}

void mali_c720_set_slot_interrupt(struct mali_c720 *mali_c720,
				  unsigned int slot_id, bool enable)
{
	const struct mali_c720_irq_regs *irq = mali_c720->registers.top->irq;
	enum mali_c720_irq_type type0 = enable ? MALI_C720_IRQ_RAWSTAT : MALI_C720_IRQ_MASK;
	enum mali_c720_irq_type type1 = enable ? MALI_C720_IRQ_MASK : MALI_C720_IRQ_RAWSTAT;

	switch (slot_id) {
	case 0:
		mali_c720_irq_write(mali_c720, &irq->cm_status_slot_0, type0, enable);
		mali_c720_irq_write(mali_c720, &irq->cm_status_slot_0, type1, 1);
		break;
	case 1:
		mali_c720_irq_write(mali_c720, &irq->cm_status_slot_1, type0, enable);
		mali_c720_irq_write(mali_c720, &irq->cm_status_slot_1, type1, 1);
		break;
	case 2:
		mali_c720_irq_write(mali_c720, &irq->cm_status_slot_2, type0, enable);
		mali_c720_irq_write(mali_c720, &irq->cm_status_slot_2, type1, 1);
		break;
	case 3:
		mali_c720_irq_write(mali_c720, &irq->cm_status_slot_3, type0, enable);
		mali_c720_irq_write(mali_c720, &irq->cm_status_slot_3, type1, 1);
		break;
	case 4:
		mali_c720_irq_write(mali_c720, &irq->cm_status_slot_4, type0, enable);
		mali_c720_irq_write(mali_c720, &irq->cm_status_slot_4, type1, 1);
		break;
	case 5:
		mali_c720_irq_write(mali_c720, &irq->cm_status_slot_5, type0, enable);
		mali_c720_irq_write(mali_c720, &irq->cm_status_slot_5, type1, 1);
		break;
	case 6:
		mali_c720_irq_write(mali_c720, &irq->cm_status_slot_6, type0, enable);
		mali_c720_irq_write(mali_c720, &irq->cm_status_slot_6, type1, 1);
		break;
	case 7:
		mali_c720_irq_write(mali_c720, &irq->cm_status_slot_7, type0, enable);
		mali_c720_irq_write(mali_c720, &irq->cm_status_slot_7, type1, 1);
		break;
	case 8:
		mali_c720_irq_write(mali_c720, &irq->cm_status_slot_8, type0, enable);
		mali_c720_irq_write(mali_c720, &irq->cm_status_slot_8, type1, 1);
		break;
	case 9:
		mali_c720_irq_write(mali_c720, &irq->cm_status_slot_9, type0, enable);
		mali_c720_irq_write(mali_c720, &irq->cm_status_slot_9, type1, 1);
		break;
	case 10:
		mali_c720_irq_write(mali_c720, &irq->cm_status_slot_10, type0, enable);
		mali_c720_irq_write(mali_c720, &irq->cm_status_slot_10, type1, 1);
		break;
	case 11:
		mali_c720_irq_write(mali_c720, &irq->cm_status_slot_11, type0, enable);
		mali_c720_irq_write(mali_c720, &irq->cm_status_slot_11, type1, 1);
		break;
	case 12:
		mali_c720_irq_write(mali_c720, &irq->cm_status_slot_12, type0, enable);
		mali_c720_irq_write(mali_c720, &irq->cm_status_slot_12, type1, 1);
		break;
	case 13:
		mali_c720_irq_write(mali_c720, &irq->cm_status_slot_13, type0, enable);
		mali_c720_irq_write(mali_c720, &irq->cm_status_slot_13, type1, 1);
		break;
	case 14:
		mali_c720_irq_write(mali_c720, &irq->cm_status_slot_14, type0, enable);
		mali_c720_irq_write(mali_c720, &irq->cm_status_slot_14, type1, 1);
		break;
	case 15:
		mali_c720_irq_write(mali_c720, &irq->cm_status_slot_15, type0, enable);
		mali_c720_irq_write(mali_c720, &irq->cm_status_slot_15, type1, 1);
		break;
	default:
		dev_warn_once(mali_c720->dev,
			      "Invalid slot interrupt id %u\n", slot_id);
		return;
	}
}

void mali_c720_set_frontend_interrupts(struct mali_c720_context *ctx, bool enable)
{
	const struct mali_c720_input_cfg *input_cfg = &ctx->input_cfg;

	mali_c720_set_input_interrupt(ctx->mali_c720, input_cfg->id,
				      enable);
}
