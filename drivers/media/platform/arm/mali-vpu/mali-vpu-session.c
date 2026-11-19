// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 ARM Limited.
 */

#include <linux/array_size.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <media/v4l2-event.h>
#include <media/v4l2-mem2mem.h>

#include "mali-vpu-session.h"
#include "mali-vpu-controls.h"
#include "mali-vpu-dec.h"
#include "mali-vpu-firmware.h"
#include "mali-vpu-firmware-cache.h"
#include "mali-vpu-formats.h"
#include "mali-vpu-variant.h"
#include "mali-vpu-vb2.h"

#define MALI_VPU_TS_METADATA_FLAGS \
	(V4L2_BUF_FLAG_TIMECODE | V4L2_BUF_FLAG_TSTAMP_SRC_MASK)

static const unsigned int mali_vpu_fw_response_timeout_ms = 10000;
static const unsigned int mali_vpu_fw_load_timeout_ms     = 10000;

static struct mali_vpu_port *
mali_vpu_session_get_port(struct mali_vpu_session *sess, enum v4l2_buf_type type)
{
	return V4L2_TYPE_IS_OUTPUT(type) ? &sess->port_src : &sess->port_dst;
}

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

static bool mali_vpu_both_queues_streaming(const struct mali_vpu_session *sess)
{
	struct vb2_queue *src_q = v4l2_m2m_get_src_vq(sess->m2m_ctx);
	struct vb2_queue *dst_q = v4l2_m2m_get_dst_vq(sess->m2m_ctx);

	return vb2_is_streaming(src_q) && vb2_is_streaming(dst_q);
}

static bool
mali_vpu_both_ports_unrequested(struct mali_vpu_session *sess)
{
	return !sess->port_src.stream_requested &&
	       !sess->port_dst.stream_requested;
}

static bool mali_vpu_session_can_stream(const struct mali_vpu_session *sess)
{
	/*
	 * Start a decoder with OUTPUT/src port only, so firmware can
	 * parse the bitstream and report its decoded format.
	 * An encoder requires both ports.
	 *
	 * Use the driver's preparation state because VB2 sets its streaming
	 * flag only after prepare_streaming() and initial buffer handoff.
	 */
	if (sess->role == MALI_VPU_DECODER)
		return sess->port_src.stream_requested;

	return sess->port_src.stream_requested &&
	       sess->port_dst.stream_requested;
}

static int mali_vpu_session_fw_send_job(struct mali_vpu_session *sess, u16 frames)
{
	int ret;

	ret = mali_vpu_fw_send_job(&sess->fw, sess->ncores, frames);
	if (ret)
		return ret;

	mali_vpu_sched_send_irq(&sess->sched_sess, &sess->vpu_dev->sched);
	return 0;
}

static int mali_vpu_session_fw_send_ping(struct mali_vpu_session *sess)
{
	int ret;

	ret = mali_vpu_fw_send_ping(&sess->fw);
	if (ret)
		return ret;

	mali_vpu_sched_send_irq(&sess->sched_sess, &sess->vpu_dev->sched);
	return 0;
}

static int mali_vpu_session_fw_send_state(struct mali_vpu_session *sess,
					  enum mali_vpu_fw_state new_state)
{
	int ret;

	if (sess->fw_requested_state != sess->fw_state)
		/* There is a pending request: is this one duplicate? */
		return sess->fw_requested_state == new_state ? 0 : -EBUSY;

	if (sess->fw_state == new_state)
		return 0;

	ret = mali_vpu_fw_send_state_change(&sess->fw, new_state);
	if (ret)
		return ret;

	sess->fw_requested_state = new_state;
	mali_vpu_sched_send_irq(&sess->sched_sess, &sess->vpu_dev->sched);

	return 0;
}

int mali_vpu_session_fw_send_set_option(struct mali_vpu_session *sess,
					struct mali_vpu_fw_set_option *opt)
{
	int ret;

	if (sess->error)
		return sess->error;

	ret = mali_vpu_fw_send_set_option(&sess->fw, opt);
	if (ret)
		return ret;

	mali_vpu_sched_send_irq(&sess->sched_sess, &sess->vpu_dev->sched);
	return 0;
}

int mali_vpu_session_fw_send_eos(struct mali_vpu_session *sess)
{
	int ret;

	if (sess->error)
		return sess->error;

	if (!mali_vpu_both_queues_streaming(sess))
		return 0;

	ret = mali_vpu_fw_send_eos(&sess->fw);
	if (ret)
		return ret;

	mali_vpu_sched_send_irq(&sess->sched_sess, &sess->vpu_dev->sched);
	return 0;
}

