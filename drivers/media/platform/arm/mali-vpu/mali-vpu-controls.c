// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 ARM Limited.
 */

#include <linux/errno.h>
#include <linux/limits.h>
#include <linux/slab.h>
#include <media/v4l2-mem2mem.h>

#include <linux/media/arm/mali-vpu-config.h>

#include "mali-vpu-controls.h"
#include "mali-vpu-session.h"

static int mali_vpu_ctrl_slot_from_cid(const struct mali_vpu_session *sess,
				       u32 cid, u32 *slot)
{
	const struct mali_vpu_ctrl_state *ctrl_state = sess->ctrls_state;
	u32 i;

	for (i = 0; i < sess->num_ctrls_caps; i++) {
		if (ctrl_state[i].cid == cid) {
			*slot = i;
			return 0;
		}
	}

	return -EINVAL;
}

static int mali_vpu_get_volatile_ctrl(struct v4l2_ctrl *ctrl)
{
	struct mali_vpu_session *sess = container_of(ctrl->handler,
						     struct mali_vpu_session,
						     ctrl_handler);

	switch (ctrl->id) {
	case V4L2_CID_MIN_BUFFERS_FOR_CAPTURE:
		if (sess->role != MALI_VPU_DECODER)
			return -EINVAL;
		ctrl->val = sess->min_buffers_required;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int mali_vpu_apply_or_stage_ctrl(struct mali_vpu_session *sess,
					const struct mali_vpu_ctrl_desc *desc,
					struct mali_vpu_ctrl_state *state,
					s64 value)
{
	bool staged;
	int ret;

	staged = sess->fw_bin_state != MALI_VPU_SESSION_FW_BIN_READY;

	if (!staged) {
		ret = desc->set(sess, state->cid, value);
		if (ret)
			return ret;
	}

	state->value = value;
	state->client_set = true;
	state->is_staged = staged;

	return 0;
}

static int mali_vpu_set_ctrl(struct v4l2_ctrl *ctrl)
{
	struct mali_vpu_session *sess = container_of(ctrl->handler,
						     struct mali_vpu_session,
						     ctrl_handler);
	const struct mali_vpu_ctrl_desc *desc;
	struct mali_vpu_ctrl_state *state;
	struct vb2_queue *src_vq;
	u32 slot;
	int ret;

	ret = mali_vpu_ctrl_slot_from_cid(sess, ctrl->id, &slot);
	if (ret)
		return ret;

	desc = &sess->ctrls_caps[slot];
	state = &sess->ctrls_state[slot];

	src_vq = v4l2_m2m_get_src_vq(sess->m2m_ctx);
	if (vb2_is_streaming(src_vq) &&
	    !(desc->flags & MALI_VPU_CTRL_FLAG_DYNAMIC))
		return -EBUSY;

	return mali_vpu_apply_or_stage_ctrl(sess, desc, state, ctrl->val);
}

static const struct v4l2_ctrl_ops mali_vpu_ctrl_ops = {
	.g_volatile_ctrl = mali_vpu_get_volatile_ctrl,
	.s_ctrl          = mali_vpu_set_ctrl,
};

static int mali_vpu_register_control(struct mali_vpu_session *sess,
				     const struct mali_vpu_ctrl_desc *d)
{
	struct v4l2_ctrl *ctrl;

	if (d->is_custom) {
		struct v4l2_ctrl_config cfg = {
			.ops = &mali_vpu_ctrl_ops,
			.id = d->cid,
			.name = d->name,
			.type = d->type,
			.min = d->min_val,
			.max = d->max_val,
			.step = (d->type == V4L2_CTRL_TYPE_MENU) ? 0 : d->step_or_mask,
			.def = d->default_val,
			.menu_skip_mask = (d->type == V4L2_CTRL_TYPE_MENU) ? ~(d->step_or_mask) : 0,
			.qmenu = d->qmenu,
		};

		ctrl = v4l2_ctrl_new_custom(&sess->ctrl_handler, &cfg, NULL);

	} else if (d->type == V4L2_CTRL_TYPE_MENU) {
		ctrl = v4l2_ctrl_new_std_menu(&sess->ctrl_handler,
					      &mali_vpu_ctrl_ops,
					      d->cid,
					      d->max_val,
					      ~(d->step_or_mask),
					      d->default_val);
	} else {
		ctrl = v4l2_ctrl_new_std(&sess->ctrl_handler,
					 &mali_vpu_ctrl_ops,
					 d->cid,
					 d->min_val,
					 d->max_val,
					 d->step_or_mask,
					 d->default_val);
	}

	if (!ctrl || sess->ctrl_handler.error)
		return sess->ctrl_handler.error ? sess->ctrl_handler.error : -EINVAL;

	return 0;
}

static int mali_vpu_validate_ctrl_caps(const struct mali_vpu_session *sess)
{
	u32 i, j;

	if (!sess->ctrls_caps || !sess->num_ctrls_caps) {
		dev_err(sess->vpu_dev->dev, "invalid ctrl caps structure\n");
		return -EINVAL;
	}

	for (i = 0; i < sess->num_ctrls_caps; i++) {
		const struct mali_vpu_ctrl_desc *d = &sess->ctrls_caps[i];

		if (!d->cid || !d->set)
			return -EINVAL;

		if (d->min_val > d->max_val)
			return -EINVAL;

		if (d->default_val < d->min_val || d->default_val > d->max_val)
			return -EINVAL;

		if (d->is_custom && !d->name)
			return -EINVAL;

		if (d->is_custom && d->type == V4L2_CTRL_TYPE_MENU && !d->qmenu)
			return -EINVAL;

		/* Reject duplicate entries */
		for (j = i + 1; j < sess->num_ctrls_caps; j++) {
			if (d->cid == sess->ctrls_caps[j].cid)
				return -EINVAL;
		}
	}

	return 0;
}

int mali_vpu_ctrls_init(struct mali_vpu_session *sess)
{
	const struct mali_vpu_ctrl_desc *ctrls_cap = sess->ctrls_caps;
	struct mali_vpu_ctrl_state *ctrl_state;
	struct v4l2_ctrl *volatile_ctrl;
	u32 i;
	int ret;

	ret = mali_vpu_validate_ctrl_caps(sess);
	if (ret)
		return ret;

	sess->ctrls_state = kcalloc(sess->num_ctrls_caps, sizeof(*sess->ctrls_state), GFP_KERNEL);
	if (!sess->ctrls_state)
		return -ENOMEM;

	/*
	 * Adding 1 to num_ctrls to include
	 * V4L2_CID_MIN_BUFFERS_FOR_CAPTURE for decoder
	 */
	ret = v4l2_ctrl_handler_init(&sess->ctrl_handler, sess->num_ctrls_caps + 1);
	if (ret)
		goto err_free_state;

	sess->ctrl_handler.lock = &sess->vq_lock;

	ctrl_state = sess->ctrls_state;

	for (i = 0; i < sess->num_ctrls_caps; i++) {
		const struct mali_vpu_ctrl_desc *d = &ctrls_cap[i];

		ctrl_state[i].cid = d->cid;
		ctrl_state[i].value = d->default_val;
		ctrl_state[i].client_set = false;
		ctrl_state[i].is_staged = false;

		ret = mali_vpu_register_control(sess, d);
		if (ret)
			goto err_free_handler;
	}

	if (sess->role == MALI_VPU_DECODER) {
		volatile_ctrl = v4l2_ctrl_new_std(&sess->ctrl_handler,
						  &mali_vpu_ctrl_ops,
						  V4L2_CID_MIN_BUFFERS_FOR_CAPTURE,
						  1, 32, 1, 1);
	} else {
		ret = -EINVAL;
		goto err_free_handler;
	}

	if (!volatile_ctrl || sess->ctrl_handler.error) {
		ret = sess->ctrl_handler.error ? sess->ctrl_handler.error : -EINVAL;
		goto err_free_handler;
	}

	volatile_ctrl->flags |= V4L2_CTRL_FLAG_VOLATILE | V4L2_CTRL_FLAG_READ_ONLY;

	sess->fh.ctrl_handler = &sess->ctrl_handler;

	return 0;

err_free_handler:
	v4l2_ctrl_handler_free(&sess->ctrl_handler);

err_free_state:
	kfree(sess->ctrls_state);
	sess->ctrls_state = NULL;
	return ret;
}

void mali_vpu_ctrls_cleanup(struct mali_vpu_session *sess)
{
	v4l2_ctrl_handler_free(&sess->ctrl_handler);
	kfree(sess->ctrls_state);
	sess->ctrls_state = NULL;
	sess->fh.ctrl_handler = NULL;
}

int mali_vpu_apply_staged_controls(struct mali_vpu_session *sess)
{
	u32 i;
	int ret;

	if (sess->fw_bin_state != MALI_VPU_SESSION_FW_BIN_READY)
		return -EINVAL;

	for (i = 0; i < sess->num_ctrls_caps; i++) {
		const struct mali_vpu_ctrl_desc *d = &sess->ctrls_caps[i];
		struct mali_vpu_ctrl_state *s = &sess->ctrls_state[i];

		if (!s->client_set || !s->is_staged)
			continue;

		ret = mali_vpu_apply_or_stage_ctrl(sess, d, s, s->value);
		if (ret)
			return ret;
	}

	return 0;
}

int mali_vpu_ctrl_set_nalu_format(struct mali_vpu_session *sess, u32 cid,
				  s64 value)
{
	struct mali_vpu_fw_set_option option;

	if (cid != V4L2_CID_MALI_VPU_NALU_FORMAT)
		return -EINVAL;

	option.code = MALI_VPU_FW_SET_NALU_FORMAT;
	option.nalu_format = value;

	return mali_vpu_session_fw_send_set_option(sess, &option);
}

int mali_vpu_ctrl_set_intbuf_size(struct mali_vpu_session *sess, u32 cid,
				  s64 value)
{
	struct mali_vpu_fw_set_option option;

	if (cid != V4L2_CID_MALI_VPU_INTBUF_SIZE || value < 0 || value > INT_MAX)
		return -EINVAL;

	option.code = MALI_VPU_FW_SET_INTBUF_SIZE;
	option.intbuf_size = value;

	return mali_vpu_session_fw_send_set_option(sess, &option);
}

int mali_vpu_ctrl_set_stream_escaping(struct mali_vpu_session *sess, u32 cid,
				      s64 value)
{
	struct mali_vpu_fw_set_option option;

	if (cid != V4L2_CID_MALI_VPU_STREAM_ESCAPING)
		return -EINVAL;

	if (value == -1)
		return 0; /* tristate no-op */

	if (value != 0 && value != 1)
		return -EINVAL;

	option.code = MALI_VPU_FW_SET_STREAM_ESCAPING;
	option.stream_escaping = value;

	return mali_vpu_session_fw_send_set_option(sess, &option);
}

int mali_vpu_ctrl_set_host_print_enable(struct mali_vpu_session *sess, u32 cid,
					s64 value)
{
	struct mali_vpu_fw_set_option option;

	if (cid != V4L2_CID_MALI_VPU_HOST_PRINT_ENABLE)
		return -EINVAL;

	if (value != 0 && value != 1)
		return -EINVAL;

	option.code = MALI_VPU_FW_SET_HOST_PROTOCOL_PRINTS;
	option.host_protocol_prints = value;

	return mali_vpu_session_fw_send_set_option(sess, &option);
}
