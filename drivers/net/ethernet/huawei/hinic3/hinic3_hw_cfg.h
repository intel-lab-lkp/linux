/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (c) Huawei Technologies Co., Ltd. 2024. All rights reserved. */

#ifndef HINIC3_HW_CFG_H
#define HINIC3_HW_CFG_H

#include <linux/types.h>
#include <linux/mutex.h>

#include "hinic3_hw_intf.h"

struct hinic3_hwdev;

enum intr_type {
	INTR_TYPE_MSIX,
	INTR_TYPE_MSI,
	INTR_TYPE_INT,
	INTR_TYPE_NONE,
};

struct cfg_ceq {
	enum hinic3_service_type type;
	int                      eqn;
	/* 1 - allocated, 0- freed */
	int                      free;
};

struct cfg_ceq_info {
	struct cfg_ceq *ceq;
	u8             num_ceq;
	u8             num_ceq_remain;
};

struct irq_info {
	u16 msix_entry_idx;
	/* provided by OS */
	u32 irq_id;
};

struct cfg_irq_alloc_info {
	enum hinic3_service_type type;
	/* 1 - allocated, 0- freed */
	int                      free;
	struct irq_info          info;
};

struct cfg_irq_info {
	struct cfg_irq_alloc_info *alloc_info;
	enum intr_type            interrupt_type;
	u16                       num_total;
	u16                       num_irq_remain;
	/* device max irq number */
	u16                       num_irq_hw;

	/* protect irq alloc and free */
	struct mutex              irq_mutex;
};

struct nic_service_cap {
	u16 max_sqs;
	u16 max_rqs;
	u16 default_num_queues;
};

/* device capability */
struct service_cap {
	/* HW supported service type, reference to service_bit_define */
	u16                    chip_svc_type;
	/* PF/VF's physical port */
	u8                     port_id;

	u8                     cos_valid_bitmap;
	u8                     port_cos_valid_bitmap;
	/* max number of VFs that PF supports */
	u16                    max_vf;

	/* NIC capability */
	struct nic_service_cap nic_cap;
};

struct cfg_mgmt_info {
	struct hinic3_hwdev *hwdev;
	/* Completion event queue (ceq) */
	struct cfg_ceq_info ceq_info;
	/* interrupt queue (irq) */
	struct cfg_irq_info irq_info;
	struct service_cap  svc_cap;
};

int hinic3_init_cfg_mgmt(struct hinic3_hwdev *hwdev);
void hinic3_free_cfg_mgmt(struct hinic3_hwdev *hwdev);

int hinic3_alloc_irqs(struct hinic3_hwdev *hwdev, enum hinic3_service_type type, u16 num,
		      struct irq_info *irq_info_array, u16 *act_num);
void hinic3_free_irq(struct hinic3_hwdev *hwdev, enum hinic3_service_type type, u32 irq_id);

int init_capability(struct hinic3_hwdev *hwdev);
bool hinic3_support_nic(struct hinic3_hwdev *hwdev);
u16 hinic3_func_max_qnum(struct hinic3_hwdev *hwdev);
u8 hinic3_physical_port_id(struct hinic3_hwdev *hwdev);

#endif
