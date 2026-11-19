/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2026 ARM Limited.
 */

#ifndef __MALI_VPU_SESSION_H__
#define __MALI_VPU_SESSION_H__

#include <media/v4l2-fh.h>

#include "mali-vpu-device.h"

/**
 * struct mali_vpu_session - holds per session state and resources.
 * @vpu_dev: Reference to the parent device structure.
 * @role: Role of this session.
 * @fh: V4L2 file handle for this session.
 */
struct mali_vpu_session {
	struct mali_vpu_dev		*vpu_dev;
	enum mali_vpu_session_role	role;

	struct v4l2_fh		fh;
};

#endif /* __MALI_VPU_SESSION_H__ */
