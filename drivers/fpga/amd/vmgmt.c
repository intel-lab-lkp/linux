// SPDX-License-Identifier: GPL-2.0
/*
 * Driver for Versal PCIe device
 *
 * Copyright (C) 2024 Advanced Micro Devices, Inc. All rights reserved.
 */

#include <linux/cdev.h>
#include <linux/device/class.h>
#include <linux/err.h>
#include <linux/firmware.h>
#include <linux/fs.h>
#include <linux/fpga/fpga-mgr.h>
#include <linux/idr.h>
#include <linux/kdev_t.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/types.h>
#include <linux/uuid.h>
#include <linux/vmgmt.h>

#include "vmgmt.h"
#include "vmgmt-comms.h"

#define DRV_NAME			"amd-vmgmt"
#define CLASS_NAME			DRV_NAME

#define PCI_DEVICE_ID_V70PQ2		0x50B0
#define VERSAL_XCLBIN_MAGIC_ID		"xclbin2"

static DEFINE_IDA(vmgmt_dev_minor_ida);
static dev_t vmgmt_devnode;
struct class *vmgmt_class;
static struct fpga_bridge_ops vmgmt_br_ops;

struct vmgmt_fpga_region {
	struct fpga_device *fdev;
	uuid_t *uuid;
};

static inline struct vmgmt_device *vmgmt_inode_to_vdev(struct inode *inode)
{
	return (struct vmgmt_device *)container_of(inode->i_cdev, struct vmgmt_device, cdev);
}

static enum fpga_mgr_states vmgmt_fpga_state(struct fpga_manager *mgr)
{
	struct fpga_device *fdev = mgr->priv;

	return fdev->state;
}

static const struct fpga_manager_ops vmgmt_fpga_ops = {
	.state = vmgmt_fpga_state,
};

static int vmgmt_get_bridges(struct fpga_region *region)
{
	struct fpga_device *fdev = region->priv;

	return fpga_bridge_get_to_list(&fdev->vdev->pdev->dev, region->info,
				       &region->bridge_list);
}

static void vmgmt_fpga_fini(struct fpga_device *fdev)
{
	fpga_region_unregister(fdev->region);
	fpga_bridge_unregister(fdev->bridge);
	fpga_mgr_unregister(fdev->mgr);
}

static struct fpga_device *vmgmt_fpga_init(struct vmgmt_device *vdev)
{
	struct device *dev = &vdev->pdev->dev;
	struct fpga_region_info region = { 0 };
	struct fpga_manager_info info = { 0 };
	struct fpga_device *fdev;
	int ret;

	fdev = devm_kzalloc(dev, sizeof(*fdev), GFP_KERNEL);
	if (!fdev)
		return ERR_PTR(-ENOMEM);

	fdev->vdev = vdev;

	info = (struct fpga_manager_info) {
		.name = "AMD Versal FPGA Manager",
		.mops = &vmgmt_fpga_ops,
		.priv = fdev,
	};

	fdev->mgr = fpga_mgr_register_full(dev, &info);
	if (IS_ERR(fdev->mgr)) {
		ret = PTR_ERR(fdev->mgr);
		vmgmt_err(vdev, "Failed to register FPGA manager, err %d", ret);
		return ERR_PTR(ret);
	}

	/* create fgpa bridge, region for the base shell */
	fdev->bridge = fpga_bridge_register(dev, "AMD Versal FPGA Bridge",
					    &vmgmt_br_ops, fdev);
	if (IS_ERR(fdev->bridge)) {
		vmgmt_err(vdev, "Failed to register FPGA bridge, err %ld",
			  PTR_ERR(fdev->bridge));
		ret = PTR_ERR(fdev->bridge);
		goto unregister_fpga_mgr;
	}

	region = (struct fpga_region_info) {
		.compat_id = (struct fpga_compat_id *)&vdev->intf_uuid,
		.get_bridges = vmgmt_get_bridges,
		.mgr = fdev->mgr,
		.priv = fdev,
	};

	fdev->region = fpga_region_register_full(dev, &region);
	if (IS_ERR(fdev->region)) {
		vmgmt_err(vdev, "Failed to register FPGA region, err %ld",
			  PTR_ERR(fdev->region));
		ret = PTR_ERR(fdev->region);
		goto unregister_fpga_bridge;
	}

	return fdev;

unregister_fpga_bridge:
	fpga_bridge_unregister(fdev->bridge);

unregister_fpga_mgr:
	fpga_mgr_unregister(fdev->mgr);

