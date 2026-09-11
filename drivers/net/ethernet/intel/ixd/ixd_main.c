// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (C) 2025 Intel Corporation */

#include "ixd.h"
#include "ixd_ctlq.h"
#include "ixd_lan_regs.h"
#include "ixd_devlink.h"

MODULE_DESCRIPTION("Intel(R) Control Plane Function Device Driver");
MODULE_IMPORT_NS("LIBIE_CP");
MODULE_IMPORT_NS("LIBIE_PCI");
MODULE_LICENSE("GPL");

/**
 * ixd_remove - remove a CPF PCI device
 * @pdev: PCI device being removed
 */
static void ixd_remove(struct pci_dev *pdev)
{
	struct ixd_adapter *adapter = pci_get_drvdata(pdev);

	/* Do not mix removal with (re)initialization */
	cancel_delayed_work_sync(&adapter->init_task.init_work);

	ixd_devlink_unregister(adapter);

	/* Leave the device clean on exit */
	if (adapter->xnm)
		libie_ctlq_xn_shutdown(adapter->xnm);
	ixd_trigger_reset(adapter);
	ixd_deinit_dflt_mbx(adapter);

	libie_pci_unmap_all_mmio_regions(&adapter->cp_ctx.mmio_info);
	ixd_devlink_free(adapter);
}

/**
 * ixd_shutdown - shut down a CPF PCI device
 * @pdev: PCI device being shut down
 */
static void ixd_shutdown(struct pci_dev *pdev)
{
	ixd_remove(pdev);

	if (system_state == SYSTEM_POWER_OFF)
		pci_set_power_state(pdev, PCI_D3hot);
}

/**
 * ixd_iomap_regions - iomap PCI BARs
 * @mmio_info: PCI resources info
 * @num: number of regions to map
 * @regions: array of regions to map (offset and size)
 *
 * Returns: %0 on success, negative on failure
 */
static int ixd_iomap_regions(struct libie_mmio_info *mmio_info, int num,
			     const struct ixd_bar_region *regions)
{
	for (int i = 0; i < num; i++) {
		bool map_ok;

		map_ok = libie_pci_map_mmio_region(mmio_info,
						   regions[i].offset,
						   regions[i].size);
		if (!map_ok)
			return -EIO;
	}

	return 0;
}

/* Regions needed to reset the device and to talk to the control plane.
 * They are mapped in probe and stay mapped for the whole driver life.
 */
static const struct ixd_bar_region ixd_start_regions[] = {
	{
		.offset = PFGEN_RTRIG,
		.size = PFGEN_RTRIG_REG_LEN,
	},
	{
		.offset = PF_FW_MBX,
		.size = PF_FW_MBX_REG_LEN,
	},
};

/**
 * ixd_iomap_is_not_start_region - check if the region isn't a start region
 * @info: PCI resources info, unused
 * @reg: region to check
 *
 * Return: %true if it isn't a start region, %false otherwise
 */
bool ixd_iomap_is_not_start_region(struct libie_mmio_info *info,
				   struct libie_pci_mmio_region *reg)
{
	for (unsigned int i = 0; i < ARRAY_SIZE(ixd_start_regions); i++) {
		if (reg->bar_idx == 0 &&
		    reg->offset == ixd_start_regions[i].offset &&
		    reg->size == ixd_start_regions[i].size)
			return false;
	}

	return true;
}

/**
 * ixd_iomap_start_regions - iomap PCI BARs needed for driver startup
 * @adapter: adapter to map memory regions for
 *
 * Returns: %0 on success, negative on failure
 */
static int ixd_iomap_start_regions(struct ixd_adapter *adapter)
{
	struct libie_mmio_info *mmio_info = &adapter->cp_ctx.mmio_info;
	int err;

	err = ixd_iomap_regions(mmio_info, ARRAY_SIZE(ixd_start_regions),
				ixd_start_regions);
	if (err) {
		dev_err(ixd_to_dev(adapter),
			"Failed to map startup PCI device MMIO region\n");

		libie_pci_unmap_all_mmio_regions(mmio_info);
	}

	return err;
}

/**
 * ixd_iomap_running_regions - iomap PCI BARs needed for driver when running
 * @adapter: adapter to map memory regions for
 *
 * It should be called only when the GET_LAN_MEMORY_REGIONS virtchnl command
 * isn't supported. Calculate the offsets and sizes for the regions before,
 * in between, and after the start regions (mailbox and reset registers) and
 * map those ranges.
 *
 * Returns: %0 on success, negative on failure
 */
int ixd_iomap_running_regions(struct ixd_adapter *adapter)
{
	struct libie_mmio_info *mmio_info = &adapter->cp_ctx.mmio_info;
	resource_size_t start, size;
	bool ok = true;

	/* Region preceding the mailbox */
	size = PF_FW_MBX;
	ok &= !size || libie_pci_map_mmio_region(mmio_info, 0, size);

	/* Region between the mailbox and the reset registers */
	start = PF_FW_MBX + PF_FW_MBX_REG_LEN;
	size = PFGEN_RTRIG - start;
	ok &= !size || libie_pci_map_mmio_region(mmio_info, start, size);

	/* Region after the reset registers */
	start = PFGEN_RTRIG + PFGEN_RTRIG_REG_LEN;
	size = pci_resource_len(mmio_info->pdev, 0) - start;
	ok &= !size || libie_pci_map_mmio_region(mmio_info, start, size);

	if (!ok) {
		dev_err(ixd_to_dev(adapter),
			"Failed to map running PCI device MMIO region\n");

		libie_pci_unmap_fltr_regs(mmio_info,
					  ixd_iomap_is_not_start_region);

		return -EIO;
	}

	return 0;
}

/**
 * ixd_probe - probe a CPF PCI device
 * @pdev: corresponding PCI device
 * @ent: entry in ixd_pci_tbl
 *
 * Returns: %0 on success, negative errno code on failure
 */
static int ixd_probe(struct pci_dev *pdev, const struct pci_device_id *ent)
{
	struct ixd_adapter *adapter;
	int err;

	adapter = ixd_adapter_alloc(&pdev->dev);
	if (!adapter)
		return -ENOMEM;

	adapter->cp_ctx.mmio_info.pdev = pdev;
	INIT_LIST_HEAD(&adapter->cp_ctx.mmio_info.mmio_list);

	err = libie_pci_init_dev(pdev);
	if (err)
		goto free_adapter;

	pci_set_drvdata(pdev, adapter);

	err = ixd_iomap_start_regions(adapter);
	if (err)
		goto free_adapter;

	INIT_DELAYED_WORK(&adapter->init_task.init_work,
			  ixd_init_task);
	INIT_DELAYED_WORK(&adapter->mbx_task, ixd_ctlq_rx_task);

	ixd_trigger_reset(adapter);
	queue_delayed_work(system_dfl_wq, &adapter->init_task.init_work,
			   IXD_INIT_TASK_DELAY_JIFFIES);

	return 0;

free_adapter:
	ixd_devlink_free(adapter);
	return err;
}

static const struct pci_device_id ixd_pci_tbl[] = {
	{ PCI_VDEVICE(INTEL, IXD_DEV_ID_CPF) },
	{ }
};
MODULE_DEVICE_TABLE(pci, ixd_pci_tbl);

static struct pci_driver ixd_driver = {
	.name			= KBUILD_MODNAME,
	.id_table		= ixd_pci_tbl,
	.probe			= ixd_probe,
	.remove			= ixd_remove,
	.shutdown		= ixd_shutdown,
};
module_pci_driver(ixd_driver);
