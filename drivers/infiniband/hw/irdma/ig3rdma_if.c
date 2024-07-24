// SPDX-License-Identifier: GPL-2.0 or Linux-OpenIB
/* Copyright (c) 2023 - 2024 Intel Corporation */
#include "main.h"
#include "ig3rdma_hw.h"

static void ig3rdma_idc_core_event_handler(struct idc_rdma_core_dev_info *cdev_info,
					   struct idc_rdma_event *event)
{
	struct irdma_pci_f *rf = auxiliary_get_drvdata(cdev_info->adev);

	if (*event->type & BIT(IDC_RDMA_EVENT_WARN_RESET)) {
		rf->reset = true;
		rf->sc_dev.vchnl_up = false;
	}
}

static int ig3rdma_cfg_regions(struct irdma_hw *hw,
			       struct idc_rdma_core_dev_info *cdev_info)
{
	struct pci_dev *pdev = cdev_info->pdev;
	int i;

	switch (cdev_info->ftype) {
	case IDC_FUNCTION_TYPE_PF:
		hw->rdma_reg.len = IG3_PF_RDMA_REGION_LEN;
		hw->rdma_reg.offset = IG3_PF_RDMA_REGION_OFFSET;
		break;
	case IDC_FUNCTION_TYPE_VF:
		hw->rdma_reg.len = IG3_VF_RDMA_REGION_LEN;
		hw->rdma_reg.offset = IG3_VF_RDMA_REGION_OFFSET;
		break;
	default:
		return -ENODEV;
	}

	hw->rdma_reg.addr = ioremap(pci_resource_start(pdev, 0) + hw->rdma_reg.offset,
				    hw->rdma_reg.len);

	if (!hw->rdma_reg.addr)
		return -ENOMEM;

	hw->io_regs = kcalloc(cdev_info->num_memory_regions,
			      sizeof(struct irdma_mmio_region), GFP_KERNEL);

	if (!hw->io_regs) {
		iounmap(hw->rdma_reg.addr);
		return -ENOMEM;
	}

	hw->num_io_regions = le16_to_cpu(cdev_info->num_memory_regions);
	for (i = 0; i < cdev_info->num_memory_regions; i++) {
		hw->io_regs[i].addr =
			cdev_info->mapped_mem_regions[i].region_addr;
		hw->io_regs[i].len =
			cdev_info->mapped_mem_regions[i].size;
		hw->io_regs[i].offset =
			cdev_info->mapped_mem_regions[i].start_offset;
	}

	return 0;
}

static void ig3rdma_decfg_rf(struct irdma_pci_f *rf)
{
	struct irdma_hw *hw = &rf->hw;

	destroy_workqueue(rf->vchnl_wq);
	kfree(hw->io_regs);
	iounmap(hw->rdma_reg.addr);
}

static int ig3rdma_cfg_rf(struct irdma_pci_f *rf,
			  struct idc_rdma_core_dev_info *cdev_info)
{
	int err;

	rf->sc_dev.hw = &rf->hw;
	rf->cdev = cdev_info;
	rf->pcidev = cdev_info->pdev;
	rf->hw.device = &rf->pcidev->dev;
	rf->msix_count = cdev_info->msix_count;
	rf->msix_entries = cdev_info->msix_entries;

	err = irdma_vchnl_init(rf, cdev_info, &rf->rdma_ver);
	if (err)
		return err;

	err = ig3rdma_cfg_regions(&rf->hw, cdev_info);
	if (err) {
		destroy_workqueue(rf->vchnl_wq);
		return err;
	}

	rf->protocol_used = IRDMA_ROCE_PROTOCOL_ONLY;
	rf->rsrc_profile = IRDMA_HMC_PROFILE_DEFAULT;
	rf->rst_to = IRDMA_RST_TIMEOUT_HZ;
	rf->gen_ops.request_reset = irdma_request_reset;
	rf->limits_sel = 7;
	mutex_init(&rf->ah_tbl_lock);

	return 0;
}

static int ig3rdma_core_probe(struct auxiliary_device *aux_dev,
			      const struct auxiliary_device_id *id)
{
	struct idc_rdma_core_auxiliary_dev *idc_adev =
		container_of(aux_dev, struct idc_rdma_core_auxiliary_dev, adev);
	struct idc_rdma_core_dev_info *cdev_info = idc_adev->cdev_info;
	struct irdma_pci_f *rf;
	int err;

	rf = kzalloc(sizeof(*rf), GFP_KERNEL);
	if (!rf)
		return -ENOMEM;

	err = ig3rdma_cfg_rf(rf, cdev_info);
	if (err)
		goto err_cfg_rf;

	err = irdma_ctrl_init_hw(rf);
	if (err)
		goto err_ctrl_init;

	auxiliary_set_drvdata(aux_dev, rf);

	err = cdev_info->ops->vport_dev_ctrl(cdev_info, true);
	if (err)
		goto err_vport_ctrl;

	return 0;

err_vport_ctrl:
	irdma_ctrl_deinit_hw(rf);
err_ctrl_init:
	ig3rdma_decfg_rf(rf);
err_cfg_rf:
	kfree(rf);

	return err;
}

static void ig3rdma_core_remove(struct auxiliary_device *aux_dev)
{
	struct idc_rdma_core_auxiliary_dev *idc_adev =
		container_of(aux_dev, struct idc_rdma_core_auxiliary_dev, adev);
	struct idc_rdma_core_dev_info *cdev_info = idc_adev->cdev_info;
	struct irdma_pci_f *rf = auxiliary_get_drvdata(aux_dev);

	cdev_info->ops->vport_dev_ctrl(cdev_info, false);
	irdma_ctrl_deinit_hw(rf);
	ig3rdma_decfg_rf(rf);
	kfree(rf);
}

static const struct auxiliary_device_id ig3rdma_core_auxiliary_id_table[] = {
	{.name = "idpf.8086.rdma.core", },
	{},
};

MODULE_DEVICE_TABLE(auxiliary, ig3rdma_core_auxiliary_id_table);

struct idc_rdma_core_auxiliary_drv ig3rdma_core_auxiliary_drv = {
	.adrv = {
		.name = "core",
		.id_table = ig3rdma_core_auxiliary_id_table,
		.probe = ig3rdma_core_probe,
		.remove = ig3rdma_core_remove,
	},
	.event_handler = ig3rdma_idc_core_event_handler,
};