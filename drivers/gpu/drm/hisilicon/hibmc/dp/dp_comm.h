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

#define HIBMC_DP_LANE_NUM_MAX 2

struct hibmc_link_status {
	bool clock_recovered;
	bool channel_equalized;
};

struct hibmc_link_cap {
	int rx_dpcd_revision;
	u8 link_rate;
	u8 lanes;
	bool is_tps3;
	bool is_tps4;
};

struct hibmc_dp_link {
	struct hibmc_link_status status;
	u8 train_set[HIBMC_DP_LANE_NUM_MAX];
	struct hibmc_link_cap cap;
};

struct hibmc_dp_dev {
	struct drm_dp_aux aux;
	struct drm_device *dev;
	void __iomem *base;
	struct mutex lock; /* protects concurrent RW in hibmc_dp_reg_write_field() */
	struct hibmc_dp_link link;
	u8 dpcd[DP_RECEIVER_CAP_SIZE];
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
int hibmc_dp_link_training(struct hibmc_dp_dev *dp);

#endif