static int mali_vpu_session_fw_send_flush(struct mali_vpu_session *sess, bool to_src)
{
	int ret;

	if (to_src)
		ret = mali_vpu_fw_send_flush_src(&sess->fw);
	else
		ret = mali_vpu_fw_send_flush_dst(&sess->fw);

	if (ret)
		return ret;

	mali_vpu_sched_send_irq(&sess->sched_sess, &sess->vpu_dev->sched);
	return 0;
}

static int mali_vpu_session_send_drc_event(struct mali_vpu_session *sess)
{
	const struct v4l2_event event = {
		.type                 = V4L2_EVENT_SOURCE_CHANGE,
		.u.src_change.changes = V4L2_EVENT_SRC_CH_RESOLUTION
	};

	v4l2_event_queue_fh(&sess->fh, &event);
	return 0;
}

static int
mali_vpu_session_update_capture_format(struct mali_vpu_session *sess,
				       const struct mali_vpu_fw_alloc_param alloc_params)
{
	struct v4l2_pix_format_mplane *src = &sess->port_src.curr_fmt.fmt.pix_mp;
	struct v4l2_pix_format_mplane *dst = &sess->port_dst.curr_fmt.fmt.pix_mp;
	int ret = 0;

	src->width = alloc_params.width;
	src->height = alloc_params.height;

	ret = v4l2_fill_pixfmt_mp(dst, dst->pixelformat, alloc_params.width,
				  alloc_params.height);
	return ret;
}

static int mali_vpu_buffer_map_fw_region(struct mali_vpu_session *sess,
					 struct mali_vpu_buffer *vpu_buf)
{
	mali_vpu_mmu_va begin, end;
	enum mali_vpu_fw_region region;
	enum v4l2_buf_type type;
	int ret;

	type = vpu_buf->m2m_buf.vb.vb2_buf.type;

	if (mali_vpu_is_coded_q(sess, type))
		region = MALI_VPU_FW_REGION_PROTECTED;
	else
		region = MALI_VPU_FW_REGION_FRAMEBUF;

	ret = mali_vpu_fw_get_region(region, &begin, &end);
	if (ret)
		return ret;

	return mali_vpu_buffer_map(vpu_buf, begin, end);
}

static int mali_vpu_queue_buffer(struct mali_vpu_session *sess, struct mali_vpu_buffer *vpu_buf)
{
	int ret;
	enum dma_data_direction dma_dir;
	struct mali_vpu_fw_msg fw_msg;

	dma_dir = (vpu_buf->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) ?
		   DMA_TO_DEVICE : DMA_FROM_DEVICE;

	/* Map MMU buffer pages if it is not already mapped */
	if (!mali_vpu_is_buffer_mapped(vpu_buf)) {
		ret = mali_vpu_buffer_map_fw_region(sess, vpu_buf);
		if (ret)
			return ret;
	}

	if (!mali_vpu_is_coded_q(sess, vpu_buf->type) && sess->role == MALI_VPU_DECODER) {
		ret = mali_vpu_buffer_set_frame_layout(vpu_buf);
		if (ret)
			return ret;
	}

	/* dma sync for cache coherency */
	ret = mali_vpu_buffer_sync(vpu_buf, dma_dir);
	if (ret)
		return ret;

	/* put buf to fw q */
	fw_msg.code = MALI_VPU_FW_CODE_BUFFER;
	fw_msg.data.vpu_buf = vpu_buf;
	ret = mali_vpu_fw_send_buffer(&sess->fw, &fw_msg);
	if (ret)
		return ret;

	return 0;
}

static int mali_vpu_submit_deferred_bufs_for_queue(struct mali_vpu_session *sess,
						   enum v4l2_buf_type type)
{
	struct v4l2_m2m_buffer *buffer;
	struct mali_vpu_buffer *vpu_buf;
	int ret;

	if (!mali_vpu_session_get_port(sess, type)->stream_requested ||
	    !mali_vpu_session_can_stream(sess) ||
	    sess->fw_state != MALI_VPU_FW_STATE_RUNNING)
		return 0;

