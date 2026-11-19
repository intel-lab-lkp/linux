/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2026 ARM Limited.
 */

#ifndef __MALI_VPU_DEC_H__
#define __MALI_VPU_DEC_H__

#include <linux/types.h>
#include <linux/videodev2.h>

struct file;
struct mali_vpu_session;
struct v4l2_decoder_cmd;

void mali_vpu_session_init_dec(struct mali_vpu_session *sess);

/* Decoder controls setters */
int mali_vpu_ctrl_set_ignore_stream_headers(struct mali_vpu_session *sess, u32 cid, s64 value);
int mali_vpu_ctrl_set_frame_reordering(struct mali_vpu_session *sess, u32 cid, s64 value);

int mali_vpu_decoder_cmd(struct file *filp, void *priv, struct v4l2_decoder_cmd *cmd);
int mali_vpu_dec_update_state(struct mali_vpu_session *sess, u32 new_state);
int mali_vpu_dec_state_change_streamon(struct mali_vpu_session *sess, enum v4l2_buf_type type);
int mali_vpu_dec_state_change_streamoff(struct mali_vpu_session *sess, enum v4l2_buf_type type);

#endif /* __MALI_VPU_DEC_H__ */
