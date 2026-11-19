// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 ARM Limited.
 */

#include <linux/device.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <media/v4l2-mem2mem.h>

#include "mali-vpu-session.h"
#include "mali-vpu-controls.h"
#include "mali-vpu-dec.h"
#include "mali-vpu-firmware.h"
#include "mali-vpu-firmware-cache.h"
#include "mali-vpu-formats.h"
#include "mali-vpu-variant.h"
#include "mali-vpu-vb2.h"

static const unsigned int mali_vpu_fw_load_timeout_ms     = 10000;

static int mali_vpu_set_port_fmt(struct mali_vpu_port *port,
				 enum v4l2_buf_type type,
				 u32 width, u32 height)
{
	struct v4l2_pix_format_mplane *pix_mp = &port->curr_fmt.fmt.pix_mp;
	const struct mali_vpu_format_desc *fmt_default;
	int ret;

	if (!port->supported_fmts.fmts || !port->supported_fmts.num_fmts)
		return -EINVAL;

	fmt_default = &port->supported_fmts.fmts[0];

	memset(&port->curr_fmt, 0, sizeof(port->curr_fmt));
	port->curr_fmt.type = type;

	ret = mali_vpu_fill_pixmp_fmt(fmt_default, width, height, pix_mp);
	if (ret)
		return ret;

	pix_mp->field = V4L2_FIELD_NONE;

	return 0;
}

static int mali_vpu_set_port_defaults(struct mali_vpu_session *sess)
{
	const struct mali_vpu_framesize_caps *fmsz = &sess->vpu_dev->variant->frmsz_caps;
	int ret;

	ret = mali_vpu_set_port_fmt(&sess->port_src, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
				    fmsz->width_default, fmsz->height_default);
	if (ret)
		return ret;

	return mali_vpu_set_port_fmt(&sess->port_dst, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
				    fmsz->width_default, fmsz->height_default);
}

static bool
mali_vpu_both_ports_unrequested(struct mali_vpu_session *sess)
{
	return !sess->port_src.stream_requested &&
	       !sess->port_dst.stream_requested;
}

int mali_vpu_session_fw_send_set_option(struct mali_vpu_session *sess,
					struct mali_vpu_fw_set_option *opt)
{
	return mali_vpu_fw_send_set_option(&sess->fw, opt);
}

int mali_vpu_session_init(struct mali_vpu_session *sess)
{
	int ret;

	mutex_init(&sess->vq_lock);

	if (sess->role != MALI_VPU_DECODER) {
		ret = -EINVAL;
		goto err_mutex_destroy;
	}

	mali_vpu_session_init_dec(sess);

	ret = mali_vpu_set_port_defaults(sess);
	if (ret)
		goto err_mutex_destroy;

	sess->m2m_ctx = v4l2_m2m_ctx_init(sess->vpu_dev->m2m_dev, sess, mali_vpu_queue_init);
	if (IS_ERR(sess->m2m_ctx)) {
		ret = PTR_ERR(sess->m2m_ctx);
		goto err_mutex_destroy;
	}

	sess->fw_bin_state = MALI_VPU_SESSION_FW_BIN_UNINITIALIZED;

	ret = mali_vpu_ctrls_init(sess);
	if (ret) {
		dev_err(sess->vpu_dev->dev, "failed to initialise controls: %d\n", ret);
		goto err_m2m_ctx_release;
	}

	ret = mali_vpu_mmu_init(&sess->mmu, sess->vpu_dev->dev);
	if (ret) {
		dev_err(sess->vpu_dev->dev, "failed to initialise MMU: %d\n", ret);
		goto err_ctrls_cleanup;
	}

	sess->fw_event = kzalloc(sizeof(*sess->fw_event), GFP_KERNEL);
	if (!sess->fw_event) {
		ret = -ENOMEM;
		goto err_mmu_cleanup;
	}

	sess->fw_bin = NULL;
	sess->port_src.stream_requested = false;
	sess->port_dst.stream_requested = false;
	init_completion(&sess->fw_load_done);

	return 0;

err_mmu_cleanup:
	mali_vpu_mmu_cleanup(&sess->mmu);

err_ctrls_cleanup:
	mali_vpu_ctrls_cleanup(sess);

err_m2m_ctx_release:
	v4l2_m2m_ctx_release(sess->m2m_ctx);

err_mutex_destroy:
	mutex_destroy(&sess->vq_lock);
	return ret;
}

static void mali_vpu_session_release_fw_bin(struct mali_vpu_session *sess)
{
	lockdep_assert_held(&sess->vq_lock);

	if (sess->fw_bin_state == MALI_VPU_SESSION_FW_BIN_READY) {
		mali_vpu_fw_cleanup(&sess->fw);
		mali_vpu_fw_cache_put(sess->fw_bin);
		sess->fw_bin = NULL;
		sess->fw_bin_state = MALI_VPU_SESSION_FW_BIN_UNINITIALIZED;
		return;
	}

	if (sess->fw_bin_state == MALI_VPU_SESSION_FW_BIN_FAILED)
		sess->fw_bin_state = MALI_VPU_SESSION_FW_BIN_UNINITIALIZED;
}

