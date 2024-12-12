// SPDX-License-Identifier: GPL-2.0
// Copyright (c) Huawei Technologies Co., Ltd. 2024. All rights reserved.

#include <linux/device.h>

#include "hinic3_hw_cfg.h"
#include "hinic3_hwdev.h"
#include "hinic3_mbox.h"
#include "hinic3_hwif.h"

#define IS_NIC_TYPE(hwdev) \
	(((u32)(hwdev)->cfg_mgmt->svc_cap.chip_svc_type) & BIT(SERVICE_T_NIC))

int hinic3_alloc_irqs(struct hinic3_hwdev *hwdev, u16 num,
		      struct irq_info *alloc_arr, u16 *act_num)
{
	struct cfg_irq_alloc_info *curr;
	struct cfg_irq_info *irq_info;
	u16 i, found = 0;

	irq_info = &hwdev->cfg_mgmt->irq_info;
	mutex_lock(&irq_info->irq_mutex);
	for (i = 0; i < irq_info->num_irq && found < num; i++) {
		curr = irq_info->alloc_info + i;
		if (curr->allocated)
			continue;
		curr->allocated = true;
		alloc_arr[found].msix_entry_idx = curr->info.msix_entry_idx;
		alloc_arr[found].irq_id = curr->info.irq_id;
		found++;
	}
	mutex_unlock(&irq_info->irq_mutex);

	*act_num = found;
	return found == 0 ? -ENOMEM : 0;
}

void hinic3_free_irq(struct hinic3_hwdev *hwdev, u32 irq_id)
{
	struct cfg_irq_alloc_info *curr;
	struct cfg_irq_info *irq_info;
	u16 i;

	irq_info = &hwdev->cfg_mgmt->irq_info;
	mutex_lock(&irq_info->irq_mutex);
	for (i = 0; i < irq_info->num_irq; i++) {
		curr = irq_info->alloc_info + i;
		if (curr->info.irq_id == irq_id) {
			curr->allocated = false;
			break;
		}
	}
	mutex_unlock(&irq_info->irq_mutex);
}

bool hinic3_support_nic(struct hinic3_hwdev *hwdev)
{
	if (!IS_NIC_TYPE(hwdev))
		return false;

	return true;
}

u16 hinic3_func_max_qnum(struct hinic3_hwdev *hwdev)
{
	return hwdev->cfg_mgmt->svc_cap.nic_cap.max_sqs;
}

u8 hinic3_physical_port_id(struct hinic3_hwdev *hwdev)
{
	return hwdev->cfg_mgmt->svc_cap.port_id;
}