	if (V4L2_TYPE_IS_OUTPUT(type)) {
		v4l2_m2m_for_each_src_buf(sess->m2m_ctx, buffer) {
			vpu_buf = to_mali_vpu_buffer(&buffer->vb);
			if (vpu_buf->attr != MALI_VPU_BUF_ATTR_DEFERRED)
				continue;

			ret = mali_vpu_queue_buffer(sess, vpu_buf);
			if (ret)
				return ret;

			vpu_buf->attr = MALI_VPU_BUF_ATTR_QUEUED;
		}
	} else if (V4L2_TYPE_IS_CAPTURE(type)) {
		v4l2_m2m_for_each_dst_buf(sess->m2m_ctx, buffer) {
			vpu_buf = to_mali_vpu_buffer(&buffer->vb);
			if (vpu_buf->attr != MALI_VPU_BUF_ATTR_DEFERRED)
				continue;

			ret = mali_vpu_queue_buffer(sess, vpu_buf);
			if (ret)
				return ret;

			vpu_buf->attr = MALI_VPU_BUF_ATTR_QUEUED;
		}
	} else {
		return -EINVAL;
	}

	return 0;
}

int mali_vpu_submit_deferred_bufs(struct mali_vpu_session *sess)
{
	int ret;

	ret = mali_vpu_submit_deferred_bufs_for_queue(sess, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE);
	if (ret)
		goto err_submit;

	ret = mali_vpu_submit_deferred_bufs_for_queue(sess,
						      V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);
	if (ret)
		goto err_submit;

	mali_vpu_sched_send_irq(&sess->sched_sess, &sess->vpu_dev->sched);

	return 0;

err_submit:
	dev_err(sess->vpu_dev->dev, "failed to queue deferred buffers\n");
	return ret;
}

static int mali_vpu_session_sched_activate(struct mali_vpu_session *sess)
{
	lockdep_assert_held(&sess->vq_lock);

	if (sess->sched_sess.lsid)
		return 0;

	/* This maps LSID 0 and makes any staged IRQHOST visible. */
	return mali_vpu_sched_switch_in(&sess->sched_sess, &sess->vpu_dev->sched);
}

static void mali_vpu_session_store_ts_metadata(struct mali_vpu_session *sess,
					       const struct vb2_v4l2_buffer *vbuf)
{
	struct mali_vpu_ts_metadata *metadata;

	if (sess->ts_metadata_idx >= ARRAY_SIZE(sess->ts_metadata))
		sess->ts_metadata_idx = 0;

	metadata = &sess->ts_metadata[sess->ts_metadata_idx++];
	metadata->timestamp = vbuf->vb2_buf.timestamp;
	metadata->flags = vbuf->flags & MALI_VPU_TS_METADATA_FLAGS;
	metadata->valid = true;
	metadata->timecode = vbuf->timecode;
}

static void mali_vpu_session_restore_ts_metadata(struct mali_vpu_session *sess,
						 struct vb2_v4l2_buffer *vbuf)
{
	u64 timestamp = vbuf->vb2_buf.timestamp;
	u32 i;

	/*
	 * Matching the auxiliary metadata by timestamp is ambiguous when
	 * OUTPUT buffers have duplicate timestamps: timecode and timestamp-
	 * source flags may be restored from a different OUTPUT buffer.
	 *
	 * The timestamp itself is returned independently through firmware.
	 * Unique association tags require reliable metadata lifetime tracking.
	 */

	vbuf->flags &= ~MALI_VPU_TS_METADATA_FLAGS;

	for (i = 0; i < ARRAY_SIZE(sess->ts_metadata); i++) {
		struct mali_vpu_ts_metadata *metadata = &sess->ts_metadata[i];

		if (!metadata->valid || metadata->timestamp != timestamp)
			continue;

		vbuf->flags |= metadata->flags;
		if (metadata->flags & V4L2_BUF_FLAG_TIMECODE)
			vbuf->timecode = metadata->timecode;

		return;
	}
}

void mali_vpu_session_reset_ts_metadata(struct mali_vpu_session *sess)
{
	memset(sess->ts_metadata, 0, sizeof(sess->ts_metadata));
	sess->ts_metadata_idx = 0;
}

static void mali_vpu_session_buffer_done(struct mali_vpu_session *sess,
					 struct mali_vpu_buffer *vpu_buf,
					 enum vb2_buffer_state state)
{
	struct vb2_v4l2_buffer *vbuf = &vpu_buf->m2m_buf.vb;
	struct vb2_buffer *vb2_buf = &vbuf->vb2_buf;
	struct vb2_queue *q = v4l2_m2m_get_vq(sess->m2m_ctx, vb2_buf->type);
	struct mali_vpu_port *port;

	if (vb2_buf->state != VB2_BUF_STATE_ACTIVE) {
		dev_dbg(sess->vpu_dev->dev,
			"ignore returned buffer. type=%u index=%u vb2_state=%u\n",
			vpu_buf->type, vb2_buf->index, vb2_buf->state);
		return;
	}

