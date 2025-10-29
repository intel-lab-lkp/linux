/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright 2025 NXP */

/*
 * @file linux/imx_rpmsg.h
 *
 * @brief Global header file for iMX RPMSG
 *
 * @ingroup RPMSG
 */
#ifndef __LINUX_IMX_RPMSG_H__
#define __LINUX_IMX_RPMSG_H__

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
	u8 cate;	/* Category */
	u8 major;	/* Major version */
	u8 minor;	/* Minor version */
	u8 type;	/* Message type */
	u8 cmd;		/* Command code */
	u8 reserved[5];
} __packed;

struct imx_rpmsg_driver_data {
	int map_idx;
	const char *rproc_name;
	struct rpmsg_device *rpdev;
	struct device_node *channel_node;
	int (*rx_callback)(struct rpmsg_device *rpdev, void *data,
			   int len, void *priv, u32 src);
	void *channel_devices[MAX_DEV_PER_CHANNEL];
};

#endif /* __LINUX_IMX_RPMSG_H__ */
