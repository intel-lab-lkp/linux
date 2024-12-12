// SPDX-License-Identifier: GPL-2.0
// Copyright (c) Huawei Technologies Co., Ltd. 2024. All rights reserved.

#include <linux/device.h>

#include "hinic3_hw_cfg.h"
#include "hinic3_hwdev.h"
#include "hinic3_mbox.h"
#include "hinic3_hwif.h"

#define HINIC3_CFG_MAX_QP  256
#define VECTOR_THRESHOLD   2

#define IS_NIC_TYPE(hwdev) \
	(((u32)(hwdev)->cfg_mgmt->svc_cap.chip_svc_type) & BIT(SERVICE_T_NIC))

static void parse_pub_res_cap(struct hinic3_hwdev *hwdev,
			      struct service_cap *cap,
			      const struct cfg_cmd_dev_cap *dev_cap,
			      enum func_type type)
{
	cap->port_id = dev_cap->port_id;
	cap->chip_svc_type = dev_cap->svc_cap_en;
}

static void parse_l2nic_res_cap(struct hinic3_hwdev *hwdev,
				struct service_cap *cap,
				const struct cfg_cmd_dev_cap *dev_cap,
				enum func_type type)
{
	struct nic_service_cap *nic_cap = &cap->nic_cap;

	nic_cap->max_sqs = min(dev_cap->nic_max_sq_id + 1, HINIC3_CFG_MAX_QP);
}

static void parse_dev_cap(struct hinic3_hwdev *hwdev,
			  const struct cfg_cmd_dev_cap *dev_cap, enum func_type type)
{
	struct service_cap *cap = &hwdev->cfg_mgmt->svc_cap;

	/* Public resource */
	parse_pub_res_cap(hwdev, cap, dev_cap, type);

	/* L2 NIC resource */
	if (IS_NIC_TYPE(hwdev))
		parse_l2nic_res_cap(hwdev, cap, dev_cap, type);
}

static int get_cap_from_fw(struct hinic3_hwdev *hwdev, enum func_type type)
{
	struct cfg_cmd_dev_cap dev_cap;
	u32 out_len = sizeof(dev_cap);
	int err;

	memset(&dev_cap, 0, sizeof(dev_cap));
	dev_cap.func_id = hinic3_global_func_id(hwdev);

	err = hinic3_send_mbox_to_mgmt(hwdev, HINIC3_MOD_CFGM,
				       CFG_CMD_GET_DEV_CAP,
				       &dev_cap, sizeof(dev_cap),
				       &dev_cap, &out_len, 0);
	if (err || dev_cap.head.status || !out_len) {
		dev_err(hwdev->dev,
			"Failed to get capability from FW, err: %d, status: 0x%x, out size: 0x%x\n",
			err, dev_cap.head.status, out_len);
		return -EIO;
	}

	parse_dev_cap(hwdev, &dev_cap, type);

	return 0;
}

static int hinic3_init_irq_info(struct hinic3_hwdev *hwdev)
{
	struct cfg_mgmt_info *cfg_mgmt = hwdev->cfg_mgmt;
	struct hinic3_hwif *hwif = hwdev->hwif;
	u16 intr_num = hwif->attr.num_irqs;
	struct cfg_irq_info *irq_info;
	u16 intr_needed;

	if (!intr_num) {
		dev_err(hwdev->dev, "Irq num cfg in fw is zero, msix_flex_en %d\n",
			hwif->attr.msix_flex_en);
		return -EFAULT;
	}

	intr_needed = hwif->attr.msix_flex_en ? (hwif->attr.num_aeqs +
		      hwif->attr.num_ceqs + hwif->attr.num_sq) : intr_num;
	if (intr_needed > intr_num) {
		dev_warn(hwdev->dev, "Irq num cfg %d is less than the needed irq num %d msix_flex_en %d\n",
			 intr_num, intr_needed, hwdev->hwif->attr.msix_flex_en);
		intr_needed = intr_num;
	}

	irq_info = &cfg_mgmt->irq_info;
	irq_info->alloc_info = kcalloc(intr_num, sizeof(*irq_info->alloc_info),
				       GFP_KERNEL);
	if (!irq_info->alloc_info)
		return -ENOMEM;

	irq_info->num_irq_hw = intr_needed;
	mutex_init(&irq_info->irq_mutex);

	return 0;
}

