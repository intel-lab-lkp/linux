/* SPDX-License-Identifier: GPL-2.0 */
/*
 * This file is part of ILITEK CommonFlow
 *
 * Copyright (c) 2022 ILI Technology Corp.
 * Copyright (c) 2022 Luca Hsu <luca_hsu@ilitek.com>
 * Copyright (c) 2022 Joe Hung <joe_hung@ilitek.com>
 */

#ifndef __ILITEK_REPORT_H__
#define __ILITEK_REPORT_H__

#include "ilitek_def.h"
#include "ilitek_protocol.h"

#ifdef _WIN32
/* packed below structures by 1 byte */
#pragma pack(1)
#endif

struct __PACKED__ touch_data {
	struct touch_fmt finger[40];

	u8 cnt;
	u8 algo;
	u8 dbg[64];
	u32 dbg_size;
};

struct __PACKED__ pen_data {
	struct pen_fmt pen;

	u8 cnt;
	u8 algo;
	u8 dbg[64];
	u32 dbg_size;
};

#ifdef _WIN32
#pragma pack()
#endif

/* return touch event */
typedef void(*report_touch_event_t)(struct touch_data *, void *);
/* return pen event */
typedef void(*report_pen_event_t)(struct pen_data *, void *);
/* return debug msg */
typedef void(*report_dmsg_t)(char *, int, void *);
/* return raw data buf */
typedef void(*report_buf_t)(u8 *, int, bool, void *);

struct ilitek_report_callback {
	report_touch_event_t report_touch_event;
	report_pen_event_t report_pen_event;
	report_dmsg_t report_dmsg;
	report_buf_t report_buf;
};

struct ilitek_report {
	struct touch_data touch;
	struct pen_data pen;
	struct ilitek_report_callback cb;

	bool skip_checksum;

	void *_private;
};

#ifdef __cplusplus
extern "C" {
#endif
	int __DLL ilitek_report_update(void *handle, struct ilitek_report *report);

#ifdef __cplusplus
}
#endif

#endif
