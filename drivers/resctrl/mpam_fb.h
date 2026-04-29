/* SPDX-License-Identifier: GPL-2.0 */
// Copyright (C) 2024-2025 Arm Ltd.

#ifndef MPAM_FB_H_
#define MPAM_FB_H_

#include <linux/types.h>
#include "mpam_internal.h"

#define SCMI_MSG_PAYLOAD_OFS	0x1c
#define MPAM_WRITE_MSG_SIZE	(SCMI_MSG_PAYLOAD_OFS + 4 * sizeof(u32))
#define MPAM_FB_MAX_MSG_SIZE	MPAM_WRITE_MSG_SIZE

int mpam_fb_send_read_request(struct mpam_msc *msc, u16 reg, u32 *result);
int mpam_fb_send_write_request(struct mpam_msc *msc, u16 reg, u32 value);

#endif
