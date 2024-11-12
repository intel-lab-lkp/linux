// SPDX-License-Identifier: GPL-2.0-only
/*
 * AMD Secure Encrypted Virtualization (SEV) firmware upload API
 */

#include <linux/firmware.h>
#include <linux/psp.h>
#include <linux/psp-sev.h>

#include <asm/sev.h>

#include "sev-dev.h"

static bool synthetic_restore_required;

int sev_snp_synthetic_error(struct sev_device *sev, int *psp_ret)
{
	if (synthetic_restore_required) {
		*psp_ret = SEV_RET_RESTORE_REQUIRED;
		return -EIO;
	}

	return 0;
}

static int sev_snp_download_firmware_ex(struct sev_device *sev, const u8 *data, u32 size,
					int *error)
{
	struct sev_data_download_firmware_ex *data_ex;
	int ret, order;
	struct page *p;
	u64 data_size;
	void *fw_dest;

	data_size = ALIGN(sizeof(struct sev_data_download_firmware_ex), SEV_FW_ALIGNMENT);

	order = get_order(size + data_size);
	p = alloc_pages(GFP_KERNEL, order);
	if (!p)
		return -ENOMEM;

	/*
	 * Copy firmware data to a kernel allocated contiguous
	 * memory region.
	 */
	data_ex = page_address(p);
	fw_dest = page_address(p) + data_size;
	memset(data_ex, 0, data_size);
	memcpy(fw_dest, data, size);

	/* commit is purposefully unset for GCTX update failure to advise rollback */
	data_ex->fw_paddr = __psp_pa(fw_dest);
	data_ex->fw_len = size;
	data_ex->length = sizeof(struct sev_data_download_firmware_ex);

	ret = sev_do_cmd(SEV_CMD_SNP_DOWNLOAD_FIRMWARE_EX, data_ex, error);

	if (ret)
		goto free_err;

	/* Need to do a DF_FLUSH after live firmware update */
	wbinvd_on_all_cpus();
	ret = sev_do_cmd(SEV_CMD_SNP_DF_FLUSH, NULL, error);
	if (ret)
		dev_dbg(sev->dev, "DF_FLUSH error %d\n", *error);

free_err:
	__free_pages(p, order);
	return ret;
}

static enum fw_upload_err snp_dlfw_ex_prepare(struct fw_upload *fw_upload,
					      const u8 *data, u32 size)
{
	struct sev_device *sev = fw_upload->dd_handle;

	sev->fw_cancel = false;
	return FW_UPLOAD_ERR_NONE;
}

static enum fw_upload_err snp_dlfw_ex_poll_complete(struct fw_upload *fw_upload)
{
	return FW_UPLOAD_ERR_NONE;
}

/* Cancel can be called asynchronously, but DOWNLOAD_FIRMWARE_EX is atomic and cannot
 * be canceled. There is no need to synchronize updates to fw_cancel.
 */
static void snp_dlfw_ex_cancel(struct fw_upload *fw_upload)
{
	/* fw_upload not-NULL guaranteed by firmware_upload API */
	struct sev_device *sev = fw_upload->dd_handle;

	sev->fw_cancel = true;
}

static enum fw_upload_err snp_dlfw_ex_err_translate(struct sev_device *sev, int psp_ret)
{
	dev_dbg(sev->dev, "Failed to update SEV firmware: %#x\n", psp_ret);

	/*
	 * Operation error:
	 *   HW_ERROR: Critical error. Machine needs repairs now.
	 *   RW_ERROR: Severe error. Roll back to the prior version to recover.
	 * User error:
	 *   FW_INVALID: Bad input for this interface.
	 *   BUSY: Wrong machine state to run download_firmware_ex.
	 */
	switch (psp_ret) {
	case SEV_RET_RESTORE_REQUIRED:
		dev_warn(sev->dev, "Firmware updated but unusable. Rollback!!!\n");
		return FW_UPLOAD_ERR_RW_ERROR;
	case SEV_RET_SHUTDOWN_REQUIRED:
		/* No state changes made. Not a hardware error. */
		dev_warn(sev->dev, "Firmware image cannot be live updated\n");
		return FW_UPLOAD_ERR_FW_INVALID;
	case SEV_RET_BAD_VERSION:
		/* No state changes made. Not a hardware error. */
		dev_warn(sev->dev, "Firmware image is not well formed\n");
		return FW_UPLOAD_ERR_FW_INVALID;
		/* SEV-specific errors that can still happen. */
	case SEV_RET_BAD_SIGNATURE:
		/* No state changes made. Not a hardware error. */
		dev_warn(sev->dev, "Firmware image signature is bad\n");
		return FW_UPLOAD_ERR_FW_INVALID;
	case SEV_RET_INVALID_PLATFORM_STATE:
		/* Calling at the wrong time. Not a hardware error. */
		dev_warn(sev->dev, "Firmware not updated as SEV in INIT state\n");
		return FW_UPLOAD_ERR_BUSY;
	case SEV_RET_HWSEV_RET_UNSAFE:
		dev_err(sev->dev, "Firmware is unstable. Reset your machine!!!\n");
		return FW_UPLOAD_ERR_HW_ERROR;
		/* Kernel bug cases. */
	case SEV_RET_INVALID_PARAM:
		dev_err(sev->dev, "Download-firmware-EX invalid parameter\n");
		return FW_UPLOAD_ERR_RW_ERROR;
	case SEV_RET_INVALID_ADDRESS:
		dev_err(sev->dev, "Download-firmware-EX invalid address\n");
		return FW_UPLOAD_ERR_RW_ERROR;
	default:
		dev_err(sev->dev, "Unhandled download_firmware_ex err %d\n", psp_ret);
		return FW_UPLOAD_ERR_HW_ERROR;
	}
}

