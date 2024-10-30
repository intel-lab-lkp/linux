// SPDX-License-Identifier: GPL-2.0
// Copyright (c) Huawei Technologies Co., Ltd. 2024. All rights reserved.

#include <linux/time.h>
#include <linux/timex.h>
#include <linux/rtc.h>
#include <linux/kernel.h>
#include <linux/pci.h>
#include <linux/types.h>
#include <linux/module.h>
#include <linux/completion.h>
#include <linux/semaphore.h>
#include <linux/interrupt.h>
#include <linux/vmalloc.h>
#include <linux/bits.h>
#include <linux/bitfield.h>

#include "hinic3_csr.h"
#include "hinic3_common.h"
#include "hinic3_hwif.h"
#include "hinic3_hw_cfg.h"
#include "hinic3_eqs.h"
#include "hinic3_mbox.h"
#include "hinic3_mgmt.h"
#include "hinic3_hw_comm.h"
#include "hinic3_cmdq.h"
#include "hinic3_lld.h"
#include "hinic3_hwdev.h"

enum hinic3_pcie_nosnoop {
	HINIC3_PCIE_SNOOP    = 0,
	HINIC3_PCIE_NO_SNOOP = 1,
};

enum hinic3_pcie_tph {
	HINIC3_PCIE_TPH_DISABLE = 0,
	HINIC3_PCIE_TPH_ENABLE  = 1,
};

#define HINIC3_SYNFW_TIME_PERIOD  (60 * 60 * 1000)

