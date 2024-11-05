// SPDX-License-Identifier: GPL-2.0-only
/*
 * AMD Secure Encrypted Virtualization (SEV) firmware upload API
 */

#include <linux/firmware.h>
#include <linux/psp.h>
#include <linux/psp-sev.h>

#include <asm/sev.h>

#include "sev-dev.h"

/*
 * After a gctx is created, it is used by snp_launch_start before getting
 * bound to an asid. The launch protocol allocates an asid before creating a
 * matching gctx page, so there should never be more unbound gctx pages than
 * there are possible SNP asids.
 *
 * The unbound gctx pages must be updated after executing DOWNLOAD_FIRMWARE_EX
 * and before committing the firmware.
 */
static void snp_gctx_create_track_locked(struct sev_device *sev, void *data)
{
	struct sev_data_snp_addr *gctx_create = data;

	/* This condition should never happen, but is needed for memory safety. */
	if (sev->snp_unbound_gctx_end >= sev->last_snp_asid) {
		dev_warn(sev->dev, "Too many unbound SNP GCTX pages to track\n");
		return;
	}

	sev->snp_unbound_gctx_pages[sev->snp_unbound_gctx_end] = gctx_create->address;
	sev->snp_unbound_gctx_end++;
}

/*
 * PREREQUISITE: The snp_activate command was successful, meaning the asid
 * is in the acceptable range 1..sev->last_snp_asid.
 *
 * The gctx_paddr must be in the unbound gctx buffer.
 */
static void snp_activate_track_locked(struct sev_device *sev, void *data)
{
	struct sev_data_snp_activate *data_activate = data;

	sev->snp_asid_to_gctx_pages_map[data_activate->asid] = data_activate->gctx_paddr;

	for (int i = 0; i < sev->snp_unbound_gctx_end; i++) {
		if (sev->snp_unbound_gctx_pages[i] == data_activate->gctx_paddr) {
			/*
			 * Swap the last unbound gctx page with the now-bound
			 * gctx page to shrink the buffer.
			 */
			sev->snp_unbound_gctx_end--;
			sev->snp_unbound_gctx_pages[i] =
				sev->snp_unbound_gctx_pages[sev->snp_unbound_gctx_end];
			sev->snp_unbound_gctx_pages[sev->snp_unbound_gctx_end] = 0;
			break;
		}
	}
}

static void snp_decommission_track_locked(struct sev_device *sev, void *data)
{
	struct sev_data_snp_addr *data_decommission = data;

	for (int i = 1; i <= sev->last_snp_asid; i++) {
		if (sev->snp_asid_to_gctx_pages_map[i] == data_decommission->address) {
			sev->snp_asid_to_gctx_pages_map[i] = 0;
			break;
		}
	}
}

void snp_cmd_bookkeeping_locked(int cmd, struct sev_device *sev, void *data)
{
	if (!sev->snp_asid_to_gctx_pages_map)
		return;

	switch (cmd) {
	case SEV_CMD_SNP_GCTX_CREATE:
		snp_gctx_create_track_locked(sev, data);
		break;
	case SEV_CMD_SNP_ACTIVATE:
		snp_activate_track_locked(sev, data);
		break;
	case SEV_CMD_SNP_DECOMMISSION:
		snp_decommission_track_locked(sev, data);
		break;
	default:
		break;
	}
}

int sev_snp_platform_init_firmware_upload(struct sev_device *sev)
{
	u32 max_snp_asid;

	/*
	 * cpuid_edx(0x8000001f) is the minimum SEV ASID, hence the exclusive
	 * maximum SEV-SNP ASID. Save the inclusive maximum to avoid confusing
	 * logic elsewhere.
	 */
	max_snp_asid = cpuid_edx(0x8000001f);
	sev->last_snp_asid = max_snp_asid - 1;
	if (sev->last_snp_asid) {
		sev->snp_asid_to_gctx_pages_map = devm_kmalloc_array(
			sev->dev, max_snp_asid * 2, sizeof(u64), GFP_KERNEL | __GFP_ZERO);
		sev->snp_unbound_gctx_pages = &sev->snp_asid_to_gctx_pages_map[max_snp_asid];
		if (!sev->snp_asid_to_gctx_pages_map) {
			dev_err(sev->dev,
				"SEV-SNP: snp_asid_to_gctx_pages_map memory allocation failed\n");
			return -ENOMEM;
		}
	}
	return 0;
}

static enum fw_upload_err snp_dlfw_ex_prepare(struct fw_upload *fw_upload,
					      const u8 *data, u32 size)
{
	return FW_UPLOAD_ERR_NONE;
}