static int hinic3_init_irq_alloc_info(struct hinic3_hwdev *hwdev)
{
	struct cfg_mgmt_info *cfg_mgmt = hwdev->cfg_mgmt;
	struct cfg_irq_alloc_info *irq_alloc_info;
	u16 nreq = cfg_mgmt->irq_info.num_irq_hw;
	struct pci_dev *pdev = hwdev->pdev;
	struct msix_entry *entry;
	int actual_irq;
	u16 i;

	irq_alloc_info = cfg_mgmt->irq_info.alloc_info;

	if (!nreq) {
		dev_err(hwdev->dev, "Number of interrupts must not be zero\n");
		return -EINVAL;
	}
	entry = kcalloc(nreq, sizeof(*entry), GFP_KERNEL);
	if (!entry)
		return -ENOMEM;

	for (i = 0; i < nreq; i++)
		entry[i].entry = i;

	actual_irq = pci_enable_msix_range(pdev, entry, VECTOR_THRESHOLD, nreq);
	if (actual_irq < 0) {
		dev_err(hwdev->dev, "Alloc msix entries with threshold 2 failed. actual_irq: %d\n",
			actual_irq);
		kfree(entry);
		return -ENOMEM;
	}

	nreq = (u16)actual_irq;
	cfg_mgmt->irq_info.num_irq = nreq;

	for (i = 0; i < nreq; ++i) {
		irq_alloc_info[i].info.msix_entry_idx = entry[i].entry;
		irq_alloc_info[i].info.irq_id = entry[i].vector;
		irq_alloc_info[i].allocated = false;
	}

	kfree(entry);
	return 0;
}

int hinic3_init_cfg_mgmt(struct hinic3_hwdev *hwdev)
{
	struct cfg_mgmt_info *cfg_mgmt;
	int err;

	if (!hwdev->hwif->attr.num_ceqs) {
		dev_err(hwdev->dev, "Ceq num cfg in fw is zero\n");
		return -EINVAL;
	}

	cfg_mgmt = kzalloc(sizeof(*cfg_mgmt), GFP_KERNEL);
	if (!cfg_mgmt)
		return -ENOMEM;

	hwdev->cfg_mgmt = cfg_mgmt;

	err = hinic3_init_irq_info(hwdev);
	if (err) {
		dev_err(hwdev->dev, "Failed to init cfg_irq_info, err: %d\n",
			err);
		goto err_undo_cfg_mgmt_alloc;
	}

	err = hinic3_init_irq_alloc_info(hwdev);
	if (err) {
		dev_err(hwdev->dev, "Failed to init irq_alloc_info, err: %d\n",
			err);
		goto err_undo_irq_info_alloc;
	}

	return 0;

err_undo_irq_info_alloc:
	kfree(cfg_mgmt->irq_info.alloc_info);
	cfg_mgmt->irq_info.alloc_info = NULL;

err_undo_cfg_mgmt_alloc:
	kfree(cfg_mgmt);
	return err;
}

void hinic3_free_cfg_mgmt(struct hinic3_hwdev *hwdev)
{
	struct cfg_mgmt_info *cfg_mgmt = hwdev->cfg_mgmt;

	pci_disable_msix(hwdev->pdev);
	kfree(cfg_mgmt->irq_info.alloc_info);
	cfg_mgmt->irq_info.alloc_info = NULL;
	kfree(cfg_mgmt);
}

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

int init_capability(struct hinic3_hwdev *hwdev)
{
	return get_cap_from_fw(hwdev, TYPE_VF);
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
