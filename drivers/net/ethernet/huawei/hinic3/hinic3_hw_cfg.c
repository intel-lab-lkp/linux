// SPDX-License-Identifier: GPL-2.0
// Copyright (c) Huawei Technologies Co., Ltd. 2024. All rights reserved.

#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/mutex.h>
#include <linux/device.h>
#include <linux/pci.h>
#include <linux/module.h>
#include <linux/semaphore.h>

#include "hinic3_common.h"
#include "hinic3_hw_intf.h"
#include "hinic3_hwif.h"
#include "hinic3_mgmt.h"
#include "hinic3_hwdev.h"
#include "hinic3_hw_cfg.h"

enum {
	CFG_FREE = 0,
	CFG_BUSY = 1,
};

#define HINIC3_CFG_MAX_QP  256
#define VECTOR_THRESHOLD   2

#define CFG_SERVICE_MASK_NIC  (0x1 << SERVICE_T_NIC)
#define IS_NIC_TYPE(hwdev) \
	(((u32)(hwdev)->cfg_mgmt->svc_cap.chip_svc_type) & CFG_SERVICE_MASK_NIC)

static void parse_pub_res_cap(struct hinic3_hwdev *hwdev,
			      struct service_cap *cap,
			      const struct cfg_cmd_dev_cap *dev_cap,
			      enum func_type type)
{
	cap->port_id = dev_cap->port_id;

	cap->chip_svc_type = dev_cap->svc_cap_en;

	cap->cos_valid_bitmap = dev_cap->valid_cos_bitmap;
	cap->port_cos_valid_bitmap = dev_cap->port_cos_valid_bitmap;

	if (type != TYPE_VF)
		cap->max_vf = dev_cap->max_vf;
	else
		cap->max_vf = 0;

	dev_dbg(hwdev->dev, "Port_id: 0x%x, cos_bitmap: 0x%x, Max_vf: 0x%x\n",
		cap->port_id, cap->cos_valid_bitmap, cap->max_vf);
}

static void parse_l2nic_res_cap(struct hinic3_hwdev *hwdev,
				struct service_cap *cap,
				const struct cfg_cmd_dev_cap *dev_cap,
				enum func_type type)
{
	struct nic_service_cap *nic_cap = &cap->nic_cap;

	nic_cap->max_sqs = dev_cap->nic_max_sq_id + 1;
	nic_cap->max_rqs = dev_cap->nic_max_rq_id + 1;
	nic_cap->default_num_queues = dev_cap->nic_default_num_queues;

	dev_dbg(hwdev->dev, "L2nic resource capbility, max_sqs: 0x%x, max_rqs: 0x%x\n",
		nic_cap->max_sqs, nic_cap->max_rqs);

	/* Check parameters from firmware */
	if (nic_cap->max_sqs > HINIC3_CFG_MAX_QP ||
	    nic_cap->max_rqs > HINIC3_CFG_MAX_QP) {
		dev_dbg(hwdev->dev, "Number of qp exceeds limit[1-%d]: sq: %u, rq: %u\n",
			HINIC3_CFG_MAX_QP, nic_cap->max_sqs, nic_cap->max_rqs);
		nic_cap->max_sqs = HINIC3_CFG_MAX_QP;
		nic_cap->max_rqs = HINIC3_CFG_MAX_QP;
	}
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

