// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 ARM Limited.
 */

#include <linux/errno.h>
#include <linux/media/arm/mali-vpu-config.h>

#include "mali-vpu-dec.h"
#include "mali-vpu-session.h"
#include "mali-vpu-variant.h"
#include "mali-vpu-state.h"

void mali_vpu_session_init_dec(struct mali_vpu_session *sess)
{
	const struct mali_vpu_format_caps *variant_fmts = &sess->vpu_dev->variant->fmt_caps;

	sess->port_src.supported_fmts = variant_fmts->dec_src;
	sess->port_dst.supported_fmts = variant_fmts->dec_dst;

	const struct mali_vpu_control_caps *variant_ctrls = &sess->vpu_dev->variant->ctrl_caps;

	sess->ctrls_caps	= variant_ctrls->dec_ctrls;
	sess->num_ctrls_caps	= variant_ctrls->num_dec_ctrls;

	sess->min_buffers_required = 3;	/* For CAPTURE/dst queue */
	sess->sess_state	= MALI_VPU_STATE_INITIALIZATION;
}

int mali_vpu_ctrl_set_ignore_stream_headers(struct mali_vpu_session *sess,
					    u32 cid, s64 value)
{
	struct mali_vpu_fw_set_option option;

	if (cid != V4L2_CID_MALI_VPU_IGNORE_STREAM_HEADERS)
		return -EINVAL;

	if (value == -1)
		return 0; /* tristate no-op */

	if (value != 0 && value != 1)
		return -EINVAL;

	option.code = MALI_VPU_FW_SET_IGNORE_STREAM_HEADERS;
	option.ignore_stream_headers = value;

	return mali_vpu_session_fw_send_set_option(sess, &option);
}

int mali_vpu_ctrl_set_frame_reordering(struct mali_vpu_session *sess, u32 cid,
				       s64 value)
{
	struct mali_vpu_fw_set_option option;

	if (cid != V4L2_CID_MALI_VPU_FRAME_REORDERING)
		return -EINVAL;

	if (value == -1)
		return 0; /* tristate no-op */

	if (value != 0 && value != 1)
		return -EINVAL;

	option.code = MALI_VPU_FW_SET_FRAME_REORDERING;
	option.frame_reordering = value;

	return mali_vpu_session_fw_send_set_option(sess, &option);
}

int mali_vpu_decoder_cmd(struct file *filp, void *priv, struct v4l2_decoder_cmd *cmd)
{
	struct mali_vpu_session *sess = mali_vpu_get_session(filp);
	struct vb2_queue *dst_q = v4l2_m2m_get_dst_vq(sess->m2m_ctx);
	int ret;

	ret = v4l2_m2m_ioctl_try_decoder_cmd(filp, priv, cmd);
	if (ret)
		return ret;

	mutex_lock(&sess->vq_lock);

	switch (cmd->cmd) {
	case V4L2_DEC_CMD_STOP:
		if (sess->sess_state == MALI_VPU_STATE_DRAIN) {
			ret = -EBUSY;
			break;
		} else if (sess->sess_state == MALI_VPU_STATE_STREAMING) {
			ret = mali_vpu_session_fw_send_eos(sess);
			if (!ret)
				ret = mali_vpu_dec_update_state(sess, MALI_VPU_STATE_DRAIN);
		}
		break;
	case V4L2_DEC_CMD_START:
		if (sess->sess_state == MALI_VPU_STATE_DRAIN &&
		    !dst_q->last_buffer_dequeued) {
			ret = -EBUSY;
			break;
		}

		ret = mali_vpu_submit_deferred_bufs(sess);
		if (ret)
			break;

		ret = mali_vpu_dec_update_state(sess, MALI_VPU_STATE_STREAMING);
		if (ret)
			break;

		vb2_clear_last_buffer_dequeued(dst_q);
		break;
	default:
		ret = -EINVAL;
		break;
	}

	mutex_unlock(&sess->vq_lock);
	return ret;
}