	port = V4L2_TYPE_IS_OUTPUT(vpu_buf->type) ? &sess->port_src : &sess->port_dst;

	vbuf->sequence = port->sequence++;
	if (V4L2_TYPE_IS_CAPTURE(vpu_buf->type)) {
		vbuf->field = port->curr_fmt.fmt.pix_mp.field;
		mali_vpu_session_restore_ts_metadata(sess, vbuf);
	}

	if (V4L2_TYPE_IS_OUTPUT(vpu_buf->type))
		v4l2_m2m_src_buf_remove_by_buf(sess->m2m_ctx, vbuf);
	else
		v4l2_m2m_dst_buf_remove_by_buf(sess->m2m_ctx, vbuf);

	v4l2_m2m_buf_done(vbuf, state);

	if (V4L2_TYPE_IS_CAPTURE(vpu_buf->type) &&
	    !q->last_buffer_dequeued &&
	    (vbuf->flags & V4L2_BUF_FLAG_LAST) &&
	    sess->sess_state != MALI_VPU_DEC_STATE_DYNAMIC_RESOLUTION_CHANGE) {
		const struct v4l2_event event = {
			.type = V4L2_EVENT_EOS,
		};

		v4l2_event_queue_fh(&sess->fh, &event);
	}
}

static void mali_vpu_session_set_error(struct mali_vpu_session *sess, int error)
{
	struct vb2_queue *src_q;
	struct vb2_queue *dst_q;

	lockdep_assert_held(&sess->vq_lock);

	if (sess->error)
		return;

	sess->error = error;
	sess->fw_requested_state = MALI_VPU_FW_STATE_STOPPED;
	sess->fw_state = MALI_VPU_FW_STATE_STOPPED;

	mali_vpu_sched_session_terminate(&sess->sched_sess, &sess->vpu_dev->sched);

	src_q = v4l2_m2m_get_src_vq(sess->m2m_ctx);
	dst_q = v4l2_m2m_get_dst_vq(sess->m2m_ctx);

	vb2_queue_error(src_q);
	vb2_queue_error(dst_q);

	wake_up(&sess->fw_waitq);
}

