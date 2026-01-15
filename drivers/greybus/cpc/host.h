/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2025, Silicon Laboratories, Inc.
 */

#ifndef __CPC_HOST_H
#define __CPC_HOST_H

#include <linux/device.h>
#include <linux/greybus.h>
#include <linux/mutex.h>
#include <linux/types.h>

#define GB_CPC_MSG_SIZE_MAX 4096
#define GB_CPC_NUM_CPORTS 8

struct cpc_cport;
struct cpc_host_device;

struct cpc_hd_driver {
	int (*transmit)(struct cpc_host_device *hd, struct sk_buff *skb);
};

/**
 * struct cpc_host_device - CPC host device.
 * @gb_hd: pointer to Greybus Host Device this device belongs to.
 * @driver: driver operations.
 * @lock: mutex to synchronize access to cport array.
 * @cports: array of cport pointers allocated by Greybus core.
 */
struct cpc_host_device {
	struct gb_host_device *gb_hd;
	const struct cpc_hd_driver *driver;

	struct mutex lock; /* Synchronize access to cports */
	struct cpc_cport *cports[GB_CPC_NUM_CPORTS];
};

static inline struct device *cpc_hd_dev(struct cpc_host_device *cpc_hd)
{
	return &cpc_hd->gb_hd->dev;
}

struct cpc_host_device *cpc_hd_create(struct cpc_hd_driver *driver, struct device *parent);
int cpc_hd_add(struct cpc_host_device *cpc_hd);
void cpc_hd_put(struct cpc_host_device *cpc_hd);
void cpc_hd_del(struct cpc_host_device *cpc_hd);
void cpc_hd_rcvd(struct cpc_host_device *cpc_hd, struct sk_buff *skb);
void cpc_hd_message_sent(struct sk_buff *skb, int status);

int cpc_hd_send_skb(struct cpc_host_device *cpc_hd, struct sk_buff *skb);

#endif
