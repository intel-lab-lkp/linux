// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) Meta Platforms, Inc. and affiliates. */

#include <linux/unaligned.h>
#include <linux/pci.h>
#include <linux/pldmfw.h>
#include <linux/types.h>
#include <net/devlink.h>

#include "fbnic.h"

#define FBNIC_SN_STR_LEN	24

static int fbnic_version_running_put(struct devlink_info_req *req,
				     struct fbnic_fw_ver *fw_ver,
				     char *ver_name)
{
	char running_ver[FBNIC_FW_VER_MAX_SIZE];
	int err;

	fbnic_mk_fw_ver_str(fw_ver->version, running_ver);
	err = devlink_info_version_running_put(req, ver_name, running_ver);
	if (err)
		return err;

	if (strlen(fw_ver->commit) > 0) {
		char commit_name[FBNIC_SN_STR_LEN];

		snprintf(commit_name, FBNIC_SN_STR_LEN, "%s.commit", ver_name);
		err = devlink_info_version_running_put(req, commit_name,
						       fw_ver->commit);
		if (err)
			return err;
	}

	return 0;
}

static int fbnic_version_stored_put(struct devlink_info_req *req,
				    struct fbnic_fw_ver *fw_ver,
				    char *ver_name)
{
	char stored_ver[FBNIC_FW_VER_MAX_SIZE];
	int err;

	fbnic_mk_fw_ver_str(fw_ver->version, stored_ver);
	err = devlink_info_version_stored_put(req, ver_name, stored_ver);
	if (err)
		return err;

	if (strlen(fw_ver->commit) > 0) {
		char commit_name[FBNIC_SN_STR_LEN];

		snprintf(commit_name, FBNIC_SN_STR_LEN, "%s.commit", ver_name);
		err = devlink_info_version_stored_put(req, commit_name,
						      fw_ver->commit);
		if (err)
			return err;
	}

	return 0;
}

static int fbnic_devlink_info_get(struct devlink *devlink,
				  struct devlink_info_req *req,
				  struct netlink_ext_ack *extack)
{
	struct fbnic_dev *fbd = devlink_priv(devlink);
	int err;

	err = fbnic_version_running_put(req, &fbd->fw_cap.running.mgmt,
					DEVLINK_INFO_VERSION_GENERIC_FW);
	if (err)
		return err;

	err = fbnic_version_running_put(req, &fbd->fw_cap.running.bootloader,
					DEVLINK_INFO_VERSION_GENERIC_FW_BOOTLOADER);
	if (err)
		return err;

	err = fbnic_version_stored_put(req, &fbd->fw_cap.stored.mgmt,
				       DEVLINK_INFO_VERSION_GENERIC_FW);
	if (err)
		return err;

	err = fbnic_version_stored_put(req, &fbd->fw_cap.stored.bootloader,
				       DEVLINK_INFO_VERSION_GENERIC_FW_BOOTLOADER);
	if (err)
		return err;

	err = fbnic_version_stored_put(req, &fbd->fw_cap.stored.undi,
				       DEVLINK_INFO_VERSION_GENERIC_FW_UNDI);
	if (err)
		return err;

	if (fbd->dsn) {
		unsigned char serial[FBNIC_SN_STR_LEN];
		u8 dsn[8];

		put_unaligned_be64(fbd->dsn, dsn);
		err = snprintf(serial, FBNIC_SN_STR_LEN, "%8phD", dsn);
		if (err < 0)
			return err;

		err = devlink_info_serial_number_put(req, serial);
		if (err)
			return err;
	}

	return 0;
}

/**
 * fbnic_send_package_data - Send record package data to firmware
 * @context: PLDM FW update structure
 * @data: pointer to the package data
 * @length: length of the package data
 *
 * Send a copy of the package data associated with the PLDM record matching
 * this device to the firmware.
 *
 * Return: zero on success
 *	    negative error code on failure
 */
static int fbnic_send_package_data(struct pldmfw *context, const u8 *data,
				   u16 length)
{
	struct device *dev = context->dev;

	/* Temp placeholder required by devlink */
	dev_info(dev,
		 "Sending %u bytes of PLDM record package data to firmware\n",
		 length);

	return 0;
}

