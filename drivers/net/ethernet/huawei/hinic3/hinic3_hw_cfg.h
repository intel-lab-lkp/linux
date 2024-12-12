/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (c) Huawei Technologies Co., Ltd. 2024. All rights reserved. */

#ifndef HINIC3_HW_CFG_H
#define HINIC3_HW_CFG_H

#include <linux/mutex.h>

struct hinic3_hwdev;

struct irq_info {
	u16 msix_entry_idx;
	/* provided by OS */
	u32 irq_id;
};

struct cfg_irq_alloc_info {
	bool                     allocated;
	struct irq_info          info;
};

struct cfg_irq_info {
	struct cfg_irq_alloc_info *alloc_info;
	u16                       num_irq;
	/* device max irq number */
	u16                       num_irq_hw;
	/* protect irq alloc and free */
	struct mutex              irq_mutex;
};

struct nic_service_cap {
	u16 max_sqs;
};

/* device capability */
struct service_cap {
	/* HW supported service type, reference to service_bit_define */
	u16                    chip_svc_type;
	/* physical port */
	u8                     port_id;
	/* NIC capability */
	struct nic_service_cap nic_cap;
};

struct cfg_mgmt_info {
	struct cfg_irq_info irq_info;
	struct service_cap  svc_cap;
};

int hinic3_init_cfg_mgmt(struct hinic3_hwdev *hwdev);
void hinic3_free_cfg_mgmt(struct hinic3_hwdev *hwdev);

int hinic3_alloc_irqs(struct hinic3_hwdev *hwdev, u16 num,
		      struct irq_info *alloc_arr, u16 *act_num);
void hinic3_free_irq(struct hinic3_hwdev *hwdev, u32 irq_id);

int init_capability(struct hinic3_hwdev *hwdev);
bool hinic3_support_nic(struct hinic3_hwdev *hwdev);
u16 hinic3_func_max_qnum(struct hinic3_hwdev *hwdev);
u8 hinic3_physical_port_id(struct hinic3_hwdev *hwdev);

#endif