static int mali_vpu_session_handle_fw_msg(struct mali_vpu_session *sess,
					  const struct mali_vpu_fw_msg *fw_msg)
{
	int ret = 0;
	switch (fw_msg->code) {
	case MALI_VPU_FW_CODE_ERROR:
		dev_warn(sess->vpu_dev->dev, "firmware rsp: Error. code=%u, message=%s\n",
			 fw_msg->data.error.error_code, fw_msg->data.error.message);
		return -EIO;
	case MALI_VPU_FW_CODE_JOB:
		/*
		 * The session uses an infinite job and does not implement
		 * switch-out scheduling.
		 */
		return 0;
	case MALI_VPU_FW_CODE_BUFFER: {
		struct mali_vpu_buffer *vpu_buf = fw_msg->data.vpu_buf;
		enum vb2_buffer_state state;

		dev_dbg(sess->vpu_dev->dev,
			"firmware rsp: Buffer. type=%u, len=[%u, %u, %u], flags=0x%08x, eos=%u",
			vpu_buf->type, vpu_buf->planes[0].payload_size,
			vpu_buf->planes[1].payload_size, vpu_buf->planes[2].payload_size,
			vpu_buf->flags, (vpu_buf->flags & MALI_VPU_BUFFER_EOS) != 0);

		if (vpu_buf->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE)
			mali_vpu_buffer_sync(vpu_buf, DMA_FROM_DEVICE);

		state = mali_vpu_buffer_update_info_to_v4l2(vpu_buf);

		mali_vpu_session_buffer_done(sess, vpu_buf, state);
		if (vpu_buf->flags & MALI_VPU_BUFFER_EOS) {
			if (V4L2_TYPE_IS_OUTPUT(vpu_buf->type) &&
			    sess->sess_state == MALI_VPU_STATE_STREAMING) {
				ret = mali_vpu_dec_update_state(sess, MALI_VPU_STATE_DRAIN);
				if (ret)
					return ret;
			}
		}
		return 0;
	}
	case MALI_VPU_FW_CODE_PONG:
		/* Receiving the responses completes the diagnostic ping. */
		return 0;
	case MALI_VPU_FW_CODE_STATE_CHANGE:
		sess->fw_state = fw_msg->data.state;

		if (sess->fw_state == MALI_VPU_FW_STATE_RUNNING)
			return mali_vpu_submit_deferred_bufs(sess);
		return 0;
	case MALI_VPU_FW_CODE_SWITCH_IN:
		/* Single session policy: the scheduler already tracks the active LSID mapping. */
		return 0;
	case MALI_VPU_FW_CODE_SWITCH_OUT:
		dev_warn(sess->vpu_dev->dev,
			 "Unexpected fw switch-out response for a single session policy.\n");
		return 0;
	case MALI_VPU_FW_CODE_SET_OPTION:
		/*
		 * The parser validated the confirmation and decremented the
		 * pending_response_count.
		 */
		return 0;
	case MALI_VPU_FW_CODE_SET_OPTION_FAIL:
		/*
		 * The parser printed the error and decremented the
		 * pending_response_count.
		 */
		return -EINVAL;
	case MALI_VPU_FW_CODE_IDLE:
		dev_dbg(sess->vpu_dev->dev, "fw entered Idle/sleeping state.\n");
		return 0;
	case MALI_VPU_FW_CODE_EOS:
		/*
		 * This is driver generated dummy EOS buffer response no action
		 * required as EOS is handled via buffer flags.
		 */
		return 0;
	case MALI_VPU_FW_CODE_INPUT_FLUSH:
		/*
		 * Acknowledge completion of the input flush. No additional session
		 * state update is required.
		 */
		return 0;
	case MALI_VPU_FW_CODE_COLOR_DESC:
		/* Colour metadata is currently not consumed by the driver. */
		return 0;
	case MALI_VPU_FW_CODE_DPB_HELD_FRAMES:
		/* DPB occupancy is informational. */
		dev_dbg(sess->vpu_dev->dev,
			"firmware holds %u frames in the DPB\n",
			fw_msg->data.dpb_held_frames);
		return 0;
	case MALI_VPU_FW_CODE_OUTPUT_FLUSH:
		if (sess->sess_state == MALI_VPU_DEC_STATE_DYNAMIC_RESOLUTION_CHANGE ||
		    sess->sess_state == MALI_VPU_DEC_STATE_FIRST_SOURCE_CHANGE) {
			return mali_vpu_session_send_drc_event(sess);
		}

		return 0;
	case MALI_VPU_FW_CODE_ALLOC_PARAM:
		ret = mali_vpu_session_update_capture_format(sess, fw_msg->data.alloc_params);
		if (ret)
			return ret;

		if (sess->sess_state == MALI_VPU_STATE_STREAMING) {
			ret =
			mali_vpu_dec_update_state(sess,
						  MALI_VPU_DEC_STATE_DYNAMIC_RESOLUTION_CHANGE);
			if (ret)
				return ret;
		} else if (sess->sess_state == MALI_VPU_STATE_STOPPED) {
			ret = mali_vpu_dec_update_state(sess,
							MALI_VPU_DEC_STATE_FIRST_SOURCE_CHANGE);
			if (ret)
				return ret;
		}

		return 0;
	case MALI_VPU_FW_CODE_SEQUENCE_PARAMS: {
		struct vb2_queue *dst_q;
		u32 buffers_min;

		if (fw_msg->data.seq_params.interlaced) {
			dev_err(sess->vpu_dev->dev, "interlaced streams are not supported\n");
			return -EOPNOTSUPP;
		}

		if (fw_msg->data.seq_params.bitdepth.luma != 8 ||
		    fw_msg->data.seq_params.bitdepth.chroma != 8) {
			dev_err(sess->vpu_dev->dev,
				"bitdepth luma=%u chroma=%u is not supported\n",
				fw_msg->data.seq_params.bitdepth.luma,
				fw_msg->data.seq_params.bitdepth.chroma);
			return -EOPNOTSUPP;
		}

		buffers_min = fw_msg->data.seq_params.planar.buffers_min;
		dst_q = v4l2_m2m_get_dst_vq(sess->m2m_ctx);

		if (!buffers_min || buffers_min > dst_q->max_num_buffers) {
			dev_err(sess->vpu_dev->dev,
				"invalid minimum buffer count %u, max is %u\n",
				buffers_min, dst_q->max_num_buffers);
			return -EPROTO;
		}

		sess->min_buffers_required = buffers_min;
		dst_q->min_reqbufs_allocation = sess->min_buffers_required;

		return mali_vpu_session_fw_send_flush(sess, false);
	}
	case MALI_VPU_FW_CODE_STREAM_NOT_SUPPORTED:
		return -EOPNOTSUPP;
	case MALI_VPU_FW_CODE_EVENT:
		/* No session action is required for informational events. */
		return 0;
	case MALI_VPU_FW_CODE_UNKNOWN:
	case MALI_VPU_FW_CODE_MAX:
	default:
		dev_warn(sess->vpu_dev->dev, "unknown fw msg code=%u\n", fw_msg->code);
		return -EPROTO;
	}
}

