/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2026 ARM Limited.
 */

#ifndef __MALI_VPU_DEC_H__
#define __MALI_VPU_DEC_H__

#include <linux/types.h>

struct mali_vpu_session;

void mali_vpu_session_init_dec(struct mali_vpu_session *sess);

/* Decoder controls setters */
int mali_vpu_ctrl_set_ignore_stream_headers(struct mali_vpu_session *sess, u32 cid, s64 value);
int mali_vpu_ctrl_set_frame_reordering(struct mali_vpu_session *sess, u32 cid, s64 value);

#endif /* __MALI_VPU_DEC_H__ */
