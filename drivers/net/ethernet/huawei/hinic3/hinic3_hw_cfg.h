/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved. */

#ifndef _HINIC3_HW_CFG_H_
#define _HINIC3_HW_CFG_H_

#include <linux/mutex.h>
#include <linux/pci.h>

struct hinic3_hwdev;

struct hinic3_irq {
	u32  irq_id;
	u16  msix_entry_idx;
	bool allocated;
};

struct hinic3_irq_info {
	struct hinic3_irq *irq;
	u16               num_irq;
	/* device max irq number */
	u16               num_irq_hw;
	/* protect irq alloc and free */
	struct mutex      irq_mutex;
};

struct hinic3_nic_service_cap {
	u16 max_sqs;
};

/* device capability */
struct hinic3_service_cap {
	/* HW supported service type, reference to service_bit_define */
	u16                           chip_svc_type;
	/* physical port */
	u8                            port_id;
	/* NIC capability */
	struct hinic3_nic_service_cap nic_cap;
};

struct hinic3_cfg_mgmt_info {
	struct hinic3_irq_info    irq_info;
	struct hinic3_service_cap svc_cap;
};

int hinic3_alloc_irqs(struct hinic3_hwdev *hwdev, u16 num,
		      struct msix_entry *alloc_arr, u16 *act_num);
void hinic3_free_irq(struct hinic3_hwdev *hwdev, u32 irq_id);

bool hinic3_support_nic(struct hinic3_hwdev *hwdev);
u16 hinic3_func_max_qnum(struct hinic3_hwdev *hwdev);
u8 hinic3_physical_port_id(struct hinic3_hwdev *hwdev);

#endif