	return ERR_PTR(ret);
}

static int vmgmt_open(struct inode *inode, struct file *filep)
{
	struct vmgmt_device *vdev = vmgmt_inode_to_vdev(inode);

	if (WARN_ON(!vdev))
		return -ENODEV;

	filep->private_data = vdev;

	return 0;
}

static int vmgmt_release(struct inode *inode, struct file *filep)
{
	filep->private_data = NULL;

	return 0;
}

static const struct file_operations vmgmt_fops = {
	.owner = THIS_MODULE,
	.open = vmgmt_open,
	.release = vmgmt_release,
};

static void vmgmt_chrdev_destroy(struct vmgmt_device *vdev)
{
	device_destroy(vmgmt_class, vdev->cdev.dev);
	cdev_del(&vdev->cdev);
	ida_free(&vmgmt_dev_minor_ida, vdev->minor);
}

static int vmgmt_chrdev_create(struct vmgmt_device *vdev)
{
	u32 devid;
	int ret;

	vdev->minor = ida_alloc(&vmgmt_dev_minor_ida, GFP_KERNEL);
	if (vdev->minor < 0) {
		vmgmt_err(vdev, "Failed to allocate chrdev ID");
		return -ENODEV;
	}

	cdev_init(&vdev->cdev, &vmgmt_fops);

	vdev->cdev.owner = THIS_MODULE;
	vdev->cdev.dev = MKDEV(MAJOR(vmgmt_devnode), vdev->minor);
	ret = cdev_add(&vdev->cdev, vdev->cdev.dev, 1);
	if (ret) {
		vmgmt_err(vdev, "Failed to add char device: %d\n", ret);
		ida_free(&vmgmt_dev_minor_ida, vdev->minor);
		return -ENODEV;
	}

	devid = PCI_DEVID(vdev->pdev->bus->number, vdev->pdev->devfn);
	vdev->device = device_create(vmgmt_class, &vdev->pdev->dev,
				     vdev->cdev.dev, NULL, "%s%x", DRV_NAME,
				     devid);
	if (IS_ERR(vdev->device)) {
		vmgmt_err(vdev, "Failed to create device: %ld\n",
			  PTR_ERR(vdev->device));
		cdev_del(&vdev->cdev);
		ida_free(&vmgmt_dev_minor_ida, vdev->minor);
		return -ENODEV;
	}

	return 0;
}

static void vmgmt_fw_cancel(struct fw_upload *fw_upload)
{
	struct firmware_device *fwdev = fw_upload->dd_handle;

	vmgmt_warn(fwdev->vdev, "canceled");
}

static const struct fw_upload_ops vmgmt_fw_ops = {
	.cancel = vmgmt_fw_cancel,
};

static void vmgmt_fw_upload_fini(struct firmware_device *fwdev)
{
	firmware_upload_unregister(fwdev->fw);
	kfree(fwdev->name);
}

static struct firmware_device *vmgmt_fw_upload_init(struct vmgmt_device *vdev)
{
	struct device *dev = &vdev->pdev->dev;
	struct firmware_device *fwdev;
	u32 devid;

	fwdev = devm_kzalloc(dev, sizeof(*fwdev), GFP_KERNEL);
	if (!fwdev)
		return ERR_PTR(-ENOMEM);

	devid = PCI_DEVID(vdev->pdev->bus->number, vdev->pdev->devfn);
	fwdev->name = kasprintf(GFP_KERNEL, "%s%x", DRV_NAME, devid);
	if (!fwdev->name)
		return ERR_PTR(-ENOMEM);

	fwdev->fw = firmware_upload_register(THIS_MODULE, dev, fwdev->name,
					     &vmgmt_fw_ops, fwdev);
	if (IS_ERR(fwdev->fw)) {
		kfree(fwdev->name);
		return ERR_CAST(fwdev->fw);
	}

	fwdev->vdev = vdev;

	return fwdev;
}

static void vmgmt_device_teardown(struct vmgmt_device *vdev)
{
	vmgmt_fpga_fini(vdev->fdev);
	vmgmt_fw_upload_fini(vdev->fwdev);
	vmgmtm_comms_fini(vdev->ccdev);
}

