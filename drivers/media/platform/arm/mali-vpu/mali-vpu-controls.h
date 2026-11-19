/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2026 ARM Limited.
 */

#ifndef __MALI_VPU_CONTROLS_H__
#define __MALI_VPU_CONTROLS_H__

#include <linux/bits.h>
#include <linux/types.h>
#include <media/v4l2-ctrls.h>

struct mali_vpu_session;

enum mali_vpu_ctrl_flags {
	MALI_VPU_CTRL_FLAG_DYNAMIC	= BIT(0),
};

/**
 * struct mali_vpu_ctrl_desc - static description of one driver control.
 * @cid: V4L2 control ID.
 * @type: V4L2 control type.
 * @name: Control name used for custom registration. NULL for standard controls.
 * @qmenu: Menu labels for V4L2_CTRL_TYPE_MENU custom controls.
 * @min_val: Minimum allowed value for this control.
 * @max_val: Maximum allowed value for this control.
 * @default_val: Default value for this control at session init.
 * @step_or_mask: Step for integer controls, or enabled-item mask for menu controls.
 * @is_custom: Flag to indicate if a control is a custom Mali VPU control.
 * @flags: Driver-specific behavior flags from enum mali_vpu_ctrl_flags.
 * @set: Setter callback invoked on client control changes, to apply value to firmware.
 */
struct mali_vpu_ctrl_desc {
	u32 cid;
	enum v4l2_ctrl_type type;
	const char *name;
	const char * const *qmenu;
	s64 min_val;
	s64 max_val;
	s64 default_val;
	u64 step_or_mask;
	bool is_custom;
	enum mali_vpu_ctrl_flags flags;
	int (*set)(struct mali_vpu_session *sess, u32 cid, s64 value);
};

/**
 * struct mali_vpu_ctrl_state - per-session runtime state of one driver control.
 * @cid: V4L2 control ID.
 * @value: Last accepted value for the control (from userspace or default at init).
 * @client_set: True if userspace explicitly set this control at least once.
 * @is_staged: True if value is accepted but not yet sent to firmware.
 */
struct mali_vpu_ctrl_state {
	u32 cid;
	s64 value;
	bool client_set;
	bool is_staged;
};

int mali_vpu_ctrls_init(struct mali_vpu_session *sess);
void mali_vpu_ctrls_cleanup(struct mali_vpu_session *sess);
int mali_vpu_apply_staged_controls(struct mali_vpu_session *sess);

/* Common controls setters */
int mali_vpu_ctrl_set_nalu_format(struct mali_vpu_session *sess, u32 cid, s64 value);
int mali_vpu_ctrl_set_intbuf_size(struct mali_vpu_session *sess, u32 cid, s64 value);
int mali_vpu_ctrl_set_stream_escaping(struct mali_vpu_session *sess, u32 cid, s64 value);
int mali_vpu_ctrl_set_host_print_enable(struct mali_vpu_session *sess, u32 cid, s64 value);

#endif /* __MALI_VPU_CONTROLS_H__ */
