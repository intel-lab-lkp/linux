// SPDX-License-Identifier: GPL-2.0
/*
 * Driver for Versal PCIe device
 *
 * Copyright (C) 2025 Advanced Micro Devices, Inc. All rights reserved.
 */

#include <linux/pci.h>

#include "versal-pci.h"
#include "versal-pci-rm-service.h"
#include "versal-pci-rm-queue.h"

#define DRV_NAME			"amd-versal-pci"

#define PCI_DEVICE_ID_V70PQ2		0x50B0
#define PCI_DEVICE_ID_RAVE		0x5700
#define VERSAL_XCLBIN_MAGIC_ID		"xclbin2"

static inline u32 versal_pci_devid(struct versal_pci_device *vdev)
{
	return ((pci_domain_nr(vdev->pdev->bus) << 16) |
		PCI_DEVID(vdev->pdev->bus->number, vdev->pdev->devfn));
}

static int versal_pci_upload_fw(struct versal_pci_device *vdev,
				enum rm_queue_opcode opcode,
				const char *data,
				size_t size)
{
	struct rm_cmd *cmd;
	int ret;

	ret = rm_queue_create_cmd(vdev->rdev, opcode, &cmd);
	if (ret)
		return ret;

	ret = rm_queue_data_init(cmd, data, size);
	if (ret)
		goto done;

	ret = rm_queue_send_cmd(cmd, RM_CMD_WAIT_DOWNLOAD_TIMEOUT);

done:
	rm_queue_destroy_cmd(cmd);
	return ret;
}

static int versal_pci_load_shell(struct versal_pci_device *vdev, char *fw_name)
{
	const struct firmware *fw;
	struct axlf *xsabin;
	int ret;

	strim(fw_name);

	ret = request_firmware(&fw, fw_name, &vdev->pdev->dev);
	if (ret) {
		vdev_warn(vdev, "request xsabin fw %s failed %d", fw_name, ret);
		return ret;
	}

	xsabin = (struct axlf *)fw->data;
	if (memcmp(xsabin->magic, VERSAL_XCLBIN_MAGIC_ID, sizeof(VERSAL_XCLBIN_MAGIC_ID))) {
		vdev_err(vdev, "Invalid device firmware");
		ret = -EINVAL;
		goto release_firmware;
	}

	if (!fw->size ||
	    fw->size != xsabin->header.length ||
	    fw->size < sizeof(*xsabin) ||
	    fw->size > SZ_1G) {
		vdev_err(vdev, "Invalid device firmware size %zu", fw->size);
		ret = -EINVAL;
		goto release_firmware;
	}

	if (!uuid_equal(&vdev->intf_uuid, &xsabin->header.rom_uuid)) {
		vdev_err(vdev, "base shell doesn't match uuid %pUb", &xsabin->header.uuid);
		ret = -EINVAL;
		goto release_firmware;
	}

	ret = versal_pci_upload_fw(vdev, RM_QUEUE_OP_LOAD_FW,
				   (char *)xsabin, xsabin->header.length);
	if (ret) {
		vdev_err(vdev, "failed to load xsabin %s : %d", fw_name, ret);
		goto release_firmware;
	}

	vdev_info(vdev, "Downloaded xsabin %pUb of size %lld Bytes",
		  &xsabin->header.uuid, xsabin->header.length);

release_firmware:
	release_firmware(fw);

	return ret;
}

static inline struct versal_pci_device *item_to_vdev(struct config_item *item)
{
	return container_of(to_configfs_subsystem(to_config_group(item)),
			    struct versal_pci_device, cfs_subsys);
}

static ssize_t versal_pci_cfs_config_store(struct config_item *item,
					   const char *page, size_t count)
{
	struct versal_pci_device *vdev = item_to_vdev(item);
	u32 config;
	int ret;

	ret = kstrtou32(page, 0, &config);
	if (ret)
		return -EINVAL;

	if (config)
		ret = versal_pci_load_shell(vdev, vdev->fw.name);

	if (ret)
		return -EFAULT;

	return count;
}
CONFIGFS_ATTR_WO(versal_pci_cfs_, config);

static ssize_t versal_pci_cfs_image_show(struct config_item *item, char *page)
{
	struct versal_pci_device *vdev = item_to_vdev(item);

	vdev_info(vdev, "fw name: %s", vdev->fw.name);

	return 0;
}

static ssize_t versal_pci_cfs_image_store(struct config_item *item,
					  const char *page, size_t count)
{
	struct versal_pci_device *vdev = item_to_vdev(item);

	count = snprintf(vdev->fw.name, sizeof(vdev->fw.name), "%s", page);

	vdev_info(vdev, "fw name: %s", vdev->fw.name);
	return count;
}
CONFIGFS_ATTR(versal_pci_cfs_, image);

static struct configfs_attribute *versal_pci_cfs_attrs[] = {
	&versal_pci_cfs_attr_config,
	&versal_pci_cfs_attr_image,
	NULL,
};

static const struct config_item_type versal_pci_cfs_table = {
	.ct_owner = THIS_MODULE,
	.ct_attrs = versal_pci_cfs_attrs,
};