static bool mali_vpu_dec_allow_state_change(struct mali_vpu_session *sess,
					    enum mali_vpu_sess_state new_state)
{
	switch (sess->sess_state) {
	case MALI_VPU_STATE_INITIALIZATION:
		return (new_state == MALI_VPU_STATE_RESET ||
			new_state == MALI_VPU_STATE_STOPPED);
	case MALI_VPU_STATE_STREAMING:
		return (new_state == MALI_VPU_STATE_RESET ||
			new_state == MALI_VPU_DEC_STATE_DYNAMIC_RESOLUTION_CHANGE ||
			new_state == MALI_VPU_STATE_DRAIN);
	case MALI_VPU_STATE_RESET:
		return (new_state == MALI_VPU_STATE_INITIALIZATION ||
			new_state == MALI_VPU_STATE_STREAMING);
	case MALI_VPU_STATE_DRAIN:
		return (new_state == MALI_VPU_STATE_STOPPED ||
			new_state == MALI_VPU_STATE_RESET);
	case MALI_VPU_STATE_STOPPED:
		return (new_state == MALI_VPU_STATE_INITIALIZATION ||
			new_state == MALI_VPU_DEC_STATE_FIRST_SOURCE_CHANGE ||
			new_state == MALI_VPU_STATE_STREAMING ||
			new_state == MALI_VPU_STATE_RESET);
	case MALI_VPU_DEC_STATE_DYNAMIC_RESOLUTION_CHANGE:
		return new_state == MALI_VPU_STATE_STOPPED;
	case MALI_VPU_DEC_STATE_FIRST_SOURCE_CHANGE:
		return new_state == MALI_VPU_STATE_STREAMING;
	default:
		return false;
	}
}

int mali_vpu_dec_update_state(struct mali_vpu_session *sess, u32 new_state)
{
	if (sess->role != MALI_VPU_DECODER)
		return -EINVAL;

	if (sess->sess_state == new_state)
		return 0;

	if (!mali_vpu_dec_allow_state_change(sess, new_state))
		return -EINVAL;

	sess->sess_state = new_state;
	return 0;
}

int mali_vpu_dec_state_change_streamon(struct mali_vpu_session *sess, enum v4l2_buf_type type)
{
	enum mali_vpu_sess_state new_state;

	switch (sess->sess_state) {
	case MALI_VPU_STATE_INITIALIZATION:
		new_state = V4L2_TYPE_IS_OUTPUT(type) ?
			MALI_VPU_STATE_STOPPED : MALI_VPU_STATE_RESET;
		break;
	case MALI_VPU_STATE_STOPPED:
	case MALI_VPU_DEC_STATE_FIRST_SOURCE_CHANGE:
		if (V4L2_TYPE_IS_OUTPUT(type))
			return 0;
		new_state = MALI_VPU_STATE_STREAMING;
		break;
	case MALI_VPU_STATE_RESET:
		if (!V4L2_TYPE_IS_OUTPUT(type))
			return 0;
		new_state = MALI_VPU_STATE_STREAMING;
		break;
	default:
		return 0;
	}

	return mali_vpu_dec_update_state(sess, new_state);
}

int mali_vpu_dec_state_change_streamoff(struct mali_vpu_session *sess, enum v4l2_buf_type type)
{
	enum mali_vpu_sess_state new_state;

	if (V4L2_TYPE_IS_OUTPUT(type)) {
		switch (sess->sess_state) {
		case MALI_VPU_STATE_STOPPED:
			new_state = MALI_VPU_STATE_INITIALIZATION;
			break;
		case MALI_VPU_STATE_STREAMING:
		case MALI_VPU_STATE_DRAIN:
			new_state = MALI_VPU_STATE_RESET;
			break;
		default:
			return 0;
		}
	} else {
		switch (sess->sess_state) {
		case MALI_VPU_STATE_RESET:
			new_state = MALI_VPU_STATE_INITIALIZATION;
			break;
		case MALI_VPU_STATE_STREAMING:
		case MALI_VPU_STATE_DRAIN:
			new_state = MALI_VPU_STATE_STOPPED;
			break;
		case MALI_VPU_DEC_STATE_DYNAMIC_RESOLUTION_CHANGE:
			new_state = MALI_VPU_STATE_STOPPED;
			break;
		default:
			return 0;
		}
	}

	return mali_vpu_dec_update_state(sess, new_state);
}
