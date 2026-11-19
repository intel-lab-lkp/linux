/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2026 ARM Limited.
 */

#ifndef __MALI_VPU_SESSION_H__
#define __MALI_VPU_SESSION_H__

#include <linux/completion.h>
#include <linux/mutex.h>
#include <linux/videodev2.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-fh.h>

#include "mali-vpu-buffer.h"
#include "mali-vpu-device.h"
#include "mali-vpu-firmware.h"
#include "mali-vpu-formats.h"
#include "mali-vpu-mmu.h"

struct v4l2_m2m_ctx;
struct mali_vpu_fw_bin;
struct mali_vpu_fw_event;
struct mali_vpu_sched_session;

/**
 * struct mali_vpu_port - format state of a src/dst queue in a session.
 * @curr_fmt: V4L2 format currently configured for the port
 * @supported_fmts: List of format descriptors supported by the queue.
 * @stream_requested: Port/Queue is being prepared for streaming or has
 *                    successfully completed preparation. Protected by vq_lock.
 */
struct mali_vpu_port {
	struct v4l2_format		curr_fmt;
	struct mali_vpu_format_list	supported_fmts;
	bool				stream_requested;
};

/*
 * Firmware binary load lifecycle
 *
 * Preparing the coded queue starts an asynchronous firmware request.
 * The binary request does not require queued buffers and does not wait for
 * loading to complete. READY and LOADING requests are reused.
 *
 * A cache hit may invoke the callback synchronously. An asynchronous callback
 * discards its result if neither port requests streaming. Successful callbacks
 * construct the firmware address space before making the binary READY. The
 * session retains its binary reference until cleanup.
 *
 * A synchronous callback leaves firmware startup to session preparation.
 * An asynchronous callback starts firmware if the required ports still request
 * streaming: OUTPUT for a decoder, both ports for an encoder.
 * Binary READY and firmware RUNNING are separate states.
 *
 * Buffers received before the firmware confirms RUNNING are deferred. The
 * RUNNING response submits those buffers. Buffers received afterwards are
 * submitted directly.
 *
 * Session cleanup waits for an in-flight callback before releasing the
 * session-owned binary reference and callback event. On timeout, it cancels
 * the pending cache waiter or waits for an already claimed callback.
 */
enum mali_vpu_session_fw_bin_state {
	MALI_VPU_SESSION_FW_BIN_UNINITIALIZED = 0,
	MALI_VPU_SESSION_FW_BIN_LOADING,
	MALI_VPU_SESSION_FW_BIN_READY,
	MALI_VPU_SESSION_FW_BIN_FAILED,
};

/**
 * struct mali_vpu_session - holds per session state and resources.
 * @vpu_dev: Reference to the parent device structure.
 * @role: Role of this session.
 * @port_src: Source (OUTPUT) port for this session.
 * @port_dst: Destination (CAPTURE) port for this session.
 * @mmu: MMU handle for this session
 * @vq_lock: Mutex lock to serialise per-session queue related ioctls.
 * @min_buffers_required: Driver-reported minimum buffers for role-specific queue.
 * @ctrls_caps: Variant-provided immutable control capability table.
 * @num_ctrls_caps: Number of entries in @ctrls_caps.
 * @ctrls_state: Per-session control cache and staging state.
 * @ctrl_handler: V4L2 control handler bound to this session.
 * @fh: V4L2 file handle for this session.
 * @m2m_ctx: V4L2 mem2mem context for this session.
 * @fw: Per-session firmware address space object.
 * @fw_bin: Pointer to firmware binary loaded for this session.
 * @fw_event: Firmware binary load event callback structure for this session.
 * @fw_bin_state: Session firmware binary status.
 * @fw_load_done: Completion used to synchronize teardown with an in-flight firmware load.
 * @fw_waitq: Wait queue used while waiting for firmware responses.
 * @fw_state: Last state confirmed by the firmware.
 * @fw_requested_state: Last firmware state requested by the driver.
 * @sched_sess: Scheduler state used to map this session to an LSID.
 * @ncores: Number of cores assigned to this session by scheduler.
 */
struct mali_vpu_session {
	struct mali_vpu_dev		*vpu_dev;
	enum mali_vpu_session_role	role;
	struct mali_vpu_port		port_src;
	struct mali_vpu_port		port_dst;
	struct mali_vpu_mmu		mmu;
	struct mutex			vq_lock; /* lock to serialise per-session ioctls */
	u32				min_buffers_required;

	const struct mali_vpu_ctrl_desc *ctrls_caps;
	u32				num_ctrls_caps;
	struct mali_vpu_ctrl_state	*ctrls_state;
	struct v4l2_ctrl_handler	ctrl_handler;

	struct v4l2_fh		fh;
	struct v4l2_m2m_ctx	*m2m_ctx;

	struct mali_vpu_fw			fw;
	struct mali_vpu_fw_bin			*fw_bin;
	struct mali_vpu_fw_event		*fw_event;
	enum mali_vpu_session_fw_bin_state	fw_bin_state;
	struct completion			fw_load_done;
	wait_queue_head_t			fw_waitq;
	enum mali_vpu_fw_state			fw_state;
	enum mali_vpu_fw_state			fw_requested_state;

	struct mali_vpu_sched_session	sched_sess;
	u32				ncores;
};

int mali_vpu_session_init(struct mali_vpu_session *sess);
int mali_vpu_session_cleanup(struct mali_vpu_session *sess);
int mali_vpu_session_request_fw_load(struct mali_vpu_session *sess);
int mali_vpu_session_fw_send_set_option(struct mali_vpu_session *sess,
					struct mali_vpu_fw_set_option *opt);
int mali_vpu_session_prepare_streaming(struct mali_vpu_session *sess,
				       enum v4l2_buf_type type);
int mali_vpu_session_streamoff(struct mali_vpu_session *sess, enum v4l2_buf_type type);
void mali_vpu_session_irq(struct mali_vpu_session *sess);
int mali_vpu_session_qbuf(struct mali_vpu_session *sess, struct vb2_buffer *vb2_buf);

static inline struct mali_vpu_session *mali_vpu_get_session(struct file *filp)
{
	return container_of(file_to_v4l2_fh(filp), struct mali_vpu_session, fh);
}

#endif /* __MALI_VPU_SESSION_H__ */
