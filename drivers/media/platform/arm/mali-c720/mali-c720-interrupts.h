/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * ARM Mali-C720 ISP Driver - Image signal processor
 *
 * Copyright (C) 2026 Arm Ltd
 */

#ifndef MALI_C720_INTERRUPTS_H
#define MALI_C720_INTERRUPTS_H

#include <linux/interrupt.h>
#include <linux/types.h>

struct mali_c720;
struct mali_c720_context;

#define MALI_C720_IRQ_FIFO_SIZE		256

struct mali_c720_interrupt_data {
	u64 timestamp;

	unsigned long input_frame_start;

	unsigned long frame_end;
};

irqreturn_t mali_c720_isr_top(int irq, void *dev_id);
irqreturn_t mali_c720_isr(int irq, void *dev_id);

void mali_c720_set_slot_interrupt(struct mali_c720 *mali_c720,
				  unsigned int slot_id, bool enable);
void mali_c720_set_frontend_interrupts(struct mali_c720_context *ctx, bool enable);

#endif /* MALI_C720_INTERRUPTS_H */