static enum fw_upload_err snp_dlfw_ex_poll_complete(struct fw_upload *fw_upload)
{
	return FW_UPLOAD_ERR_NONE;
}

/*
 * This may be called asynchronously with an on-going update.  All other
 * functions are called sequentially in a single thread. To avoid contention on
 * register accesses, only update the cancel_request flag. Other functions will
 * check this flag and handle the cancel request synchronously.
 */
static void snp_dlfw_ex_cancel(struct fw_upload *fw_upload)
{
	struct sev_device *sev = fw_upload->dd_handle;

	mutex_lock(&sev->fw_lock);
	sev->fw_cancel = true;
	mutex_unlock(&sev->fw_lock);
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
		dev_warn(sev->dev, "Firmware updated but unusable\n");
		dev_warn(sev->dev, "Need to do manual firmware rollback!!!\n");
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

static enum fw_upload_err snp_update_guest_statuses(struct sev_device *sev)
{
	struct sev_data_snp_guest_status status_data;
	void *snp_guest_status;
	enum fw_upload_err ret;
	int error;

	/*
	 * Force an update of guest context pages after SEV firmware
	 * live update by issuing SNP_GUEST_STATUS on all guest
	 * context pages.
	 */
	snp_guest_status = sev_fw_alloc(PAGE_SIZE);
	if (!snp_guest_status)
		return FW_UPLOAD_ERR_INVALID_SIZE;

	/*
	 * After the last bound asid-to-gctx page is snp_unbound_gctx_end-many
	 * unbound gctx pages that also need updating.
	 */
	for (int i = 1; i <= sev->last_snp_asid + sev->snp_unbound_gctx_end; i++) {
		if (sev->snp_asid_to_gctx_pages_map[i]) {
			status_data.gctx_paddr = sev->snp_asid_to_gctx_pages_map[i];
			status_data.address = __psp_pa(snp_guest_status);
			ret = sev_do_cmd(SEV_CMD_SNP_GUEST_STATUS, &status_data, &error);
			if (ret) {
				/*
				 * Handle race with SNP VM being destroyed/decommissoned,
				 * if guest context page invalid error is returned,
				 * assume guest has been destroyed.
				 */
				if (error == SEV_RET_INVALID_GUEST)
					continue;
				sev->synthetic_restore_required = true;
				dev_err(sev->dev, "SNP GCTX update error: %#x\n", error);
				dev_err(sev->dev, "Roll back SNP firmware!\n");
				snp_free_firmware_page(snp_guest_status);
				ret = FW_UPLOAD_ERR_RW_ERROR;
				goto fw_err;
			}
		}
	}
fw_err:
	snp_free_firmware_page(snp_guest_status);
	return ret;
}

static enum fw_upload_err snp_dlfw_ex_write(struct fw_upload *fwl, const u8 *data,
					    u32 offset, u32 size, u32 *written)
{
	struct sev_device *sev = fwl->dd_handle;
	u8 api_major, api_minor, build;
	int ret, error;
	bool cancel;

	if (!sev)
		return FW_UPLOAD_ERR_HW_ERROR;

	mutex_lock(&sev->fw_lock);
	cancel = sev->fw_cancel;
	mutex_unlock(&sev->fw_lock);

	if (cancel)
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

	ret = snp_update_guest_statuses(sev);
	if (ret)
		return ret;

	sev_get_api_version();
	if (api_major != sev->api_major || api_minor != sev->api_minor ||
	    build != sev->build) {
		dev_info(sev->dev, "SEV firmware updated from %d.%d.%d to %d.%d.%d\n",
			 api_major, api_minor, build,
			 sev->api_major, sev->api_minor, sev->build);
	} else {
		dev_info(sev->dev, "SEV firmware same as old %d.%d.%d\n",
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

void sev_snp_dev_init_firmware_upload(struct sev_device *sev)
{
	struct fw_upload *fwl;

	fwl = firmware_upload_register(THIS_MODULE, sev->dev, "snp_dlfw_ex", &snp_dlfw_ex_ops, sev);

	if (IS_ERR(fwl))
		dev_err(sev->dev, "SEV firmware upload initialization error %ld\n", PTR_ERR(fwl));

	sev->fwl = fwl;
	mutex_init(&sev->fw_lock);
}

void sev_snp_destroy_firmware_upload(struct sev_device *sev)
{
	firmware_upload_unregister(sev->fwl);
}

int sev_snp_synthetic_error(struct sev_device *sev, int *psp_ret)
{
	if (sev->synthetic_restore_required) {
		*psp_ret = SEV_RET_RESTORE_REQUIRED;
		return -EIO;
	}
	return 0;
}