static int versal_pci_cfs_init(struct versal_pci_device *vdev)
{
	struct configfs_subsystem *subsys = &vdev->cfs_subsys;

	snprintf(subsys->su_group.cg_item.ci_namebuf,
		 sizeof(subsys->su_group.cg_item.ci_namebuf),
		 "%s%x", DRV_NAME, versal_pci_devid(vdev));

	subsys->su_group.cg_item.ci_type = &versal_pci_cfs_table;

	config_group_init(&subsys->su_group);
	return configfs_register_subsystem(subsys);
}

static void versal_pci_fw_fini(struct versal_pci_device *vdev)
{
	uuid_copy(&vdev->intf_uuid, &uuid_null);
}

static void versal_pci_cfs_fini(struct configfs_subsystem *subsys)
{
	configfs_unregister_subsystem(subsys);
}

static void versal_pci_device_teardown(struct versal_pci_device *vdev)
{
	versal_pci_cfs_fini(&vdev->cfs_subsys);
	versal_pci_fw_fini(vdev);
	versal_pci_rm_fini(vdev->rdev);
}

static void versal_pci_uuid_parse(struct versal_pci_device *vdev, uuid_t *uuid)
{
	char str[UUID_STRING_LEN];
	u8 i, j;
	int len = strlen(vdev->fw_id);

	/* parse uuid into a valid uuid string format */
	for (i = 0, j = 0; i < len && i < sizeof(str); i++) {
		str[j++] = vdev->fw_id[i];
		if (j == 8 || j == 13 || j == 18 || j == 23)
			str[j++] = '-';
	}

	uuid_parse(str, uuid);
	vdev_info(vdev, "Interface uuid %pU", uuid);
}

static int versal_pci_fw_init(struct versal_pci_device *vdev)
{
	int ret;

	ret = rm_queue_get_fw_id(vdev->rdev);
	if (ret) {
		vdev_warn(vdev, "Failed to get fw_id");
		return -EINVAL;
	}

	versal_pci_uuid_parse(vdev, &vdev->intf_uuid);

	return 0;
}

static int versal_pci_device_setup(struct versal_pci_device *vdev)
{
	int ret;

	vdev->rdev = versal_pci_rm_init(vdev);
	if (IS_ERR(vdev->rdev)) {
		ret = PTR_ERR(vdev->rdev);
		vdev_err(vdev, "Failed to init remote queue, err %d", ret);
		return ret;
	}

	ret = versal_pci_fw_init(vdev);
	if (ret) {
		vdev_err(vdev, "Failed to init fw, err %d", ret);
		goto comm_chan_fini;
	}

	ret = versal_pci_cfs_init(vdev);
	if (ret) {
		vdev_err(vdev, "Failed to init configfs subsys, err %d", ret);
		goto comm_chan_fini;
	}

	return 0;

comm_chan_fini:
	versal_pci_rm_fini(vdev->rdev);

	return ret;
}

static void versal_pci_remove(struct pci_dev *pdev)
{
	struct versal_pci_device *vdev = pci_get_drvdata(pdev);

	versal_pci_device_teardown(vdev);
}

static int versal_pci_probe(struct pci_dev *pdev, const struct pci_device_id *pdev_id)
{
	struct versal_pci_device *vdev;
	int ret;

	vdev = devm_kzalloc(&pdev->dev, sizeof(*vdev), GFP_KERNEL);
	if (!vdev)
		return -ENOMEM;

	pci_set_drvdata(pdev, vdev);
	vdev->pdev = pdev;

	ret = pcim_enable_device(pdev);
	if (ret) {
		vdev_err(vdev, "Failed to enable device %d", ret);
		return ret;
	}

	vdev->io_regs = pcim_iomap_region(vdev->pdev, MGMT_BAR, DRV_NAME);
	if (IS_ERR(vdev->io_regs)) {
		vdev_err(vdev, "Failed to map RM shared memory BAR%d", MGMT_BAR);
		return PTR_ERR(vdev->io_regs);
	}

	ret = versal_pci_device_setup(vdev);
	if (ret) {
		vdev_err(vdev, "Failed to setup Versal device %d", ret);
		return ret;
	}

	return 0;
}

static const struct pci_device_id versal_pci_ids[] = {
	{ PCI_DEVICE(PCI_VENDOR_ID_XILINX, PCI_DEVICE_ID_V70PQ2), },
	{ PCI_DEVICE(PCI_VENDOR_ID_XILINX, PCI_DEVICE_ID_RAVE), },
	{ 0 }
};

MODULE_DEVICE_TABLE(pci, versal_pci_ids);

static struct pci_driver versal_pci_driver = {
	.name = DRV_NAME,
	.id_table = versal_pci_ids,
	.probe = versal_pci_probe,
	.remove = versal_pci_remove,
};

module_pci_driver(versal_pci_driver);

MODULE_DESCRIPTION("AMD Versal PCIe Management Driver");
MODULE_AUTHOR("XRT Team <runtimeca39d@amd.com>");
MODULE_LICENSE("GPL");