	err = hinic3_msg_to_mgmt_sync(hwdev, HINIC3_MOD_CFGM, CFG_CMD_GET_DEV_CAP,
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

static int hinic3_get_dev_cap(struct hinic3_hwdev *hwdev)
{
	enum func_type type = HINIC3_FUNC_TYPE(hwdev);
	int err;

	switch (type) {
	case TYPE_PF:
	case TYPE_VF:
		err = get_cap_from_fw(hwdev, type);
		if (err) {
			dev_err(hwdev->dev, "Failed to get PF capability\n");
			return err;
		}
		break;
	default:
		dev_err(hwdev->dev, "Unsupported PCI Function type: %d\n",
			type);
		return -EINVAL;
	}

	return 0;
}

static void hinic3_init_ceq_info(struct hinic3_hwdev *hwdev)
{
	struct cfg_ceq_info *ceq_info = &hwdev->cfg_mgmt->ceq_info;

	ceq_info->num_ceq = hwdev->hwif->attr.num_ceqs;
	ceq_info->num_ceq_remain = ceq_info->num_ceq;
}

static int hinic3_init_cfg_ceq(struct hinic3_hwdev *hwdev)
{
	struct cfg_mgmt_info *cfg_mgmt = hwdev->cfg_mgmt;
	struct cfg_ceq *ceq;
	u8 num_ceq, i;

	hinic3_init_ceq_info(hwdev);
	num_ceq = cfg_mgmt->ceq_info.num_ceq;

	dev_dbg(hwdev->dev, "Cfg mgmt: ceqs=0x%x, remain=0x%x\n",
		cfg_mgmt->ceq_info.num_ceq, cfg_mgmt->ceq_info.num_ceq_remain);

	if (!num_ceq) {
		dev_err(hwdev->dev, "Ceq num cfg in fw is zero\n");
		return -EFAULT;
	}

	ceq = kcalloc(num_ceq, sizeof(*ceq), GFP_KERNEL);
	if (!ceq)
		return -ENOMEM;

	for (i = 0; i < num_ceq; ++i) {
		ceq[i].eqn = i;
		ceq[i].free = CFG_FREE;
		ceq[i].type = SERVICE_T_MAX;
	}

	cfg_mgmt->ceq_info.ceq = ceq;

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
	if (HINIC3_FUNC_TYPE(hwdev) == TYPE_VF)
		irq_info->interrupt_type = INTR_TYPE_MSIX;
	else
		irq_info->interrupt_type = 0;

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

	switch (cfg_mgmt->irq_info.interrupt_type) {
	case INTR_TYPE_MSIX:
		if (!nreq) {
			dev_err(hwdev->dev, "Number of interrupts must not be zero\n");
			return -EINVAL;
		}
		entry = kcalloc(nreq, sizeof(*entry), GFP_KERNEL);
		if (!entry)
			return -ENOMEM;

		for (i = 0; i < nreq; i++)
			entry[i].entry = i;

		actual_irq = pci_enable_msix_range(pdev, entry,
						   VECTOR_THRESHOLD, nreq);
		if (actual_irq < 0) {
			dev_err(hwdev->dev, "Alloc msix entries with threshold 2 failed. actual_irq: %d\n",
				actual_irq);
			kfree(entry);
			return -ENOMEM;
		}

		nreq = (u16)actual_irq;
		cfg_mgmt->irq_info.num_total = nreq;
		cfg_mgmt->irq_info.num_irq_remain = nreq;

		for (i = 0; i < nreq; ++i) {
			irq_alloc_info[i].info.msix_entry_idx = entry[i].entry;
			irq_alloc_info[i].info.irq_id = entry[i].vector;
			irq_alloc_info[i].type = SERVICE_T_MAX;
			irq_alloc_info[i].free = CFG_FREE;
		}

		kfree(entry);

		break;

	default:
		dev_err(hwdev->dev, "Unsupported interrupt type %d\n",
			cfg_mgmt->irq_info.interrupt_type);
		break;
	}

	return 0;
}

int hinic3_init_cfg_mgmt(struct hinic3_hwdev *hwdev)
{
	struct cfg_mgmt_info *cfg_mgmt;
	int err;

	cfg_mgmt = kzalloc(sizeof(*cfg_mgmt), GFP_KERNEL);
	if (!cfg_mgmt)
		return -ENOMEM;

	cfg_mgmt->hwdev = hwdev;
	hwdev->cfg_mgmt = cfg_mgmt;

	err = hinic3_init_cfg_ceq(hwdev);
	if (err) {
		dev_err(hwdev->dev, "Failed to init cfg_ceq, err: %d\n",
			err);
		goto free_mgmt_mem;
	}

	err = hinic3_init_irq_info(hwdev);
	if (err) {
		dev_err(hwdev->dev, "Failed to init cfg_irq_info, err: %d\n",
			err);
		goto free_eq_mem;
	}

	err = hinic3_init_irq_alloc_info(hwdev);
	if (err) {
		dev_err(hwdev->dev, "Failed to init irq_alloc_info, err: %d\n",
			err);
		goto free_interrupt_mem;
	}

	return 0;

free_interrupt_mem:
	kfree(cfg_mgmt->irq_info.alloc_info);
	cfg_mgmt->irq_info.alloc_info = NULL;

free_eq_mem:
	kfree(cfg_mgmt->ceq_info.ceq);
	cfg_mgmt->ceq_info.ceq = NULL;

free_mgmt_mem:
	kfree(cfg_mgmt);
	return err;
}

void hinic3_free_cfg_mgmt(struct hinic3_hwdev *hwdev)
{
	struct cfg_mgmt_info *cfg_mgmt = hwdev->cfg_mgmt;

	/* if the allocated resources were recycled */
	if (cfg_mgmt->irq_info.num_irq_remain !=
	    cfg_mgmt->irq_info.num_total ||
	    cfg_mgmt->ceq_info.num_ceq_remain != cfg_mgmt->ceq_info.num_ceq)
		dev_err(hwdev->dev, "Can't reclaim all irq and event queue\n");

	switch (cfg_mgmt->irq_info.interrupt_type) {
	case INTR_TYPE_MSIX:
		pci_disable_msix(hwdev->pdev);
		break;

	case INTR_TYPE_MSI:
		pci_disable_msi(hwdev->pdev);
		break;

	case INTR_TYPE_INT:
	default:
		break;
	}

	kfree(cfg_mgmt->irq_info.alloc_info);
	cfg_mgmt->irq_info.alloc_info = NULL;

	kfree(cfg_mgmt->ceq_info.ceq);
	cfg_mgmt->ceq_info.ceq = NULL;
	kfree(cfg_mgmt);
}

int hinic3_alloc_irqs(struct hinic3_hwdev *hwdev, enum hinic3_service_type type, u16 num,
		      struct irq_info *irq_info_array, u16 *act_num)
{
	struct cfg_irq_alloc_info *alloc_info;
	struct cfg_mgmt_info *cfg_mgmt;
	struct cfg_irq_info *irq_info;
	u16 num_new = num;
	u16 free_num_irq;
	int max_num_irq;
	int i, j;

	cfg_mgmt = hwdev->cfg_mgmt;
	irq_info = &cfg_mgmt->irq_info;
	alloc_info = irq_info->alloc_info;
	max_num_irq = irq_info->num_total;
	free_num_irq = irq_info->num_irq_remain;

	mutex_lock(&irq_info->irq_mutex);

	if (num > free_num_irq) {
		if (free_num_irq == 0) {
			dev_err(hwdev->dev, "no free irq resource in cfg mgmt.\n");
			mutex_unlock(&irq_info->irq_mutex);
			return -ENOMEM;
		}

		dev_warn(hwdev->dev, "only %u irq resource in cfg mgmt.\n", free_num_irq);
		num_new = free_num_irq;
	}

	*act_num = 0;

	for (i = 0; i < num_new; i++) {
		for (j = 0; j < max_num_irq; j++) {
			if (alloc_info[j].free == CFG_FREE) {
				if (irq_info->num_irq_remain == 0) {
					dev_err(hwdev->dev, "No free irq resource in cfg mgmt\n");
					mutex_unlock(&irq_info->irq_mutex);
					return -EINVAL;
				}
				alloc_info[j].type = type;
				alloc_info[j].free = CFG_BUSY;

				irq_info_array[i].msix_entry_idx =
					alloc_info[j].info.msix_entry_idx;
				irq_info_array[i].irq_id = alloc_info[j].info.irq_id;
				(*act_num)++;
				irq_info->num_irq_remain--;

				break;
			}
		}
	}

	mutex_unlock(&irq_info->irq_mutex);
	return 0;
}

void hinic3_free_irq(struct hinic3_hwdev *hwdev, enum hinic3_service_type type, u32 irq_id)
{
	struct cfg_irq_alloc_info *alloc_info;
	struct cfg_mgmt_info *cfg_mgmt;
	struct cfg_irq_info *irq_info;
	int max_num_irq;
	int i;

	cfg_mgmt = hwdev->cfg_mgmt;
	irq_info = &cfg_mgmt->irq_info;
	alloc_info = irq_info->alloc_info;
	max_num_irq = irq_info->num_total;

	mutex_lock(&irq_info->irq_mutex);

	for (i = 0; i < max_num_irq; i++) {
		if (irq_id == alloc_info[i].info.irq_id &&
		    type == alloc_info[i].type) {
			if (alloc_info[i].free == CFG_BUSY) {
				alloc_info[i].free = CFG_FREE;
				irq_info->num_irq_remain++;
				if (irq_info->num_irq_remain > max_num_irq) {
					dev_err(hwdev->dev, "Find target,but over range\n");
					mutex_unlock(&irq_info->irq_mutex);
					return;
				}
				break;
			}
		}
	}

	if (i >= max_num_irq)
		dev_warn(hwdev->dev, "Irq %u doesn't need to be freed\n", irq_id);

	mutex_unlock(&irq_info->irq_mutex);
}

int init_capability(struct hinic3_hwdev *hwdev)
{
	int err;

	err = hinic3_get_dev_cap(hwdev);
	if (err)
		return err;

	return 0;
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