static int mali_vpu_session_wait_pending(struct mali_vpu_session *sess)
{
	long timeout;

	lockdep_assert_held(&sess->vq_lock);

	while (!sess->error &&
	       sess->fw_bin_state == MALI_VPU_SESSION_FW_BIN_READY &&
	       sess->fw.pending_response_count) {
		mutex_unlock(&sess->vq_lock);

		timeout = wait_event_timeout(sess->fw_waitq,
					     READ_ONCE(sess->error) ||
					     READ_ONCE(sess->fw_bin_state) !=
					     MALI_VPU_SESSION_FW_BIN_READY ||
					     !READ_ONCE(sess->fw.pending_response_count),
					     msecs_to_jiffies(mali_vpu_fw_response_timeout_ms));

		mutex_lock(&sess->vq_lock);

		if (sess->error)
			return sess->error;

		if (!timeout && sess->fw.pending_response_count)
			mali_vpu_session_set_error(sess, -ETIMEDOUT);
	}

	return sess->error;
}

static int mali_vpu_session_fw_init(struct mali_vpu_session *sess)
{
	int ret;

	lockdep_assert_held(&sess->vq_lock);

	if (sess->fw_requested_state != sess->fw_state) {
		if (sess->fw_requested_state != MALI_VPU_FW_STATE_RUNNING)
			return -EBUSY;

		return mali_vpu_session_sched_activate(sess);
	}

	if (sess->fw_state == MALI_VPU_FW_STATE_RUNNING)
		return 0;

	ret = mali_vpu_session_fw_send_job(sess, 0);
	if (ret)
		return ret;

	/*
	 * Map the LSID and allow the stopped firmware to process the
	 * queued SET_OPTION requests before it can be RUNNING.
	 */
	ret = mali_vpu_session_sched_activate(sess);
	if (ret)
		return ret;

	ret = mali_vpu_session_fw_send_ping(sess);
	if (ret)
		return ret;

	ret = mali_vpu_apply_staged_controls(sess);
	if (ret)
		return ret;

	/*
	 * SET_OPTION is only valid while stopped. Wait for pending option
	 * responses before requesting RUNNING.
	 */
	ret = mali_vpu_session_wait_pending(sess);
	if (ret)
		return ret;

	/* Waiting drops vq_lock, so streaming may have been withdrawn. */
	if (!mali_vpu_session_can_stream(sess))
		return 0;

	return mali_vpu_session_fw_send_state(sess, MALI_VPU_FW_STATE_RUNNING);
}