/**
 * fbnic_send_component_table - Send PLDM component table to the firmware
 * @context: PLDM FW update structure
 * @component: The component to send
 * @transfer_flag: Flag indication location in component tables
 *
 * Read relevant data from component table and forward it to the firmware.
 * Check response to verify if the firmware indicates that it wishes to
 * proceed with the update.
 *
 * Return: zero on success
 *	    negative error code on failure
 */
static int fbnic_send_component_table(struct pldmfw *context,
				      struct pldmfw_component *component,
				      u8 transfer_flag)
{
	struct device *dev = context->dev;
	u16 id = component->identifier;
	u8 test_string[80];

	switch (id) {
	case QSPI_SECTION_CMRT:
	case QSPI_SECTION_CONTROL_FW:
	case QSPI_SECTION_OPTION_ROM:
		break;
	default:
		dev_err(dev, "Unknown component ID %u\n", id);
		return -EINVAL;
	}

	dev_dbg(dev, "Sending PLDM component table to firmware\n");

	/* Temp placeholder */
	strscpy(test_string, component->version_string,
		min_t(u8, component->version_len, 79));
	dev_info(dev, "PLDMFW: Component ID: %u version %s\n",
		 id, test_string);

	return 0;
}

/**
 * fbnic_flash_component - Flash a component of the QSPI
 * @context: PLDM FW update structure
 * @component: The component table to send to FW
 *
 * Map contents of component and make it available for FW to download
 * so that it can update the contents of the QSPI Flash.
 *
 * Return: zero on success
 *	    negative error code on failure
 */
static int fbnic_flash_component(struct pldmfw *context,
				 struct pldmfw_component *component)
{
	const u8 *data = component->component_data;
	u32 size = component->component_size;
	struct fbnic_fw_completion *fw_cmpl;
	struct device *dev = context->dev;
	struct pci_dev *pdev = to_pci_dev(dev);
	u16 id = component->identifier;
	const char *component_name;
	int retries = 2;
	int err;

	struct devlink *devlink;
	struct fbnic_dev *fbd;

	switch (id) {
	case QSPI_SECTION_CMRT:
		component_name = "boot1";
		break;
	case QSPI_SECTION_CONTROL_FW:
		component_name = "boot2";
		break;
	case QSPI_SECTION_OPTION_ROM:
		component_name = "option-rom";
		break;
	default:
		dev_err(dev, "Unknown component ID %u\n", id);
		return -EINVAL;
	}

	fw_cmpl = kzalloc(sizeof(*fw_cmpl), GFP_KERNEL);
	if (!fw_cmpl)
		return -ENOMEM;

	pdev = to_pci_dev(dev);
	fbd = pci_get_drvdata(pdev);
	devlink = priv_to_devlink(fbd);

	/* Initialize completion and queue it for FW to process */
	fw_cmpl->msg_type = FBNIC_TLV_MSG_ID_FW_WRITE_CHUNK_REQ;
	init_completion(&fw_cmpl->done);

	fw_cmpl->fw_update.last_offset = 0;
	fw_cmpl->fw_update.data = data;
	fw_cmpl->fw_update.size = size;

	err = fbnic_fw_xmit_fw_start_upgrade(fbd, fw_cmpl, id, size);
	if (err)
		goto cmpl_free;

	/* Monitor completions and report status of update */
	while (fw_cmpl->fw_update.data) {
		u32 offset = fw_cmpl->fw_update.last_offset;

		devlink_flash_update_status_notify(devlink, "Flashing",
						   component_name, offset,
						   size);

		/* Allow 5 seconds for reply, resend and try up to 2 times */
		if (wait_for_completion_timeout(&fw_cmpl->done, 5 * HZ)) {
			reinit_completion(&fw_cmpl->done);
			/* If we receive a reply, reinit our retry counter */
			retries = 2;
		} else if (--retries == 0) {
			dev_err(fbd->dev, "Timed out waiting on update\n");
			err = -ETIMEDOUT;
			goto cmpl_cleanup;
		}
	}

	err = fw_cmpl->result;
	if (err)
		goto cmpl_cleanup;

	devlink_flash_update_status_notify(devlink, "Flashing",
					   component_name, size, size);

cmpl_cleanup:
	fbd->cmpl_data = NULL;
cmpl_free:
	kfree(fw_cmpl);

	return err;
}

/**
 * fbnic_finalize_update - Perform last steps to complete device update
 * @context: PLDM FW update structure
 *
 * Notify FW that update is complete and that it can take any actions
 * needed to finalize the FW update.
 *
 * Return: zero on success
 *	    negative error code on failure
 */
