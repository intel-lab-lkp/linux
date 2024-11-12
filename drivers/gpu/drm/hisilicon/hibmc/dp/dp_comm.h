/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (c) 2024 Hisilicon Limited. */

#ifndef DP_COMM_H
#define DP_COMM_H

#include <linux/types.h>
#include <linux/bitops.h>
#include <linux/errno.h>
#include <linux/mutex.h>
#include <linux/kernel.h>
#include <linux/bitfield.h>
#include <linux/io.h>
#include <drm/display/drm_dp_helper.h>

struct hibmc_dp_dev {
	struct drm_dp_aux aux;
	struct drm_device *dev;
	void __iomem *base;
	struct mutex lock; /* protects concurrent RW in hibmc_dp_reg_write_field() */
};

static inline void hibmc_dp_reg_write_field(struct hibmc_dp_dev *dp, u32 offset, u32 mask, u32 val)
{
	u32 value;

	mutex_lock(&dp->lock);

	value = readl(dp->base + offset);
	value &= ~mask;
	value |= FIELD_PREP(mask, val);
	writel(value, dp->base + offset);

	mutex_unlock(&dp->lock);
}

void hibmc_dp_aux_init(struct hibmc_dp_dev *dp);

#endif
