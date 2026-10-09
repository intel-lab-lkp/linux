// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2026, Advanced Micro Devices, Inc.
 */

#include <drm/amdxdna_accel.h>
#include <drm/drm_print.h>
#include <linux/bitfield.h>
#include <linux/iommu.h>
#include <linux/mutex.h>

#include "aie.h"
#include "aie4_msg_priv.h"
#include "aie4_pci.h"
#include "amdxdna_mailbox.h"
#include "amdxdna_mailbox_helper.h"
#include "amdxdna_pci_drv.h"

u32 aie4_msg_pasid(struct amdxdna_client *client)
{
	if (!amdxdna_pasid_on(client))
		return IOMMU_PASID_INVALID;

	return FIELD_PREP(AIE4_MSG_PASID, client->pasid) |
	       FIELD_PREP(AIE4_MSG_PASID_VLD, 1);
}

int aie4_suspend_fw(struct amdxdna_dev_hdl *ndev)
{
	DECLARE_AIE_MSG(aie4_msg_suspend, AIE4_MSG_OP_SUSPEND);
	int ret;

	ret = aie_send_mgmt_msg_wait(&ndev->aie, &msg);
	if (ret)
		XDNA_ERR(ndev->aie.xdna, "Failed to suspend fw, ret %d", ret);

	return ret;
}

int aie4_query_aie_metadata(struct amdxdna_dev_hdl *ndev,
			    struct amdxdna_drm_query_aie_metadata *metadata)
{
	DECLARE_AIE_MSG(aie4_msg_aie4_tile_info, AIE4_MSG_OP_AIE_TILE_INFO);
	int ret;

	ret = aie_send_mgmt_msg_wait(&ndev->aie, &msg);
	if (ret)
		return ret;

	metadata->col_size = resp.info.size;
	metadata->cols = resp.info.cols;
	metadata->rows = resp.info.rows;

	metadata->version.major = resp.info.major;
	metadata->version.minor = resp.info.minor;

	metadata->core.row_count = resp.info.core_rows;
	metadata->core.row_start = resp.info.core_row_start;
	metadata->core.dma_channel_count = resp.info.core_dma_channels;
	metadata->core.lock_count = resp.info.core_locks;
	metadata->core.event_reg_count = resp.info.core_events;

	metadata->mem.row_count = resp.info.mem_rows;
	metadata->mem.row_start = resp.info.mem_row_start;
	metadata->mem.dma_channel_count = resp.info.mem_dma_channels;
	metadata->mem.lock_count = resp.info.mem_locks;
	metadata->mem.event_reg_count = resp.info.mem_events;

	metadata->shim.row_count = resp.info.shim_rows;
	metadata->shim.row_start = resp.info.shim_row_start;
	metadata->shim.dma_channel_count = resp.info.shim_dma_channels;
	metadata->shim.lock_count = resp.info.shim_locks;
	metadata->shim.event_reg_count = resp.info.shim_events;

	return 0;
}

int aie4_query_npu_firmware_version(struct amdxdna_dev_hdl *ndev,
				    struct amdxdna_drm_query_firmware_version *fw_version)
{
	DECLARE_AIE_MSG(aie4_msg_identify, AIE4_MSG_OP_IDENTIFY);
	int ret;

	ret = aie_send_mgmt_msg_wait(&ndev->aie, &msg);
	if (ret)
		return ret;

	fw_version->major = resp.fw_major;
	fw_version->minor = resp.fw_minor;
	fw_version->patch = resp.fw_patch;
	fw_version->build = resp.fw_build;

	return 0;
}

int aie4_query_cert_firmware_version(struct amdxdna_dev_hdl *ndev,
				     struct amdxdna_drm_query_firmware_version *cert_version)
{
	DECLARE_AIE_MSG(aie4_msg_query_cert_firmware_version,
			AIE4_MSG_OP_QUERY_CERT_FIRMWARE_VERSION);
	int ret;

	ret = aie_send_mgmt_msg_wait(&ndev->aie, &msg);
	if (ret)
		return ret;

	ret = aie_check_protocol(&ndev->aie,
				 resp.host_queue_major, resp.host_queue_minor,
				 ndev->aie.xdna->dev_info->cert_feature_tbl);
	if (ret) {
		XDNA_ERR(ndev->aie.xdna, "host queue %d.%d is not supported",
			 resp.host_queue_major, resp.host_queue_minor);
		return ret;
	}

	cert_version->major = resp.major_version;
	cert_version->minor = resp.minor_version;
	cert_version->patch = resp.hotfix;
	cert_version->build = resp.build;

	return 0;
}

int aie4_attach_work_buffer(struct amdxdna_dev_hdl *ndev)
{
	DECLARE_AIE_MSG(aie4_msg_attach_work_buffer, AIE4_MSG_OP_ATTACH_WORK_BUFFER);
	struct amdxdna_dev *xdna = ndev->aie.xdna;
	int ret;

	req.buff_addr = ndev->work_buf_addr;
	req.buff_size = AIE4_WORK_BUFFER_MIN_SIZE;

	ret = aie_send_mgmt_msg_wait(&ndev->aie, &msg);
	if (ret)
		XDNA_ERR(xdna, "Failed to attach work buffer, ret %d", ret);
	else
		XDNA_DBG(xdna, "Attached work buffer");

	return ret;
}
