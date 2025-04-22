/* SPDX-License-Identifier: (GPL-2.0 OR BSD-3-Clause) */
/*
 * Wave6 series multi-standard codec IP - wave6 driver
 *
 * Copyright (C) 2025 CHIPS&MEDIA INC
 */

#ifndef __WAVE6_VPU_H__
#define __WAVE6_VPU_H__

#include <linux/device.h>

#define WAVE6_VPU_MAXIMUM_ENTITY_CNT	4

#define call_vop(vpu, op, args...)					\
	((vpu)->ops->op ? (vpu)->ops->op(vpu, ##args) : 0)		\

#define call_void_vop(vpu, op, args...)					\
	do {								\
		if ((vpu)->ops->op)					\
			(vpu)->ops->op(vpu, ##args);			\
	} while (0)

struct vpu_buf {
	size_t size;
	dma_addr_t daddr;
	void *vaddr;
	struct device *dev;
};

struct wave6_vpu_entity {
	struct list_head list;
	struct device *dev;
	struct device *vpu;
	u32 (*read_reg)(struct device *dev, u32 addr);
	void (*write_reg)(struct device *dev, u32 addr, u32 data);
	void (*on_boot)(struct device *dev);
	void (*pause)(struct device *dev, int resume);
	bool active;
	int index;
};

struct wave6_vpu_ctrl_ops {
	int (*get_ctrl)(struct device *ctrl, struct wave6_vpu_entity *entity);
	void (*put_ctrl)(struct device *ctrl, struct wave6_vpu_entity *entity);
	int (*require_work_buffer)(struct device *ctrl,
				   struct wave6_vpu_entity *entity);
};

struct wave6_vpu_device;

struct wave6_vpu_ops {
	int (*get_vpu)(struct wave6_vpu_device *vpu,
		       struct wave6_vpu_entity *entity);
	void (*put_vpu)(struct wave6_vpu_device *vpu,
			struct wave6_vpu_entity *entity);
	int (*reg_core)(struct wave6_vpu_device *vpu,
			struct wave6_vpu_entity *entity);
	void (*unreg_core)(struct wave6_vpu_device *vpu,
			   struct wave6_vpu_entity *entity);
	int (*reg_ctrl)(struct wave6_vpu_device *vpu, struct device *ctrl,
			const struct wave6_vpu_ctrl_ops *ops);
	void (*unreg_ctrl)(struct wave6_vpu_device *vpu, struct device *ctrl);
	void (*req_work_buffer)(struct wave6_vpu_device *vpu,
				struct wave6_vpu_entity *entity);
	unsigned long (*get_clk_rate)(struct wave6_vpu_device *vpu);
};

struct wave6_vpu_device {
	struct device *dev;
	const struct wave6_vpu_ops *ops;
	struct clk_bulk_data *clks;
	int num_clks;
	struct device *ctrl;
	const struct wave6_vpu_ctrl_ops *ctrl_ops;
	struct wave6_vpu_entity *entities[WAVE6_VPU_MAXIMUM_ENTITY_CNT];
	struct mutex lock; /* the lock for vpu device */
	atomic_t ref_count;
	bool support_follower;
};

int wave6_alloc_dma(struct device *dev, struct vpu_buf *vb);
void wave6_free_dma(struct vpu_buf *vb);

#endif /* __WAVE6_VPU_H__ */
