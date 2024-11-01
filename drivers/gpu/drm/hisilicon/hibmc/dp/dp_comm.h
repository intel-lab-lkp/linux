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

#define dp_reg_read_field(addr, mask)				\
		FIELD_GET(mask, readl(addr))

#define dp_field_modify(reg_value, mask, value) ({		\
		(reg_value) &= ~(mask);				\
		(reg_value) |= FIELD_PREP(mask, value); })

#define dp_reg_write_field(addr, mask, val) ({			\
		typeof(addr) _addr = (addr);			\
		u32 _value = readl(_addr);			\
		dp_field_modify(_value, mask, val);		\
		writel(_value, _addr); })

struct link_status {
	bool clock_recovered;
	bool channel_equalized;
	u8 cr_done_lanes;
};

struct link_cap {
	int rx_dpcd_revision;
	u8 link_rate;
	u8 lanes;
	bool is_tps3;
	bool is_tps4;
};

struct hibmc_dp_link {
	struct link_status status;
	u8 *train_set;
	struct link_cap cap;
};

struct dp_dev {
	struct hibmc_dp_link link;
	struct drm_dp_aux aux;
	struct drm_device *dev;
	void __iomem *base;
	u8 dpcd[DP_RECEIVER_CAP_SIZE];
};

#endif