#define HINIC3_DMA_ATTR_INDIR_IDX_MASK          GENMASK(9, 0)
#define HINIC3_DMA_ATTR_INDIR_IDX_SET(val, member)  \
	FIELD_PREP(HINIC3_DMA_ATTR_INDIR_##member##_MASK, val)

#define HINIC3_DMA_ATTR_ENTRY_ST_MASK           GENMASK(7, 0)
#define HINIC3_DMA_ATTR_ENTRY_AT_MASK           GENMASK(9, 8)
#define HINIC3_DMA_ATTR_ENTRY_PH_MASK           GENMASK(11, 10)
#define HINIC3_DMA_ATTR_ENTRY_NO_SNOOPING_MASK  BIT(12)
#define HINIC3_DMA_ATTR_ENTRY_TPH_EN_MASK       BIT(13)
#define HINIC3_DMA_ATTR_ENTRY_SET(val, member)  \
	FIELD_PREP(HINIC3_DMA_ATTR_ENTRY_##member##_MASK, val)

#define HINIC3_PCIE_ST_DISABLE  0
#define HINIC3_PCIE_AT_DISABLE  0
#define HINIC3_PCIE_PH_DISABLE  0

#define PCIE_MSIX_ATTR_ENTRY    0

#define HINIC3_CHIP_PRESENT     1
#define HINIC3_CHIP_ABSENT      0

#define HINIC3_DEAULT_EQ_MSIX_PENDING_LIMIT      0
#define HINIC3_DEAULT_EQ_MSIX_COALESC_TIMER_CFG  0xFF
#define HINIC3_DEAULT_EQ_MSIX_RESEND_TIMER_CFG   7

#define HINIC3_HWDEV_WQ_NAME    "hinic3_hardware"
#define HINIC3_WQ_MAX_REQ       10

enum hinic3_hwdev_init_state {
	HINIC3_HWDEV_NONE_INITED = 0,
	HINIC3_HWDEV_MGMT_INITED = 1,
	HINIC3_HWDEV_MBOX_INITED = 2,
	HINIC3_HWDEV_CMDQ_INITED = 3,
};

static int hinic3_comm_aeqs_init(struct hinic3_hwdev *hwdev)
{
	struct irq_info aeq_irqs[HINIC3_MAX_AEQS];
	u16 num_aeqs, resp_num_irq, i;
	int err;

	num_aeqs = hwdev->hwif->attr.num_aeqs;
	if (num_aeqs > HINIC3_MAX_AEQS) {
		dev_warn(hwdev->dev, "Adjust aeq num to %d\n",
			 HINIC3_MAX_AEQS);
		num_aeqs = HINIC3_MAX_AEQS;
	}
	err = hinic3_alloc_irqs(hwdev, SERVICE_T_INTF, num_aeqs, aeq_irqs,
				&resp_num_irq);
	if (err) {
		dev_err(hwdev->dev, "Failed to alloc aeq irqs, num_aeqs: %u\n",
			num_aeqs);
		return err;
	}

	if (resp_num_irq < num_aeqs) {
		dev_warn(hwdev->dev, "Adjust aeq num to %u\n",
			 resp_num_irq);
		num_aeqs = resp_num_irq;
	}

	err = hinic3_aeqs_init(hwdev, num_aeqs, aeq_irqs);
	if (err) {
		dev_err(hwdev->dev, "Failed to init aeqs\n");
		goto err_aeqs_init;
	}

	return 0;

err_aeqs_init:
	for (i = 0; i < num_aeqs; i++)
		hinic3_free_irq(hwdev, SERVICE_T_INTF, aeq_irqs[i].irq_id);

	return err;
}

static void hinic3_comm_aeqs_free(struct hinic3_hwdev *hwdev)
{
	struct irq_info aeq_irqs[HINIC3_MAX_AEQS];
	u16 num_irqs, i;

	hinic3_get_aeq_irqs(hwdev, aeq_irqs, &num_irqs);

	hinic3_aeqs_free(hwdev);

	for (i = 0; i < num_irqs; i++)
		hinic3_free_irq(hwdev, SERVICE_T_INTF, aeq_irqs[i].irq_id);
}

static int hinic3_comm_ceqs_init(struct hinic3_hwdev *hwdev)
{
	struct irq_info ceq_irqs[HINIC3_MAX_CEQS];
	u16 num_ceqs, resp_num_irq, i;
	int err;

	num_ceqs = hwdev->hwif->attr.num_ceqs;
	if (num_ceqs > HINIC3_MAX_CEQS) {
		dev_warn(hwdev->dev, "Adjust ceq num to %d\n",
			 HINIC3_MAX_CEQS);
		num_ceqs = HINIC3_MAX_CEQS;
	}

	err = hinic3_alloc_irqs(hwdev, SERVICE_T_INTF, num_ceqs, ceq_irqs,
				&resp_num_irq);
	if (err) {
		dev_err(hwdev->dev, "Failed to alloc ceq irqs, num_ceqs: %u\n",
			num_ceqs);
		return err;
	}

	if (resp_num_irq < num_ceqs) {
		dev_warn(hwdev->dev, "Adjust ceq num to %u\n",
			 resp_num_irq);
		num_ceqs = resp_num_irq;
	}

	err = hinic3_ceqs_init(hwdev, num_ceqs, ceq_irqs);
	if (err) {
		dev_err(hwdev->dev,
			"Failed to init ceqs, err:%d\n", err);
		goto err_ceqs_init;
	}

	return 0;

err_ceqs_init:
	for (i = 0; i < num_ceqs; i++)
		hinic3_free_irq(hwdev, SERVICE_T_INTF, ceq_irqs[i].irq_id);

	return err;
}

static void hinic3_comm_ceqs_free(struct hinic3_hwdev *hwdev)
{
	struct irq_info ceq_irqs[HINIC3_MAX_CEQS];
	u16 num_irqs;
	int i;

	hinic3_get_ceq_irqs(hwdev, ceq_irqs, &num_irqs);

	hinic3_ceqs_free(hwdev);

	for (i = 0; i < num_irqs; i++)
		hinic3_free_irq(hwdev, SERVICE_T_INTF, ceq_irqs[i].irq_id);
}

static int hinic3_comm_mbox_init(struct hinic3_hwdev *hwdev)
{
	int err;

	err = hinic3_init_mbox(hwdev);
	if (err)
		return err;

	hinic3_aeq_register_hw_cb(hwdev, HINIC3_MBX_FROM_FUNC,
				  hinic3_mbox_func_aeqe_handler);
	hinic3_aeq_register_hw_cb(hwdev, HINIC3_MSG_FROM_FW,
				  hinic3_mgmt_msg_aeqe_handler);

	set_bit(HINIC3_HWDEV_MBOX_INITED, &hwdev->func_state);

	return 0;
}

static void hinic3_comm_mbox_free(struct hinic3_hwdev *hwdev)
{
	spin_lock_bh(&hwdev->channel_lock);
	clear_bit(HINIC3_HWDEV_MBOX_INITED, &hwdev->func_state);
	spin_unlock_bh(&hwdev->channel_lock);

	hinic3_aeq_unregister_hw_cb(hwdev, HINIC3_MBX_FROM_FUNC);
	hinic3_aeq_unregister_hw_cb(hwdev, HINIC3_MSG_FROM_FW);

	hinic3_free_mbox(hwdev);
}

static int init_aeqs_msix_attr(struct hinic3_hwdev *hwdev)
{
	struct hinic3_aeqs *aeqs = hwdev->aeqs;
	struct interrupt_info info = {};
	struct hinic3_eq *eq;
	int q_id;
	int err;

	info.lli_set = 0;
	info.interrupt_coalesc_set = 1;
	info.pending_limt = HINIC3_DEAULT_EQ_MSIX_PENDING_LIMIT;
	info.coalesc_timer_cfg = HINIC3_DEAULT_EQ_MSIX_COALESC_TIMER_CFG;
	info.resend_timer_cfg = HINIC3_DEAULT_EQ_MSIX_RESEND_TIMER_CFG;

	for (q_id = aeqs->num_aeqs - 1; q_id >= 0; q_id--) {
		eq = &aeqs->aeq[q_id];
		info.msix_index = eq->eq_irq.msix_entry_idx;
		err = hinic3_set_interrupt_cfg_direct(hwdev, &info);
		if (err) {
			dev_err(hwdev->dev, "Set msix attr for aeq %d failed\n",
				q_id);
			return -EFAULT;
		}
	}

	return 0;
}

static int init_ceqs_msix_attr(struct hinic3_hwdev *hwdev)
{
	struct hinic3_ceqs *ceqs = hwdev->ceqs;
	struct interrupt_info info = {};
	struct hinic3_eq *eq;
	u16 q_id;
	int err;

	info.lli_set = 0;
	info.interrupt_coalesc_set = 1;
	info.pending_limt = HINIC3_DEAULT_EQ_MSIX_PENDING_LIMIT;
	info.coalesc_timer_cfg = HINIC3_DEAULT_EQ_MSIX_COALESC_TIMER_CFG;
	info.resend_timer_cfg = HINIC3_DEAULT_EQ_MSIX_RESEND_TIMER_CFG;

	for (q_id = 0; q_id < ceqs->num_ceqs; q_id++) {
		eq = &ceqs->ceq[q_id];
		info.msix_index = eq->eq_irq.msix_entry_idx;
		err = hinic3_set_interrupt_cfg(hwdev, info);
		if (err) {
			dev_err(hwdev->dev, "Set msix attr for ceq %u failed\n",
				q_id);
			return -EFAULT;
		}
	}

	return 0;
}

static int hinic3_comm_pf_to_mgmt_init(struct hinic3_hwdev *hwdev)
{
	int err;

	if (HINIC3_IS_VF(hwdev))
		return 0;

	err = hinic3_pf_to_mgmt_init(hwdev);
	if (err)
		return err;

	set_bit(HINIC3_HWDEV_MGMT_INITED, &hwdev->func_state);

	return 0;
}

static void hinic3_comm_pf_to_mgmt_free(struct hinic3_hwdev *hwdev)
{
	if (HINIC3_IS_VF(hwdev))
		return;

	spin_lock_bh(&hwdev->channel_lock);
	clear_bit(HINIC3_HWDEV_MGMT_INITED, &hwdev->func_state);
	spin_unlock_bh(&hwdev->channel_lock);

	hinic3_aeq_unregister_hw_cb(hwdev, HINIC3_MSG_FROM_FW);

	hinic3_pf_to_mgmt_free(hwdev);
}

static int init_basic_mgmt_channel(struct hinic3_hwdev *hwdev)
{
	int err;

	err = hinic3_comm_aeqs_init(hwdev);
	if (err) {
		dev_err(hwdev->dev, "Failed to init async event queues\n");
		return err;
	}

	err = hinic3_comm_mbox_init(hwdev);
	if (err) {
		dev_err(hwdev->dev, "Failed to init mailbox\n");
		goto err_comm_mbox_init;
	}

	err = init_aeqs_msix_attr(hwdev);
	if (err) {
		dev_err(hwdev->dev, "Failed to init aeqs msix attr\n");
		goto err_aeqs_msix_attr_init;
	}

	return 0;

err_aeqs_msix_attr_init:
	hinic3_comm_mbox_free(hwdev);

err_comm_mbox_init:
	hinic3_comm_aeqs_free(hwdev);

	return err;
}

static void free_base_mgmt_channel(struct hinic3_hwdev *hwdev)
{
	hinic3_comm_mbox_free(hwdev);
	hinic3_comm_aeqs_free(hwdev);
}

static int dma_attr_table_init(struct hinic3_hwdev *hwdev)
{
	u32 addr, val, dst_attr;

	/* Indirect access, set entry_idx first */
	addr = HINIC3_CSR_DMA_ATTR_INDIR_IDX_ADDR;
	val = hinic3_hwif_read_reg(hwdev->hwif, addr);
	val &= ~HINIC3_DMA_ATTR_ENTRY_AT_MASK;
	val |= HINIC3_DMA_ATTR_INDIR_IDX_SET(PCIE_MSIX_ATTR_ENTRY, IDX);
	hinic3_hwif_write_reg(hwdev->hwif, addr, val);

	addr = HINIC3_CSR_DMA_ATTR_TBL_ADDR;
	val = hinic3_hwif_read_reg(hwdev->hwif, addr);

	dst_attr = HINIC3_DMA_ATTR_ENTRY_SET(HINIC3_PCIE_ST_DISABLE, ST) |
		   HINIC3_DMA_ATTR_ENTRY_SET(HINIC3_PCIE_AT_DISABLE, AT) |
		   HINIC3_DMA_ATTR_ENTRY_SET(HINIC3_PCIE_PH_DISABLE, PH) |
		   HINIC3_DMA_ATTR_ENTRY_SET(HINIC3_PCIE_SNOOP, NO_SNOOPING) |
		   HINIC3_DMA_ATTR_ENTRY_SET(HINIC3_PCIE_TPH_DISABLE, TPH_EN);
	if (val == dst_attr)
		return 0;

	return hinic3_set_dma_attr_tbl(hwdev, PCIE_MSIX_ATTR_ENTRY, HINIC3_PCIE_ST_DISABLE,
				       HINIC3_PCIE_AT_DISABLE, HINIC3_PCIE_PH_DISABLE,
				       HINIC3_PCIE_SNOOP, HINIC3_PCIE_TPH_DISABLE);
}

static int init_basic_attributes(struct hinic3_hwdev *hwdev)
{
	struct comm_global_attr glb_attr;
	int err;

	err = hinic3_func_reset(hwdev, hinic3_global_func_id(hwdev), HINIC3_COMM_RES);
	if (err)
		return err;

	err = hinic3_get_comm_features(hwdev, hwdev->features,
				       COMM_MAX_FEATURE_QWORD);
	if (err)
		return err;

	dev_dbg(hwdev->dev, "Comm hw features: 0x%llx\n", hwdev->features[0]);

	err = hinic3_get_global_attr(hwdev, &glb_attr);
	if (err)
		return err;

	err = hinic3_set_func_svc_used_state(hwdev, SVC_T_COMM, 1);
	if (err)
		return err;

	err = dma_attr_table_init(hwdev);
	if (err)
		return err;

	hwdev->max_pf = glb_attr.max_pf_num;
	hwdev->max_cmdq = min(glb_attr.cmdq_num, HINIC3_MAX_CMDQ_TYPES);
	dev_dbg(hwdev->dev,
		"global attribute: max_host: 0x%x, max_pf: 0x%x, vf_id_start: 0x%x, mgmt node id: 0x%x, cmdq_num: 0x%x\n",
		glb_attr.max_host_num, glb_attr.max_pf_num,
		glb_attr.vf_id_start, glb_attr.mgmt_host_node_id,
		glb_attr.cmdq_num);

	return 0;
}

static int hinic3_comm_cmdqs_init(struct hinic3_hwdev *hwdev)
{
	int err;

	err = hinic3_cmdqs_init(hwdev);
	if (err) {
		dev_err(hwdev->dev, "Failed to init cmd queues\n");
		return err;
	}

	hinic3_ceq_register_cb(hwdev, HINIC3_CMDQ, hinic3_cmdq_ceq_handler);

	err = hinic3_set_cmdq_depth(hwdev, HINIC3_CMDQ_DEPTH);
	if (err) {
		dev_err(hwdev->dev, "Failed to set cmdq depth\n");
		goto err_set_cmdq_depth;
	}

	set_bit(HINIC3_HWDEV_CMDQ_INITED, &hwdev->func_state);

	return 0;

err_set_cmdq_depth:
	hinic3_cmdqs_free(hwdev);

	return err;
}

static void hinic3_comm_cmdqs_free(struct hinic3_hwdev *hwdev)
{
	spin_lock_bh(&hwdev->channel_lock);
	clear_bit(HINIC3_HWDEV_CMDQ_INITED, &hwdev->func_state);
	spin_unlock_bh(&hwdev->channel_lock);

	hinic3_ceq_unregister_cb(hwdev, HINIC3_CMDQ);
	hinic3_cmdqs_free(hwdev);
}

static int init_cmdqs_channel(struct hinic3_hwdev *hwdev)
{
	int err;

	err = hinic3_comm_ceqs_init(hwdev);
	if (err) {
		dev_err(hwdev->dev, "Failed to init completion event queues\n");
		return err;
	}

	err = init_ceqs_msix_attr(hwdev);
	if (err) {
		dev_err(hwdev->dev, "Failed to init ceqs msix attr\n");
		goto err_init_ceq_msix;
	}

	hwdev->wq_page_size = HINIC3_MIN_PAGE_SIZE * (1U << HINIC3_WQ_PAGE_SIZE_ORDER);
	err = hinic3_set_wq_page_size(hwdev, hinic3_global_func_id(hwdev),
				      hwdev->wq_page_size);
	if (err) {
		dev_err(hwdev->dev, "Failed to set wq page size\n");
		goto err_init_wq_pg_size;
	}

	err = hinic3_comm_cmdqs_init(hwdev);
	if (err) {
		dev_err(hwdev->dev, "Failed to init cmd queues\n");
		goto err_cmdq_init;
	}

	return 0;

err_cmdq_init:
	if (HINIC3_FUNC_TYPE(hwdev) != TYPE_VF)
		hinic3_set_wq_page_size(hwdev, hinic3_global_func_id(hwdev),
					HINIC3_MIN_PAGE_SIZE);
err_init_wq_pg_size:
err_init_ceq_msix:
	hinic3_comm_ceqs_free(hwdev);

	return err;
}

static void hinic3_free_cmdqs_channel(struct hinic3_hwdev *hwdev)
{
	hinic3_comm_cmdqs_free(hwdev);

	if (HINIC3_FUNC_TYPE(hwdev) != TYPE_VF)
		hinic3_set_wq_page_size(hwdev, hinic3_global_func_id(hwdev),
					HINIC3_MIN_PAGE_SIZE);

	hinic3_comm_ceqs_free(hwdev);
}

static int hinic3_init_comm_ch(struct hinic3_hwdev *hwdev)
{
	int err;

	err = init_basic_mgmt_channel(hwdev);
	if (err)
		return err;

	err = hinic3_comm_pf_to_mgmt_init(hwdev);
	if (err)
		goto err_pf_to_mgmt_init;

	err = init_basic_attributes(hwdev);
	if (err)
		goto err_init_basic_attr;

	err = init_cmdqs_channel(hwdev);
	if (err) {
		dev_err(hwdev->dev, "Failed to init cmdq channel\n");
		goto err_init_cmdqs_channel;
	}

	hinic3_set_pf_status(hwdev->hwif, HINIC3_PF_STATUS_ACTIVE_FLAG);

	return 0;

err_init_cmdqs_channel:
	hinic3_set_func_svc_used_state(hwdev, SVC_T_COMM, 0);

err_init_basic_attr:
	hinic3_comm_pf_to_mgmt_free(hwdev);

err_pf_to_mgmt_init:
	free_base_mgmt_channel(hwdev);

	return err;
}

static void hinic3_uninit_comm_ch(struct hinic3_hwdev *hwdev)
{
	hinic3_set_pf_status(hwdev->hwif, HINIC3_PF_STATUS_INIT);
	hinic3_free_cmdqs_channel(hwdev);
	hinic3_set_func_svc_used_state(hwdev, SVC_T_COMM, 0);
	hinic3_comm_pf_to_mgmt_free(hwdev);
	free_base_mgmt_channel(hwdev);
}

static void hinic3_auto_sync_time_work(struct work_struct *work)
{
	struct delayed_work *delay = to_delayed_work(work);
	struct hinic3_hwdev *hwdev;

	hwdev = container_of(delay, struct hinic3_hwdev, sync_time_task);
	queue_delayed_work(hwdev->workq, &hwdev->sync_time_task,
			   msecs_to_jiffies(HINIC3_SYNFW_TIME_PERIOD));
}

static int hinic3_init_ppf_work(struct hinic3_hwdev *hwdev)
{
	if (hinic3_ppf_idx(hwdev) != hinic3_global_func_id(hwdev))
		return 0;

	INIT_DELAYED_WORK(&hwdev->sync_time_task, hinic3_auto_sync_time_work);
	queue_delayed_work(hwdev->workq, &hwdev->sync_time_task,
			   msecs_to_jiffies(HINIC3_SYNFW_TIME_PERIOD));

	return 0;
}

static void hinic3_free_ppf_work(struct hinic3_hwdev *hwdev)
{
	if (hinic3_ppf_idx(hwdev) != hinic3_global_func_id(hwdev))
		return;

	cancel_delayed_work_sync(&hwdev->sync_time_task);
}

static DEFINE_IDA(hinic3_adev_ida);

static int hinic3_adev_idx_alloc(void)
{
	return ida_alloc(&hinic3_adev_ida, GFP_KERNEL);
}

static void hinic3_adev_idx_free(int id)
{
	ida_free(&hinic3_adev_ida, id);
}

static int init_hwdev(struct pci_dev *pdev)
{
	struct hinic3_pcidev *pci_adapter = pci_get_drvdata(pdev);
	struct hinic3_hwdev *hwdev;

	hwdev = kzalloc(sizeof(*hwdev), GFP_KERNEL);
	if (!hwdev)
		return -ENOMEM;

	pci_adapter->hwdev = hwdev;
	hwdev->adapter = pci_adapter;
	hwdev->pdev = pci_adapter->pdev;
	hwdev->dev = &pci_adapter->pdev->dev;
	hwdev->poll = false;
	hwdev->probe_fault_level = pci_adapter->probe_fault_level;
	hwdev->func_state = 0;
	memset(hwdev->features, 0, sizeof(hwdev->features));
	hwdev->dev_id = hinic3_adev_idx_alloc();

	spin_lock_init(&hwdev->channel_lock);

	return 0;
}

int hinic3_init_hwdev(struct pci_dev *pdev)
{
	struct hinic3_pcidev *pci_adapter = pci_get_drvdata(pdev);
	struct hinic3_hwdev *hwdev;
	int err;

	err = init_hwdev(pdev);
	if (err)
		return err;

	hwdev = pci_adapter->hwdev;
	err = hinic3_init_hwif(hwdev);
	if (err) {
		dev_err(hwdev->dev, "Failed to init hwif\n");
		goto err_init_hwif;
	}
	hwdev->chip_present_flag = HINIC3_CHIP_PRESENT;

	hwdev->workq = alloc_workqueue(HINIC3_HWDEV_WQ_NAME, WQ_MEM_RECLAIM, HINIC3_WQ_MAX_REQ);
	if (!hwdev->workq) {
		dev_err(hwdev->dev, "Failed to alloc hardware workq\n");
		goto err_alloc_workq;
	}

	err = hinic3_init_cfg_mgmt(hwdev);
	if (err) {
		dev_err(hwdev->dev, "Failed to init config mgmt\n");
		goto err_init_cfg_mgmt;
	}

	err = hinic3_init_comm_ch(hwdev);
	if (err) {
		dev_err(hwdev->dev, "Failed to init communication channel\n");
		goto err_init_comm_ch;
	}

	err = init_capability(hwdev);
	if (err) {
		dev_err(hwdev->dev, "Failed to init capability\n");
		goto err_init_cap;
	}

	hinic3_init_ppf_work(hwdev);

	err = hinic3_set_comm_features(hwdev, hwdev->features, COMM_MAX_FEATURE_QWORD);
	if (err) {
		dev_err(hwdev->dev, "Failed to set comm features\n");
		goto err_set_feature;
	}

	return 0;

err_set_feature:
	hinic3_free_ppf_work(hwdev);

err_init_cap:
	hinic3_uninit_comm_ch(hwdev);

err_init_comm_ch:
	hinic3_free_cfg_mgmt(hwdev);

err_init_cfg_mgmt:
	destroy_workqueue(hwdev->workq);

err_alloc_workq:
	hinic3_free_hwif(hwdev);

err_init_hwif:
	pci_adapter->probe_fault_level = hwdev->probe_fault_level;
	pci_adapter->hwdev = NULL;
	hinic3_adev_idx_free(hwdev->dev_id);
	kfree(hwdev);

	return -EFAULT;
}

void hinic3_free_hwdev(struct hinic3_hwdev *hwdev)
{
	u64 drv_features[COMM_MAX_FEATURE_QWORD];

	memset(drv_features, 0, sizeof(drv_features));
	hinic3_set_comm_features(hwdev, drv_features, COMM_MAX_FEATURE_QWORD);
	hinic3_free_ppf_work(hwdev);
	hinic3_func_rx_tx_flush(hwdev);
	hinic3_uninit_comm_ch(hwdev);
	hinic3_free_cfg_mgmt(hwdev);
	destroy_workqueue(hwdev->workq);
	hinic3_free_hwif(hwdev);
	hinic3_adev_idx_free(hwdev->dev_id);
	kfree(hwdev);
}

void hinic3_set_api_stop(struct hinic3_hwdev *hwdev)
{
	struct hinic3_recv_msg *recv_resp_msg;
	struct hinic3_mbox *mbox;

	hwdev->chip_present_flag = HINIC3_CHIP_ABSENT;
	spin_lock_bh(&hwdev->channel_lock);
	if (HINIC3_IS_PF(hwdev) &&
	    test_bit(HINIC3_HWDEV_MGMT_INITED, &hwdev->func_state)) {
		recv_resp_msg = &hwdev->pf_to_mgmt->recv_resp_msg_from_mgmt;
		spin_lock_bh(&hwdev->pf_to_mgmt->sync_event_lock);
		if (hwdev->pf_to_mgmt->event_flag == SEND_EVENT_START) {
			complete(&recv_resp_msg->recv_done);
			hwdev->pf_to_mgmt->event_flag = SEND_EVENT_TIMEOUT;
		}
		spin_unlock_bh(&hwdev->pf_to_mgmt->sync_event_lock);
	}

	if (test_bit(HINIC3_HWDEV_MBOX_INITED, &hwdev->func_state)) {
		mbox = hwdev->mbox;
		spin_lock(&mbox->mbox_lock);
		if (mbox->event_flag == EVENT_START)
			mbox->event_flag = EVENT_TIMEOUT;
		spin_unlock(&mbox->mbox_lock);
	}

	if (test_bit(HINIC3_HWDEV_CMDQ_INITED, &hwdev->func_state))
		hinic3_cmdq_flush_sync_cmd(hwdev);

	spin_unlock_bh(&hwdev->channel_lock);
}
