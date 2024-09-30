/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (c) 2024 Hisilicon Limited. */

#ifndef DP_COMM_H
#define DP_COMM_H

#include <linux/types.h>
#include <linux/bitops.h>
#include <linux/errno.h>
#include <linux/mutex.h>
#include <linux/kernel.h>
#include <linux/io.h>

#define REG_LENGTH 32

static inline u32 dp_read_bits(void __iomem *addr, u32 bit_mask)
{
	u32 reg_val;

	reg_val = readl(addr);

	return (reg_val & bit_mask) >> __ffs(bit_mask);
}

static inline void dp_write_bits(void __iomem *addr, u32 bit_mask, u32 val)
{
	u32 reg_val;

	reg_val = readl(addr);
	reg_val &= ~bit_mask;
	reg_val |= (val << __ffs(bit_mask)) & bit_mask;
	writel(reg_val, addr);
}

enum dpcd_revision {
	DPCD_REVISION_10 = 0x10,
	DPCD_REVISION_11,
	DPCD_REVISION_12,
	DPCD_REVISION_13,
	DPCD_REVISION_14,
};

struct hibmc_dp_aux_msg {
	u32 address;
	u8 request;
	u8 *buf;
	u8 size;
	u8 reply;
};

struct hibmc_dp_aux {
	struct mutex lock; /* aux transfer lock */
	void __iomem *addr;
};

struct link_status {
	bool clock_recovered;
	bool channel_equalized;
	u8 cr_done_lanes;
};

struct link_cap {
	enum dpcd_revision rx_dpcd_revision;
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

struct hibmc_dp_dev {
	struct hibmc_dp_link link;
	struct hibmc_dp_aux aux;
	struct drm_device *dev;
	void __iomem *base;
};

static inline struct hibmc_dp_dev *to_dp_dev_s(struct hibmc_dp_aux *aux)
{
	return container_of(aux, struct hibmc_dp_dev, aux);
}

#endif
