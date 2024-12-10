// SPDX-License-Identifier: GPL-2.0
/*
 * Driver for Versal PCIe device
 *
 * Copyright (C) 2024 Advanced Micro Devices, Inc. All rights reserved.
 */

#include <linux/pci.h>

#include "versal-pci.h"
#include "versal-pci-comm-chan.h"
#include "versal-pci-rm-service.h"
#include "versal-pci-rm-queue.h"

#define DRV_NAME			"amd-versal-pci"

#define PCI_DEVICE_ID_V70PQ2		0x50B0
#define VERSAL_XCLBIN_MAGIC_ID		"xclbin2"

static int versal_pci_fpga_write_init(struct fpga_manager *mgr, struct fpga_image_info *info,
				      const char *buf, size_t count)
{
	struct fpga_device *fdev = mgr->priv;
	struct fw_tnx *tnx = &fdev->fw;
	int ret;

	ret = rm_queue_create_cmd(fdev->vdev->rdev, tnx->opcode, &tnx->cmd);
	if (ret) {
		fdev->state = FPGA_MGR_STATE_WRITE_INIT_ERR;
		return ret;
	}

	fdev->state = FPGA_MGR_STATE_WRITE_INIT;
	return ret;
}

static int versal_pci_fpga_write(struct fpga_manager *mgr, const char *buf,
				 size_t count)
{
	struct fpga_device *fdev = mgr->priv;
	int ret;

	ret = rm_queue_data_init(fdev->fw.cmd, buf, count);
	if (ret) {
		fdev->state = FPGA_MGR_STATE_WRITE_ERR;
		rm_queue_destory_cmd(fdev->fw.cmd);
		return ret;
	}

	fdev->state = FPGA_MGR_STATE_WRITE;
	return ret;
}

static int versal_pci_fpga_write_complete(struct fpga_manager *mgr,
					  struct fpga_image_info *info)
{
	struct fpga_device *fdev = mgr->priv;
	int ret;

	ret = rm_queue_send_cmd(fdev->fw.cmd, RM_CMD_WAIT_DOWNLOAD_TIMEOUT);
	if (ret) {
		fdev->state = FPGA_MGR_STATE_WRITE_COMPLETE_ERR;
		vdev_err(fdev->vdev, "Send cmd failed:%d, cid:%d", ret, fdev->fw.id);
	} else {
		fdev->state = FPGA_MGR_STATE_WRITE_COMPLETE;
	}

	rm_queue_data_fini(fdev->fw.cmd);
	rm_queue_destory_cmd(fdev->fw.cmd);
	memset(&fdev->fw, 0, sizeof(fdev->fw));
	return ret;
}

static enum fpga_mgr_states versal_pci_fpga_state(struct fpga_manager *mgr)
{
	struct fpga_device *fdev = mgr->priv;

	return fdev->state;
}

static const struct fpga_manager_ops versal_pci_fpga_ops = {
	.write_init = versal_pci_fpga_write_init,
	.write = versal_pci_fpga_write,
	.write_complete = versal_pci_fpga_write_complete,
	.state = versal_pci_fpga_state,
};

static void versal_pci_fpga_fini(struct fpga_device *fdev)
{
	fpga_mgr_unregister(fdev->mgr);
}

static void versal_pci_uuid_parse(struct versal_pci_device *vdev, uuid_t *uuid)
{
	char str[UUID_STRING_LEN];
	u8 i, j;

	/* parse uuid into a valid uuid string format */
	for (i  = 0, j = 0; i < strlen(vdev->fw_id) && i < sizeof(str); i++) {
		str[j++] = vdev->fw_id[i];
		if (j == 8 || j == 13 || j == 18 || j == 23)
			str[j++] = '-';
	}

	uuid_parse(str, uuid);
	vdev_info(vdev, "Interface uuid %pU", uuid);
}

static struct fpga_device *versal_pci_fpga_init(struct versal_pci_device *vdev)
{
	struct device *dev = &vdev->pdev->dev;
	struct fpga_manager_info info = { 0 };
	struct fpga_device *fdev;
	int ret;

	fdev = devm_kzalloc(dev, sizeof(*fdev), GFP_KERNEL);
	if (!fdev)
		return ERR_PTR(-ENOMEM);

	fdev->vdev = vdev;

	info = (struct fpga_manager_info) {
		.name = "AMD Versal FPGA Manager",
		.mops = &versal_pci_fpga_ops,
		.priv = fdev,
	};

