// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 ARM Limited.
 */

#include <linux/errno.h>
#include <linux/media/arm/mali-vpu-config.h>

#include "mali-vpu-dec.h"
#include "mali-vpu-session.h"
#include "mali-vpu-variant.h"

void mali_vpu_session_init_dec(struct mali_vpu_session *sess)
{
	const struct mali_vpu_format_caps *variant_fmts = &sess->vpu_dev->variant->fmt_caps;

	sess->port_src.supported_fmts = variant_fmts->dec_src;
	sess->port_dst.supported_fmts = variant_fmts->dec_dst;

	const struct mali_vpu_control_caps *variant_ctrls = &sess->vpu_dev->variant->ctrl_caps;

	sess->ctrls_caps	= variant_ctrls->dec_ctrls;
	sess->num_ctrls_caps	= variant_ctrls->num_dec_ctrls;

	sess->min_buffers_required = 3;	/* For CAPTURE/dst queue */
}

int mali_vpu_ctrl_set_ignore_stream_headers(struct mali_vpu_session *sess,
					    u32 cid, s64 value)
{
	if (cid != V4L2_CID_MALI_VPU_IGNORE_STREAM_HEADERS)
		return -EINVAL;

	if (value == -1)
		return 0; /* tristate no-op */

	if (value != 0 && value != 1)
		return -EINVAL;

	(void)sess;
	(void)value;
	return 0;
}

int mali_vpu_ctrl_set_frame_reordering(struct mali_vpu_session *sess, u32 cid,
				       s64 value)
{
	if (cid != V4L2_CID_MALI_VPU_FRAME_REORDERING)
		return -EINVAL;

	if (value == -1)
		return 0; /* tristate no-op */

	if (value != 0 && value != 1)
		return -EINVAL;

	(void)sess;
	(void)value;
	return 0;
}
