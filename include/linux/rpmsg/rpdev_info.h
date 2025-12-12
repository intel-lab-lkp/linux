/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright 2025 NXP */

/*
 * @file linux/rpdev_info.h
 *
 * @brief Global header file for RPDEV Info
 *
 * @ingroup RPMSG
 */
#ifndef __LINUX_RPDEV_INFO_H__
#define __LINUX_RPDEV_INFO_H__

#define MAX_DEV_PER_CHANNEL    10

/**
 * rpdev_platform_info - store the platform information of rpdev
 * @rproc_name: the name of the remote proc.
 * @rpdev: rpmsg channel device
 * @device_node: pointer to the device node of the rpdev.
 * @rx_callback: rx callback handler of the rpdev.
 * @channel_devices: an array of the devices related to the rpdev.
 */
struct rpdev_platform_info {
	const char *rproc_name;
	struct rpmsg_device *rpdev;
	struct device_node *channel_node;
	int (*rx_callback)(struct rpmsg_device *rpdev, void *data,
			   int len, void *priv, u32 src);
	void *channel_devices[MAX_DEV_PER_CHANNEL];
};

#endif /* __LINUX_RPDEV_INFO_H__ */