int mali_vpu_session_cleanup(struct mali_vpu_session *sess)
{
	struct mali_vpu_dev *vpu_dev = sess->vpu_dev;
	int fw_load_ret = 0;
	bool waiter_cancelled = false;

	mutex_lock(&sess->vq_lock);

	sess->port_src.stream_requested = false;
	sess->port_dst.stream_requested = false;

	if (sess->fw_bin_state == MALI_VPU_SESSION_FW_BIN_LOADING) {
		mutex_unlock(&sess->vq_lock);

		if (!wait_for_completion_timeout(&sess->fw_load_done,
						 msecs_to_jiffies(mali_vpu_fw_load_timeout_ms))) {
			dev_warn(vpu_dev->dev,
				 "firmware load timed out\n");
			fw_load_ret = -ETIMEDOUT;

			waiter_cancelled = mali_vpu_fw_cache_cancel_waiter(vpu_dev->fw_cache,
									   sess->fw_event);

			/*
			 * The waiter was not found in the cache list, so
			 * firmware completion may already have claimed it for
			 * callback execution. Wait for the callback to finish
			 * before freeing the session and its event.
			 */
			if (!waiter_cancelled)
				wait_for_completion(&sess->fw_load_done);
		}

		mutex_lock(&sess->vq_lock);

		if (waiter_cancelled)
			sess->fw_bin_state = MALI_VPU_SESSION_FW_BIN_FAILED;
	}

	mali_vpu_session_release_fw_bin(sess);

	if (sess->m2m_ctx) {
		v4l2_m2m_ctx_release(sess->m2m_ctx);
		sess->m2m_ctx = NULL;
		sess->fh.m2m_ctx = NULL;
	}

	mutex_unlock(&sess->vq_lock);

	kfree(sess->fw_event);
	mali_vpu_mmu_cleanup(&sess->mmu);
	mali_vpu_ctrls_cleanup(sess);
	mutex_destroy(&sess->vq_lock);

	return fw_load_ret;
}

static void mali_vpu_session_fw_bin_ready(struct mali_vpu_fw_bin *vpu_fw_bin,
					  void *arg,
					  bool same_thread)
{
	struct mali_vpu_session *sess = arg;
	int ret;

	if (!same_thread)
		mutex_lock(&sess->vq_lock);

	/*
	 * A synchronous cache hit precedes marking the port requested.
	 * An asynchronous callback acquires vq_lock after preparation.
	 */
	if (!same_thread && mali_vpu_both_ports_unrequested(sess)) {
		sess->fw_bin_state = MALI_VPU_SESSION_FW_BIN_UNINITIALIZED;
		goto err_cache_put;
	}

	if (vpu_fw_bin->load_status) {
		sess->fw_bin_state = MALI_VPU_SESSION_FW_BIN_FAILED;
		goto err_cache_put;
	}

	sess->fw_bin = vpu_fw_bin;

	/*
	 * Prepare the scheduler's saved session hardware context.
	 * Scheduler assigns the session's core count.
	 */
	ret = mali_vpu_sched_session_prepare(&sess->sched_sess, sess);
	if (ret) {
		sess->fw_bin_state = MALI_VPU_SESSION_FW_BIN_UNINITIALIZED;
		goto err_cache_put;
	}

	ret = mali_vpu_fw_construct(&sess->fw, sess->fw_bin, sess->vpu_dev->dev,
				    &sess->mmu, sess->ncores);
	if (ret) {
		dev_err(sess->vpu_dev->dev,
			"failed to initialise firmware: %d\n", ret);
		sess->fw_bin_state = MALI_VPU_SESSION_FW_BIN_FAILED;
		goto err_cache_put;
	}

	sess->fw_bin_state = MALI_VPU_SESSION_FW_BIN_READY;

	goto out_complete;

err_cache_put:
	mali_vpu_fw_cache_put(vpu_fw_bin);

	sess->fw_bin = NULL;

out_complete:
	complete_all(&sess->fw_load_done);

	if (!same_thread)
		mutex_unlock(&sess->vq_lock);
}

static u32 mali_vpu_coded_q_fourcc(const struct mali_vpu_session *sess)
{
	if (sess->role == MALI_VPU_DECODER)
		return sess->port_src.curr_fmt.fmt.pix_mp.pixelformat;
	return sess->port_dst.curr_fmt.fmt.pix_mp.pixelformat;
}

static int mali_vpu_session_build_fw_key(const struct mali_vpu_session *sess,
					 u32 codec_fourcc,
					 struct mali_vpu_fw_key *key)
{
	if (!key || !sess)
		return -EINVAL;

	key->codec_fourcc = codec_fourcc;
	key->session_role = sess->role;
	key->fw_ver_major = sess->vpu_dev->variant->fw_ver_major;

	return 0;
}

int mali_vpu_session_request_fw_load(struct mali_vpu_session *sess)
{
	struct mali_vpu_fw_key fw_key;
	int ret;

	lockdep_assert_held(&sess->vq_lock);

	if (sess->fw_bin_state == MALI_VPU_SESSION_FW_BIN_READY ||
	    sess->fw_bin_state == MALI_VPU_SESSION_FW_BIN_LOADING)
		return 0;

	ret = mali_vpu_session_build_fw_key(sess, mali_vpu_coded_q_fourcc(sess), &fw_key);
	if (ret)
		return ret;

	sess->fw_event->fw_bin_ready = mali_vpu_session_fw_bin_ready;
	sess->fw_event->arg = sess;
	sess->fw_bin_state = MALI_VPU_SESSION_FW_BIN_LOADING;
	reinit_completion(&sess->fw_load_done);

	ret = mali_vpu_fw_cache_get(sess->vpu_dev->fw_cache, &fw_key, sess->fw_event);
	if (ret) {
		sess->fw_bin_state = MALI_VPU_SESSION_FW_BIN_FAILED;
		complete_all(&sess->fw_load_done);
		return ret;
	}

	return 0;
}