	fdev->mgr = fpga_mgr_register_full(dev, &info);
	if (IS_ERR(fdev->mgr)) {
		ret = PTR_ERR(fdev->mgr);
		vdev_err(vdev, "Failed to register FPGA manager, err %d", ret);
		return ERR_PTR(ret);
	}

	ret = rm_queue_get_fw_id(vdev->rdev);
	if (ret) {
		vdev_warn(vdev, "Failed to get fw_id");
		ret = -EINVAL;
		goto unregister_fpga_mgr;
	}
	versal_pci_uuid_parse(vdev, &vdev->intf_uuid);

	return fdev;

unregister_fpga_mgr:
	fpga_mgr_unregister(fdev->mgr);

	return ERR_PTR(ret);
}

static int versal_pci_program_axlf(struct versal_pci_device *vdev, char *data, size_t size)
{
	const struct axlf *axlf = (struct axlf *)data;
	struct fpga_image_info *image_info;
	int ret;

	image_info = fpga_image_info_alloc(&vdev->pdev->dev);
	if (!image_info)
		return -ENOMEM;

	image_info->count = axlf->header.length;
	image_info->buf = (char *)axlf;

	ret = fpga_mgr_load(vdev->fdev->mgr, image_info);
	if (ret) {
		vdev_err(vdev, "failed to load xclbin: %d", ret);
		goto exit;
	}

	vdev_info(vdev, "Downloaded axlf %pUb of size %zu Bytes", &axlf->header.uuid, size);
	uuid_copy(&vdev->xclbin_uuid, &axlf->header.uuid);

exit:
	fpga_image_info_free(image_info);

	return ret;
}

int versal_pci_load_xclbin(struct versal_pci_device *vdev, uuid_t *xuuid)
{
	const char *xclbin_location = "xilinx/xclbins";
	char fw_name[100];
	const struct firmware *fw;
	int ret;

	snprintf(fw_name, sizeof(fw_name), "%s/%pUb_%s.xclbin",
		 xclbin_location, xuuid, vdev->fw_id);

	vdev_info(vdev, "trying to load %s", fw_name);
	ret = request_firmware(&fw, fw_name, &vdev->pdev->dev);
	if (ret) {
		vdev_warn(vdev, "request xclbin fw %s failed %d", fw_name, ret);
		return ret;
	}
	vdev_info(vdev, "loaded data size %zu", fw->size);

	ret = versal_pci_program_axlf(vdev, (char *)fw->data, fw->size);
	if (ret)
		vdev_err(vdev, "program axlf %s failed %d", fw_name, ret);

	release_firmware(fw);

	return ret;
}

static enum fw_upload_err versal_pci_fw_prepare(struct fw_upload *fw_upload, const u8 *data,
						u32 size)
{
	struct firmware_device *fwdev = fw_upload->dd_handle;
	struct axlf *xsabin = (struct axlf *)data;
	int ret;

	ret = memcmp(xsabin->magic, VERSAL_XCLBIN_MAGIC_ID, sizeof(VERSAL_XCLBIN_MAGIC_ID));
	if (ret) {
		vdev_err(fwdev->vdev, "Invalid device firmware");
		return FW_UPLOAD_ERR_INVALID_SIZE;
	}

	/* Firmware size should never be over 1G and less than size of struct axlf */
	if (!size || size != xsabin->header.length || size < sizeof(*xsabin) ||
	    size > 1024 * 1024 * 1024) {
		vdev_err(fwdev->vdev, "Invalid device firmware size");
		return FW_UPLOAD_ERR_INVALID_SIZE;
	}

	ret = rm_queue_create_cmd(fwdev->vdev->rdev, RM_QUEUE_OP_LOAD_FW,
				  &fwdev->cmd);
	if (ret)
		return FW_UPLOAD_ERR_RW_ERROR;

	uuid_copy(&fwdev->uuid, &xsabin->header.uuid);
	return FW_UPLOAD_ERR_NONE;
}

static enum fw_upload_err versal_pci_fw_write(struct fw_upload *fw_upload, const u8 *data,
					      u32 offset, u32 size, u32 *written)
{
	struct firmware_device *fwdev = fw_upload->dd_handle;
	int ret;

	ret = rm_queue_data_init(fwdev->cmd, data, size);
	if (ret)
		return FW_UPLOAD_ERR_RW_ERROR;

	*written = size;
	return FW_UPLOAD_ERR_NONE;
}

static enum fw_upload_err versal_pci_fw_poll_complete(struct fw_upload *fw_upload)
{
	struct firmware_device *fwdev = fw_upload->dd_handle;
	int ret;

	vdev_info(fwdev->vdev, "Programming device firmware: %pUb", &fwdev->uuid);

