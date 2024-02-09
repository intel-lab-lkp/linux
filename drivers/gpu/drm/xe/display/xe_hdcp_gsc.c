// SPDX-License-Identifier: MIT
/*
 * Copyright 2023, Intel Corporation.
 */

#include <drm/drm_print.h>

#include "intel_hdcp_gsc.h"
#include "xe_device_types.h"
#include "xe_gt.h"
#include "xe_gsc_proxy.h"
#include "xe_pm.h"

bool intel_hdcp_gsc_cs_required(struct xe_device *xe)
{
	return true;
}

bool intel_hdcp_gsc_check_status(struct xe_device *xe)
{
	struct xe_tile *tile = xe_device_get_root_tile(xe);
	struct xe_gt *gt = tile->media_gt;
	bool ret = true;

	xe_pm_runtime_get(xe);
	ret = xe_force_wake_get(gt_to_fw(gt), XE_FW_GSC);
	if (ret) {
		drm_dbg_kms(&xe->drm,
			    "failed to get forcewake to check proxy status\n");
		ret = false;
		goto out;
	}

	if (!xe_gsc_proxy_init_done(&gt->uc.gsc))
		ret = false;

	xe_force_wake_put(gt_to_fw(gt), XE_FW_GSC);
out:
	xe_pm_runtime_put(xe);
	return ret;
}

int intel_hdcp_gsc_init(struct xe_device *xe)
{
	drm_dbg_kms(&xe->drm, "HDCP support not yet implemented\n");
	return -ENODEV;
}

void intel_hdcp_gsc_fini(struct xe_device *xe)
{
}

ssize_t intel_hdcp_gsc_msg_send(struct xe_device *xe, u8 *msg_in,
				size_t msg_in_len, u8 *msg_out,
				size_t msg_out_len)
{
	return -ENODEV;
}