static int fbnic_finalize_update(struct pldmfw *context)
{
	struct device *dev = context->dev;

	/* Temp placeholder required by devlink */
	dev_info(dev, "PLDMFW: Finalize update\n");

	return 0;
}

static const struct pldmfw_ops fbnic_pldmfw_ops = {
	.match_record = pldmfw_op_pci_match_record,
	.send_package_data = fbnic_send_package_data,
	.send_component_table = fbnic_send_component_table,
	.flash_component = fbnic_flash_component,
	.finalize_update = fbnic_finalize_update,
};

static void fbnic_devlink_flash_update_report_err(struct fbnic_dev *fbd,
						  struct devlink *devlink,
						  const char *err_msg,
						  int err)
{
	char err_str[128];

	snprintf(err_str, sizeof(err_str),
		 "Failed to flash PLDM Image: %s (error: %d)",
		 err_msg, err);
	devlink_flash_update_status_notify(devlink, err_str, NULL, 0, 0);
	dev_err(fbd->dev, "%s\n", err_str);
}

static int
fbnic_devlink_flash_update(struct devlink *devlink,
			   struct devlink_flash_update_params *params,
			   struct netlink_ext_ack *extack)
{
	struct fbnic_dev *fbd = devlink_priv(devlink);
	const struct firmware *fw = params->fw;
	struct device *dev = fbd->dev;
	struct pldmfw context;
	char *err_msg;
	int err;

	if (!fw || !fw->data || !fw->size)
		return -EINVAL;

	devlink_flash_update_status_notify(devlink, "Preparing to flash",
					   NULL, 0, 0);

	context.ops = &fbnic_pldmfw_ops;
	context.dev = dev;

	err = pldmfw_flash_image(&context, fw);
	if (err) {
		switch (err) {
		case -EINVAL:
			err_msg = "Invalid image";
			break;
		case -EOPNOTSUPP:
			err_msg = "Unsupported image";
			break;
		case -ENOMEM:
			err_msg = "Out of memory";
			break;
		case -EFAULT:
			err_msg = "Invalid header";
			break;
		case -ENOENT:
			err_msg = "No matching record";
			break;
		case -ENODEV:
			err_msg = "No matching device";
			break;
		case -ETIMEDOUT:
			err_msg = "Timed out waiting for reply";
			break;
		default:
			err_msg = "Unknown error";
			break;
		}
		fbnic_devlink_flash_update_report_err(fbd, devlink,
						      err_msg, err);
	} else {
		devlink_flash_update_status_notify(devlink, "Flashing done",
						   NULL, 0, 0);
	}

	return err;
}

static const struct devlink_ops fbnic_devlink_ops = {
	.info_get	= fbnic_devlink_info_get,
	.flash_update	= fbnic_devlink_flash_update,
};

void fbnic_devlink_free(struct fbnic_dev *fbd)
{
	struct devlink *devlink = priv_to_devlink(fbd);

	devlink_free(devlink);
}

struct fbnic_dev *fbnic_devlink_alloc(struct pci_dev *pdev)
{
	void __iomem * const *iomap_table;
	struct devlink *devlink;
	struct fbnic_dev *fbd;

	devlink = devlink_alloc(&fbnic_devlink_ops, sizeof(struct fbnic_dev),
				&pdev->dev);
	if (!devlink)
		return NULL;

	fbd = devlink_priv(devlink);
	pci_set_drvdata(pdev, fbd);
	fbd->dev = &pdev->dev;

	iomap_table = pcim_iomap_table(pdev);
	fbd->uc_addr0 = iomap_table[0];
	fbd->uc_addr4 = iomap_table[4];

	fbd->dsn = pci_get_dsn(pdev);
	fbd->mps = pcie_get_mps(pdev);
	fbd->readrq = pcie_get_readrq(pdev);

	fbd->mac_addr_boundary = FBNIC_RPC_TCAM_MACDA_DEFAULT_BOUNDARY;

	return fbd;
}

void fbnic_devlink_register(struct fbnic_dev *fbd)
{
	struct devlink *devlink = priv_to_devlink(fbd);

	devlink_register(devlink);
}

void fbnic_devlink_unregister(struct fbnic_dev *fbd)
{
	struct devlink *devlink = priv_to_devlink(fbd);

	devlink_unregister(devlink);
}