static enum fw_upload_err snp_update_guest_contexts(struct sev_device *sev)
{
	struct sev_data_snp_guest_status status_data;
	void *snp_guest_status;
	enum fw_upload_err ret = FW_UPLOAD_ERR_NONE;
	int rc, error;

	/*
	 * Force an update of guest context pages after SEV firmware
	 * live update by issuing SNP_GUEST_STATUS on all guest
	 * context pages.
	 */
	snp_guest_status = sev_fw_alloc(PAGE_SIZE);
	if (!snp_guest_status)
		return FW_UPLOAD_ERR_INVALID_SIZE;

	for (int i = 1; i <= sev_es_max_asid; i++) {
		if (!sev_asid_data[i].snp_context)
			continue;

		status_data.gctx_paddr = __psp_pa(sev_asid_data[i].snp_context);
		status_data.address = __psp_pa(snp_guest_status);
		rc = sev_do_cmd(SEV_CMD_SNP_GUEST_STATUS, &status_data, &error);
		if (!rc)
			continue;

		/*
		 * Handle race with SNP VM being destroyed/decommissoned,
		 * if guest context page invalid error is returned,
		 * assume guest has been destroyed.
		 */
		if (error == SEV_RET_INVALID_GUEST)
			continue;

		/* Guest context page update failure should force userspace to rollback,
		 * so make all non-DOWNLOAD_FIRMWARE_EX commands fail with RESTORE_REQUIRED.
		 * This emulates the behavior of the firmware on an older PSP bootloader version
		 * that couldn't auto-restore on DOWNLOAD_FIRMWARE_EX failure. However, the error
		 * is still relevant to this follow-up guest update failure.
		 */
		synthetic_restore_required = true;
		dev_err(sev->dev,
			"SNP guest context update error, rc=%d, fw_error=0x%x. Rollback!!!\n",
			rc, error);
		ret = FW_UPLOAD_ERR_RW_ERROR;
		break;
	}

	snp_free_firmware_page(snp_guest_status);
	return ret;
}

static enum fw_upload_err snp_dlfw_ex_write(struct fw_upload *fwl, const u8 *data,
					    u32 offset, u32 size, u32 *written)
{
	/* fwl not-NULL guaranteed by firmware_upload API, and sev is non-NULL by precondition to
	 * snp_init_firmware_upload.
	 */
	struct sev_device *sev = fwl->dd_handle;
	u8 api_major, api_minor, build;
	int ret, error;

	if (!sev)
		return FW_UPLOAD_ERR_HW_ERROR;

	if (sev->fw_cancel)
		return FW_UPLOAD_ERR_CANCELED;

	/*
	 * SEV firmware update is a one-shot update operation, the write()
	 * callback to be invoked multiple times for the same update is
	 * unexpected.
	 */
	if (offset)
		return FW_UPLOAD_ERR_INVALID_SIZE;

	if (sev_get_api_version())
		return FW_UPLOAD_ERR_HW_ERROR;

	api_major = sev->api_major;
	api_minor = sev->api_minor;
	build     = sev->build;

	ret = sev_snp_download_firmware_ex(sev, data, size, &error);
	if (ret)
		return snp_dlfw_ex_err_translate(sev, error);

	ret = snp_update_guest_contexts(sev);
	if (ret)
		return ret;

	sev_get_api_version();
	if (api_major != sev->api_major || api_minor != sev->api_minor ||
	    build != sev->build) {
		dev_info(sev->dev, "SEV firmware updated from %d.%d.%d to %d.%d.%d\n",
			 api_major, api_minor, build,
			 sev->api_major, sev->api_minor, sev->build);
	} else {
		dev_info(sev->dev, "SEV firmware not updated, same as current version %d.%d.%d\n",
			 api_major, api_minor, build);
	}

	*written = size;

	return FW_UPLOAD_ERR_NONE;
}

static const struct fw_upload_ops snp_dlfw_ex_ops = {
	.prepare = snp_dlfw_ex_prepare,
	.write = snp_dlfw_ex_write,
	.poll_complete = snp_dlfw_ex_poll_complete,
	.cancel = snp_dlfw_ex_cancel,
};

/* PREREQUISITE: sev is non-NULL */
void snp_init_firmware_upload(struct sev_device *sev)
{
	struct fw_upload *fwl;

	fwl = firmware_upload_register(THIS_MODULE, sev->dev, "snp_dlfw_ex", &snp_dlfw_ex_ops, sev);
	if (IS_ERR(fwl)) {
		dev_err(sev->dev, "SEV firmware upload initialization error %ld\n", PTR_ERR(fwl));
		return;
	}

	sev->fwl = fwl;
}

/* PREREQUISITE: sev is non-NULL */
void snp_destroy_firmware_upload(struct sev_device *sev)
{
	if (!sev->fwl)
		return;

	firmware_upload_unregister(sev->fwl);
}
