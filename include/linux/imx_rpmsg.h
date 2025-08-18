/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2025 NXP.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

/*
 * @file linux/imx_rpmsg.h
 *
 * @brief Global header file for iMX RPMSG
 *
 * @ingroup RPMSG
 */
#ifndef __LINUX_IMX_RPMSG_H__
#define __LINUX_IMX_RPMSG_H__

#include <linux/completion.h>
#include <linux/mutex.h>

/* Category define */
#define IMX_RMPSG_LIFECYCLE	1
#define IMX_RPMSG_PMIC		2
#define IMX_RPMSG_AUDIO		3
#define IMX_RPMSG_KEY		4
#define IMX_RPMSG_GPIO		5
#define IMX_RPMSG_RTC		6
#define IMX_RPMSG_SENSOR	7
/* rpmsg version */
#define IMX_RMPSG_MAJOR		1
#define IMX_RMPSG_MINOR		0

#define MAX_DEV_PER_CHANNEL	10

struct imx_rpmsg_head {
	u8 cate;
	u8 major;
	u8 minor;
	u8 type;
	u8 cmd;
	u8 reserved[5];
} __packed;

struct imx_rpmsg_driver_data {
	int map_idx;
	const char *rproc_name;
	struct rpmsg_device *rpdev;
	struct device_node *channel_node;
	int (*rx_callback)(struct rpmsg_device *, void *, int, void *, u32);
	void *channel_devices[MAX_DEV_PER_CHANNEL];
};

#endif /* __LINUX_IMX_RPMSG_H__ */
