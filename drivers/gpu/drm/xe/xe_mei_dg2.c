// SPDX-License-Identifier: MIT
/*
 * Copyright (c) 2026 Intel Corporation. All rights reserved.
 */

#include <linux/device.h>
#include <linux/mei_cl_bus.h>
#include <drm/drm_managed.h>
#include <uapi/drm/xe_drm.h>

#include "xe_mei_dg2.h"
#include "xe_bo.h"
#include "xe_device.h"
#include "xe_gt.h"
#include "xe_huc.h"

#define PXP_APIVER(x, y) (((x) & 0xFFFF) << 16 | ((y) & 0xFFFF))
#define PXP_STATUS_SUCCESS 0x0
#define PXP_STATUS_OP_NOT_PERMITTED 0x1003

struct pxp_cmd_header {
	u32 api_version;
	u32 command_id;
	u32 status;
	u32 buffer_len;
} __packed;

#define PXP43_CMDID_START_HUC_AUTH 0x0000003A

struct pxp43_start_huc_auth_in {
	struct pxp_cmd_header header;
	__le64 huc_base_address;
} __packed;

struct pxp43_huc_auth_out {
	struct pxp_cmd_header header;
} __packed;

static int match_pxp_client(struct device *dev, void *data)
{
	if (!dev->bus || !dev->bus->name || strcmp(dev->bus->name, "mei") != 0)
		return 0;

	if (strstr(dev_name(dev), "fbf6fcf1")) {
		struct device **result = data;
		*result = dev;
		return 1;
	}
	return 0;
}

static int find_pxp_client_deep(struct device *dev, void *data)
{
	if (match_pxp_client(dev, data))
		return 1;
	return device_for_each_child(dev, data, find_pxp_client_deep);
}

int xe_mei_dg2_auth_huc(struct xe_device *xe, struct xe_huc *huc)
{
	struct device *cl_dev = NULL;
	struct mei_cl_device *cldev;
	struct pxp43_start_huc_auth_in in = { 0 };
	struct pxp43_huc_auth_out out = { 0 };
	ssize_t byte;
	int ret;
	static int retry_count;

	device_for_each_child(xe->drm.dev, &cl_dev, find_pxp_client_deep);

	if (!cl_dev) {
		if (retry_count++ % 5 == 0)
			drm_info(
				&xe->drm,
				"HuC auth: waiting for MEI PXP client (HECI1) to initialize...\n");
		return -EAGAIN;
	}

	get_device(cl_dev);
	cldev = to_mei_cl_device(cl_dev);

	if (!mei_cldev_enabled(cldev)) {
		ret = mei_cldev_enable(cldev);
		if (ret) {
			put_device(cl_dev);
			return -EAGAIN;
		}
	}

	in.header.api_version = PXP_APIVER(4, 3);
	in.header.command_id = PXP43_CMDID_START_HUC_AUTH;
	in.header.buffer_len = sizeof(in.huc_base_address);
	in.huc_base_address =
		cpu_to_le64(xe_bo_main_addr(huc->fw.bo, PAGE_SIZE));

	byte = mei_cldev_send(cldev, (u8 *)&in, sizeof(in));
	if (byte < 0) {
		put_device(cl_dev);
		return byte;
	}

	byte = mei_cldev_recv(cldev, (u8 *)&out, sizeof(out));
	if (byte < 0) {
		put_device(cl_dev);
		return byte;
	}

	put_device(cl_dev);
	if (out.header.status == PXP_STATUS_OP_NOT_PERMITTED) {
		drm_info(
			&xe->drm,
			"HuC auth: GSC reports already authenticated (0x1003).\n");
		return 0;
	} else if (out.header.status != PXP_STATUS_SUCCESS) {
		drm_err(&xe->drm, "HuC auth MEI rejected: status 0x%x\n",
			out.header.status);
		return -EIO;
	}

	return 0;
}