static int vmgmt_device_setup(struct vmgmt_device *vdev)
{
	int ret;

	vdev->fwdev = vmgmt_fw_upload_init(vdev);
	if (IS_ERR(vdev->fwdev)) {
		ret = PTR_ERR(vdev->fwdev);
		vmgmt_err(vdev, "Failed to init FW uploader, err %d", ret);
		goto done;
	}

	vdev->ccdev = vmgmtm_comms_init(vdev);
	if (IS_ERR(vdev->ccdev)) {
		ret = PTR_ERR(vdev->ccdev);
		vmgmt_err(vdev, "Failed to init comms channel, err %d", ret);
		goto upload_fini;
	}

	vdev->fdev = vmgmt_fpga_init(vdev);
	if (IS_ERR(vdev->fdev)) {
		ret = PTR_ERR(vdev->fdev);
		vmgmt_err(vdev, "Failed to init FPGA maanger, err %d", ret);
		goto comms_fini;
	}

	return 0;
comms_fini:
	vmgmtm_comms_fini(vdev->ccdev);
upload_fini:
	vmgmt_fw_upload_fini(vdev->fwdev);
done:
	return ret;
}

static void vmgmt_remove(struct pci_dev *pdev)
{
	struct vmgmt_device *vdev = pci_get_drvdata(pdev);

	vmgmt_chrdev_destroy(vdev);
	vmgmt_device_teardown(vdev);
}

static int vmgmt_probe(struct pci_dev *pdev,
		       const struct pci_device_id *pdev_id)
{
	struct vmgmt_device *vdev;
	int ret;

	vdev = devm_kzalloc(&pdev->dev, sizeof(*vdev), GFP_KERNEL);
	if (!vdev)
		return -ENOMEM;

	pci_set_drvdata(pdev, vdev);
	vdev->pdev = pdev;

	ret = pcim_enable_device(pdev);
	if (ret) {
		vmgmt_err(vdev, "Failed to enable device %d", ret);
		return ret;
	}

	ret = pcim_iomap_regions(vdev->pdev, AMD_VMGMT_BAR_MASK, "amd-vmgmt");
	if (ret) {
		vmgmt_err(vdev, "Failed iomap regions %d", ret);
		return -ENOMEM;
	}

	vdev->tbl = pcim_iomap_table(vdev->pdev)[AMD_VMGMT_BAR];
	if (IS_ERR(vdev->tbl)) {
		vmgmt_err(vdev, "Failed to map RM shared memory BAR%d", AMD_VMGMT_BAR);
		return -ENOMEM;
	}

	ret = vmgmt_device_setup(vdev);
	if (ret) {
		vmgmt_err(vdev, "Failed to setup Versal device %d", ret);
		return ret;
	}

	ret = vmgmt_chrdev_create(vdev);
	if (ret) {
		vmgmt_device_teardown(vdev);
		return ret;
	}

	vmgmt_dbg(vdev, "Successfully probed %s driver!", DRV_NAME);
	return 0;
}

static const struct pci_device_id vmgmt_pci_ids[] = {
	{ PCI_DEVICE(PCI_VENDOR_ID_XILINX, PCI_DEVICE_ID_V70PQ2), },
	{ 0 }
};

MODULE_DEVICE_TABLE(pci, vmgmt_pci_ids);

static struct pci_driver amd_vmgmt_driver = {
	.name = DRV_NAME,
	.id_table = vmgmt_pci_ids,
	.probe = vmgmt_probe,
	.remove = vmgmt_remove,
};

static int amd_vmgmt_init(void)
{
	int ret;

	vmgmt_class = class_create(CLASS_NAME);
	if (IS_ERR(vmgmt_class))
		return PTR_ERR(vmgmt_class);

	ret = alloc_chrdev_region(&vmgmt_devnode, 0, MINORMASK, DRV_NAME);
	if (ret)
		goto chr_err;

	ret = pci_register_driver(&amd_vmgmt_driver);
	if (ret)
		goto pci_err;

	return 0;

pci_err:
	unregister_chrdev_region(vmgmt_devnode, MINORMASK);
chr_err:
	class_destroy(vmgmt_class);
	return ret;
}

static void amd_vmgmt_exit(void)
{
	pci_unregister_driver(&amd_vmgmt_driver);
	unregister_chrdev_region(vmgmt_devnode, MINORMASK);
	class_destroy(vmgmt_class);
}

module_init(amd_vmgmt_init);
module_exit(amd_vmgmt_exit);

MODULE_DESCRIPTION("AMD PCIe Versal Management Driver");
MODULE_AUTHOR("XRT Team <runtimeca39d@amd.com>");
MODULE_LICENSE("GPL");