	ret = rm_queue_send_cmd(fwdev->cmd, RM_CMD_WAIT_DOWNLOAD_TIMEOUT);
	if (ret) {
		vdev_err(fwdev->vdev, "Send cmd failedi:%d, cid %d", ret, fwdev->id);
		return FW_UPLOAD_ERR_HW_ERROR;
	}

	vdev_info(fwdev->vdev, "Successfully programmed device firmware: %pUb",
		  &fwdev->uuid);
	return FW_UPLOAD_ERR_NONE;
}

static void versal_pci_fw_cancel(struct fw_upload *fw_upload)
{
	struct firmware_device *fwdev = fw_upload->dd_handle;

	vdev_warn(fwdev->vdev, "canceled");
	rm_queue_withdraw_cmd(fwdev->cmd);
}

static void versal_pci_fw_cleanup(struct fw_upload *fw_upload)
{
	struct firmware_device *fwdev = fw_upload->dd_handle;

	if (!fwdev->cmd)
		return;

	rm_queue_data_fini(fwdev->cmd);
	rm_queue_destory_cmd(fwdev->cmd);

	fwdev->cmd = NULL;
	fwdev->id = 0;
}

static const struct fw_upload_ops versal_pci_fw_ops = {
	.prepare = versal_pci_fw_prepare,
	.write = versal_pci_fw_write,
	.poll_complete = versal_pci_fw_poll_complete,
	.cancel = versal_pci_fw_cancel,
	.cleanup = versal_pci_fw_cleanup,
};

static void versal_pci_fw_upload_fini(struct firmware_device *fwdev)
{
	firmware_upload_unregister(fwdev->fw);
	kfree(fwdev->name);
}

static u32 versal_pci_devid(struct versal_pci_device *vdev)
{
	return ((pci_domain_nr(vdev->pdev->bus) << 16) |
		PCI_DEVID(vdev->pdev->bus->number, vdev->pdev->devfn));
}

static struct firmware_device *versal_pci_fw_upload_init(struct versal_pci_device *vdev)
{
	struct device *dev = &vdev->pdev->dev;
	struct firmware_device *fwdev;
	u32 devid;

	fwdev = devm_kzalloc(dev, sizeof(*fwdev), GFP_KERNEL);
	if (!fwdev)
		return ERR_PTR(-ENOMEM);

	devid = versal_pci_devid(vdev);
	fwdev->name = kasprintf(GFP_KERNEL, "%s%x", DRV_NAME, devid);
	if (!fwdev->name)
		return ERR_PTR(-ENOMEM);

	fwdev->fw = firmware_upload_register(THIS_MODULE, dev, fwdev->name,
					     &versal_pci_fw_ops, fwdev);
	if (IS_ERR(fwdev->fw)) {
		kfree(fwdev->name);
		return ERR_CAST(fwdev->fw);
	}

	fwdev->vdev = vdev;

	return fwdev;
}

static void versal_pci_device_teardown(struct versal_pci_device *vdev)
{
	versal_pci_fpga_fini(vdev->fdev);
	versal_pci_fw_upload_fini(vdev->fwdev);
	versal_pci_comm_chan_fini(vdev->ccdev);
	versal_pci_rm_fini(vdev->rdev);
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

	vdev->fwdev = versal_pci_fw_upload_init(vdev);
	if (IS_ERR(vdev->fwdev)) {
		ret = PTR_ERR(vdev->fwdev);
		vdev_err(vdev, "Failed to init FW uploader, err %d", ret);
		goto rm_fini;
	}

	vdev->ccdev = versal_pci_comm_chan_init(vdev);
	if (IS_ERR(vdev->ccdev)) {
		ret = PTR_ERR(vdev->ccdev);
		vdev_err(vdev, "Failed to init comm channel, err %d", ret);
		goto upload_fini;
	}

	vdev->fdev = versal_pci_fpga_init(vdev);
	if (IS_ERR(vdev->fdev)) {
		ret = PTR_ERR(vdev->fdev);
		vdev_err(vdev, "Failed to init FPGA manager, err %d", ret);
		goto comm_chan_fini;
	}

	return 0;
comm_chan_fini:
	versal_pci_comm_chan_fini(vdev->ccdev);
upload_fini:
	versal_pci_fw_upload_fini(vdev->fwdev);
rm_fini:
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

	vdev_dbg(vdev, "Successfully probed %s driver!", DRV_NAME);
	return 0;
}

static const struct pci_device_id versal_pci_ids[] = {
	{ PCI_DEVICE(PCI_VENDOR_ID_XILINX, PCI_DEVICE_ID_V70PQ2), },
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
