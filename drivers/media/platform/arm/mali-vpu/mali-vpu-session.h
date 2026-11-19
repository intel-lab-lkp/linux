/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2026 ARM Limited.
 */

#ifndef __MALI_VPU_SESSION_H__
#define __MALI_VPU_SESSION_H__

#include <linux/mutex.h>
#include <linux/videodev2.h>
#include <media/v4l2-fh.h>

#include "mali-vpu-device.h"
#include "mali-vpu-formats.h"

struct v4l2_m2m_ctx;

/**
 * struct mali_vpu_port - format state of a src/dst queue in a session.
 * @curr_fmt: V4L2 format currently configured for the port
 * @supported_fmts: List of format descriptors supported by the queue.
 */
struct mali_vpu_port {
	struct v4l2_format		curr_fmt;
	struct mali_vpu_format_list	supported_fmts;
};

/**
 * struct mali_vpu_session - holds per session state and resources.
 * @vpu_dev: Reference to the parent device structure.
 * @role: Role of this session.
 * @port_src: Source (OUTPUT) port for this session.
 * @port_dst: Destination (CAPTURE) port for this session.
 * @vq_lock: Mutex lock to serialise per-session queue related ioctls.
 * @fh: V4L2 file handle for this session.
 * @m2m_ctx: V4L2 mem2mem context for this session.
 */
struct mali_vpu_session {
	struct mali_vpu_dev		*vpu_dev;
	enum mali_vpu_session_role	role;
	struct mali_vpu_port		port_src;
	struct mali_vpu_port		port_dst;
	struct mutex			vq_lock; /* lock to serialise per-session ioctls */

	struct v4l2_fh		fh;
	struct v4l2_m2m_ctx	*m2m_ctx;
};

int mali_vpu_session_init(struct mali_vpu_session *sess);
void mali_vpu_session_cleanup(struct mali_vpu_session *sess);

#endif /* __MALI_VPU_SESSION_H__ */
