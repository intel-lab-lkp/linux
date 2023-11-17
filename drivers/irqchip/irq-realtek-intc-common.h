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
 * realtek_intc_subset_cfg - subset interrupt mask
 * @ints_mask: inetrrupt mask
 */
struct realtek_intc_subset_cfg {
	unsigned int	ints_mask;
};

/**
 * realtek_intc_info - interrupt controller data.
 * @isr_offset: interrupt status register offset.
 * @umsk_isr_offset: unmask interrupt status register offset.
 * @scpu_int_en_offset: interrupt enable register offset.
 * @cfg: cfg of the subset.
 * @cfg_num: number of cfg.
 */
struct realtek_intc_info {
	const struct realtek_intc_subset_cfg *cfg;
	unsigned int			     isr_offset;
	unsigned int			     umsk_isr_offset;
	unsigned int			     scpu_int_en_offset;
	const u32			     *isr_to_scpu_int_en_mask;
	int				     cfg_num;
};

/**
 * realtek_intc_subset_data - handler of a interrupt source only handles ints
 *                            bits in the mask.
 * @cfg: cfg of the subset.
 * @common: common data.
 * @parent_irq: interrupt source.
 */
struct realtek_intc_subset_data {
	const struct realtek_intc_subset_cfg *cfg;
	struct realtek_intc_data	     *common;
	int				     parent_irq;
};

/**
 * realtek_intc_data - configuration data for realtek interrupt controller driver.
 * @base: base of interrupt register
 * @info: info of intc
 * @domain: interrupt domain
 * @lock: lock
 * @saved_en: status of interrupt enable
 * @subset_data_num: number of subset data
 * @subset_data: subset data
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

#define IRQ_ALWAYS_ENABLED U32_MAX
#define DISABLE_INTC (0)
#define CLEAN_INTC_STATUS GENMASK(31, 1)

int realtek_intc_probe(struct platform_device *pdev, const struct realtek_intc_info *info);

#endif /* _IRQ_REALTEK_COMMON_H */
