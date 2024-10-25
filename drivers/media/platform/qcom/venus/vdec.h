/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2012-2016, The Linux Foundation. All rights reserved.
 * Copyright (C) 2017 Linaro Ltd.
 */
#ifndef __VENUS_VDEC_H__
#define __VENUS_VDEC_H__

#include <media/v4l2-ctrls.h>

struct venus_inst;

int vdec_ctrl_init(struct venus_inst *inst);
static inline void vdec_ctrl_deinit(struct venus_inst *inst)
{
	v4l2_ctrl_handler_free(&inst->ctrl_handler);
}

#endif
