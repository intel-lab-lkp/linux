/* SPDX-License-Identifier: (GPL-2.0-or-later OR BSD-2-Clause) */
/*
 * Copyright (c) 2023 Realtek Semiconductor Corporation
 */

#ifndef _IRQ_REALTEK_COMMON_H
#define _IRQ_REALTEK_COMMON_H

#include <linux/bits.h>
#include <linux/limits.h>
#include <linux/hwspinlock.h>

/**
 * realtek_intc_mask - The mask of an interrupt subset.
 * @ints_mask: The interrupt mask.
 */
struct realtek_intc_mask {
	u32	ints_mask;
};

/**
 * realtek_intc_info - Information about the interrupt controller.
 * @subset_mask:	The masks of the interrupt subsets.
 * @subset_num:		The number of interrupt subsets.
 * @isr_offset:		The offset of the interrupt status register.
 * @scpu_int_en_offset:	The offset of the interrupt enable register.
 */
struct realtek_intc_info {
	const struct realtek_intc_mask	*subset_mask;
	int				subset_num;
	unsigned int			isr_offset;
	unsigned int			scpu_int_en_offset;
	const u32			*isr_to_scpu_int_en_mask;
};

/**
 * realtek_intc_subset_data - The data of interrupt subset.
 * @subset_mask:	The subset interrupt masks.
 * @common:		The configuration data of interrupt controller.
 * @parent_irq:		The subset interrupt source.
 */
struct realtek_intc_subset_data {
	const struct realtek_intc_mask	*subset_mask;
	struct realtek_intc_data	*common;
	int				parent_irq;
};

/**
 * realtek_intc_data - The configuration data for interrupt controller driver.
 * @base:		The base address of interrupt register.
 * @info:		The Information of the interrupt controller.
 * @domain:		Interrupt domain of the interrupt controller.
 * @lock:		The lock of the interrupt controller.
 * @saved_en:		Stores the state of the interrupt enable.
 * @subset_data_num:	The number of entries in the interrupt subset data.
 * @subset_data:	The data for the interrupt subset.
 */
struct realtek_intc_data {
	void __iomem			*base;
	const struct realtek_intc_info	*info;
	struct irq_domain		*domain;
	struct raw_spinlock		lock;
	unsigned int			saved_en;
	int				subset_data_num;
	struct realtek_intc_subset_data subset_data[];
};

#define IRQ_ALWAYS_ENABLED	U32_MAX
#define DISABLE_INTC		(0)
#define CLEAN_INTC_STATUS	GENMASK(31, 1)

int realtek_intc_probe(struct platform_device *pdev, const struct realtek_intc_info *info);
int realtek_intc_suspend(struct device *dev);
int realtek_intc_resume(struct device *dev);

#endif /* _IRQ_REALTEK_COMMON_H */