static int mali_vpu_session_fw_stop(struct mali_vpu_session *sess)
{
	int ret;

	lockdep_assert_held(&sess->vq_lock);

	ret = mali_vpu_session_wait_pending(sess);
	if (ret)
		return ret;

	if (sess->fw_state == MALI_VPU_FW_STATE_STOPPED)
		return 0;

	ret = mali_vpu_session_fw_send_state(sess, MALI_VPU_FW_STATE_STOPPED);
	if (ret)
		return ret;

	return mali_vpu_session_wait_pending(sess);
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

	sess->error = 0;
	sess->fw_bin = NULL;
	sess->port_src.stream_requested = false;
	sess->port_dst.stream_requested = false;
	init_completion(&sess->fw_load_done);
	init_waitqueue_head(&sess->fw_waitq);
	sess->fw_state = MALI_VPU_FW_STATE_STOPPED;
	sess->fw_requested_state = MALI_VPU_FW_STATE_STOPPED;

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

int mali_vpu_session_qbuf(struct mali_vpu_session *sess, struct vb2_buffer *vb2_buf)
{
	struct vb2_v4l2_buffer *v4l2_buf = to_vb2_v4l2_buffer(vb2_buf);
	struct mali_vpu_buffer *vpu_buf = to_mali_vpu_buffer(v4l2_buf);
	struct mali_vpu_port *port =
			mali_vpu_session_get_port(sess, vb2_buf->type);
	int ret;

	if (sess->error)
		return sess->error;

	/*
	 * After SEQUENCE_PARAMS and DRC fw has flushed the destination queue,
	 * but userspace can briefly requeue one of the returned buffer before
	 * it tears down the old destination buffer allocation with REQBUF(0).
	 * Do not give the requeued buffer back to fw as it belongs to pre DRC
	 * allocation which will be unmapped immediately afterwards.
	 */
	if (sess->sess_state == MALI_VPU_DEC_STATE_DYNAMIC_RESOLUTION_CHANGE &&
	    mali_vpu_is_raw_q(sess, vb2_buf->type) &&
	    port->stream_requested) {
		v4l2_buf->flags |= V4L2_BUF_FLAG_LAST;
		mali_vpu_session_buffer_done(sess, vpu_buf, VB2_BUF_STATE_DONE);

		return 0;
	}

	if (!port->stream_requested ||
	    !mali_vpu_session_can_stream(sess) ||
	    sess->fw_state != MALI_VPU_FW_STATE_RUNNING ||
	    (V4L2_TYPE_IS_OUTPUT(vb2_buf->type) &&
	    sess->sess_state == MALI_VPU_STATE_DRAIN)) {
		vpu_buf->attr = MALI_VPU_BUF_ATTR_DEFERRED;

		if (V4L2_TYPE_IS_OUTPUT(vb2_buf->type))
			mali_vpu_session_store_ts_metadata(sess, v4l2_buf);

		return 0;
	}

	ret = mali_vpu_queue_buffer(sess, vpu_buf);
	if (ret)
		return ret;

	vpu_buf->attr = MALI_VPU_BUF_ATTR_QUEUED;

	if (V4L2_TYPE_IS_OUTPUT(vb2_buf->type))
		mali_vpu_session_store_ts_metadata(sess, v4l2_buf);

	mali_vpu_sched_send_irq(&sess->sched_sess, &sess->vpu_dev->sched);

	return 0;
}

int mali_vpu_session_cleanup(struct mali_vpu_session *sess)
{
	struct mali_vpu_dev *vpu_dev = sess->vpu_dev;
	int fw_load_ret = 0;
	int fw_stop_ret = 0;
	int terminate_ret;
	bool waiter_cancelled = false;

	mutex_lock(&sess->vq_lock);

	sess->port_src.stream_requested = false;
	sess->port_dst.stream_requested = false;

	if (sess->fw_bin_state == MALI_VPU_SESSION_FW_BIN_LOADING ||
	    (sess->fw_bin_state == MALI_VPU_SESSION_FW_BIN_READY &&
	     !completion_done(&sess->fw_load_done))) {
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

	if (sess->fw_bin_state == MALI_VPU_SESSION_FW_BIN_READY)
		fw_stop_ret = mali_vpu_session_fw_stop(sess);

	terminate_ret = mali_vpu_sched_session_terminate(&sess->sched_sess,
							 &vpu_dev->sched);
	mutex_unlock(&sess->vq_lock);

	/*
	 * The LSID is no longer routed to this session. Drain a worker that
	 * may have resolved sess before it was detached.
	 */
	flush_work(&vpu_dev->irq_work);

	mutex_lock(&sess->vq_lock);

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

	if (fw_load_ret)
		return fw_load_ret;

	if (fw_stop_ret)
		return fw_stop_ret;

	return terminate_ret;
}

int mali_vpu_session_prepare_streaming(struct mali_vpu_session *sess,
				       enum v4l2_buf_type type)
{
	struct mali_vpu_port *port = mali_vpu_session_get_port(sess, type);
	enum mali_vpu_sess_state old_state;
	int ret;

	lockdep_assert_held(&sess->vq_lock);

	if (sess->error)
		return sess->error;

	if (port->stream_requested)
		return 0;

	if (sess->fw_bin_state == MALI_VPU_SESSION_FW_BIN_READY &&
	    sess->fw_requested_state != sess->fw_state &&
	    sess->fw_requested_state != MALI_VPU_FW_STATE_RUNNING)
		return -EBUSY;

	old_state = sess->sess_state;

	ret = mali_vpu_dec_state_change_streamon(sess, type);
	if (ret)
		return ret;

	port->stream_requested = true;

	if (sess->fw_bin_state == MALI_VPU_SESSION_FW_BIN_READY &&
	    mali_vpu_session_can_stream(sess)) {
		ret = mali_vpu_session_fw_init(sess);
		if (ret) {
			mali_vpu_session_set_error(sess, ret);
			goto err_rollback;
		}
	}

	port->sequence = 0;

	return 0;

err_rollback:

	port->stream_requested = false;
	sess->sess_state = old_state;

	return ret;
}

int mali_vpu_session_streamoff(struct mali_vpu_session *sess, enum v4l2_buf_type type)
{
	struct mali_vpu_port *port = mali_vpu_session_get_port(sess, type);
	bool drc_capture;
	int ret;

	lockdep_assert_held(&sess->vq_lock);

	if (!port->stream_requested)
		return 0;

	port->stream_requested = false;

	if (sess->error)
		return 0;

	drc_capture =
		sess->sess_state == MALI_VPU_DEC_STATE_DYNAMIC_RESOLUTION_CHANGE &&
		mali_vpu_is_raw_q(sess, type);

	ret = mali_vpu_dec_state_change_streamoff(sess, type);
	if (ret)
		goto err_session;

	if (sess->fw_bin_state != MALI_VPU_SESSION_FW_BIN_READY ||
	    !sess->sched_sess.lsid)
		return 0;

	if (drc_capture) {
		ret = mali_vpu_session_wait_pending(sess);
		if (ret)
			goto err_session;

		return 0;
	}

	ret = mali_vpu_session_fw_stop(sess);
	if (ret)
		goto err_session;

	ret = mali_vpu_session_fw_send_flush(sess, V4L2_TYPE_IS_OUTPUT(type));
	if (ret)
		goto err_session;

	ret = mali_vpu_session_wait_pending(sess);
	if (ret)
		goto err_session;

	return 0;

err_session:
	mali_vpu_session_set_error(sess, ret);

	return ret;
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
	if (sess->error ||
	    (!same_thread && mali_vpu_both_ports_unrequested(sess))) {
		sess->fw_bin_state = MALI_VPU_SESSION_FW_BIN_UNINITIALIZED;
		goto err_cache_put;
	}

	if (vpu_fw_bin->load_status) {
		sess->fw_bin_state = MALI_VPU_SESSION_FW_BIN_FAILED;
		mali_vpu_session_set_error(sess, vpu_fw_bin->load_status);
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
		mali_vpu_session_set_error(sess, ret);
		goto err_cache_put;
	}

	ret = mali_vpu_fw_construct(&sess->fw, sess->fw_bin, sess->vpu_dev->dev,
				    &sess->mmu, sess->ncores);
	if (ret) {
		dev_err(sess->vpu_dev->dev,
			"failed to initialise firmware: %d\n", ret);
		sess->fw_bin_state = MALI_VPU_SESSION_FW_BIN_FAILED;
		mali_vpu_session_set_error(sess, ret);
		goto err_cache_put;
	}

	sess->fw_bin_state = MALI_VPU_SESSION_FW_BIN_READY;

	/*
	 * A synchronous callback leaves startup to session preparation.
	 * A delayed callback completes startup itself.
	 */
	if (!same_thread && mali_vpu_session_can_stream(sess)) {
		ret = mali_vpu_session_fw_init(sess);
		if (ret) {
			dev_err(sess->vpu_dev->dev,
				"failed to start streaming: %d\n", ret);
			sess->fw_bin_state = MALI_VPU_SESSION_FW_BIN_UNINITIALIZED;
			mali_vpu_session_set_error(sess, ret);
			goto err_fw_cleanup;
		}
	}

	goto out_complete;

err_fw_cleanup:
	mali_vpu_fw_cleanup(&sess->fw);
	sess->fw_state = MALI_VPU_FW_STATE_STOPPED;
	sess->fw_requested_state = MALI_VPU_FW_STATE_STOPPED;

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

	if (sess->error)
		return sess->error;

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
		mali_vpu_session_set_error(sess, ret);
		complete_all(&sess->fw_load_done);
		return ret;
	}

	return 0;
}

void mali_vpu_session_irq(struct mali_vpu_session *sess)
{
	struct mali_vpu_fw_msg msg;
	bool flush_mmu = false;
	int ret;

	mutex_lock(&sess->vq_lock);

	if (sess->error)
		goto unlock_mutex;

	ret = mali_vpu_fw_handle_rpc(&sess->fw, &flush_mmu);
	if (ret) {
		if (flush_mmu)
			mali_vpu_sched_flush_mmu(&sess->sched_sess, &sess->vpu_dev->sched);
		mali_vpu_sched_send_irq(&sess->sched_sess, &sess->vpu_dev->sched);

		if (ret < 0) {
			mali_vpu_session_set_error(sess, ret);
			goto unlock_mutex;
		}
	}

	do {
		ret = mali_vpu_fw_parse_msg(&sess->fw, &msg);
		if (ret < 0) {
			mali_vpu_session_set_error(sess, ret);
			goto unlock_mutex;
		}

		if (!ret)
			break;

		ret = mali_vpu_session_handle_fw_msg(sess, &msg);
		if (ret < 0) {
			mali_vpu_session_set_error(sess, ret);
			goto unlock_mutex;
		}
	} while (ret >= 0);

	wake_up(&sess->fw_waitq);

unlock_mutex:
	mutex_unlock(&sess->vq_lock);
}
