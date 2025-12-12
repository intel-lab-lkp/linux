/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2025, Silicon Laboratories, Inc.
 */

#ifndef __CPC_HOST_H
#define __CPC_HOST_H

#include <linux/device.h>
#include <linux/greybus.h>
#include <linux/types.h>

#define GB_CPC_MSG_SIZE_MAX 4096
#define GB_CPC_NUM_CPORTS 8

struct cpc_host_device;

struct cpc_hd_driver {
	int (*message_send)(struct cpc_host_device *hd, u16 dest_cport_id,
			    struct gb_message *message, gfp_t gfp_mask);
	void (*message_cancel)(struct gb_message *message);
};

/**
 * struct cpc_host_device - CPC host device.
 * @gb_hd: pointer to Greybus Host Device this device belongs to.
 * @driver: driver operations.
 */
struct cpc_host_device {
	struct gb_host_device *gb_hd;
	const struct cpc_hd_driver *driver;
};

struct cpc_host_device *cpc_hd_create(struct cpc_hd_driver *driver, struct device *parent);
int cpc_hd_add(struct cpc_host_device *cpc_hd);
void cpc_hd_put(struct cpc_host_device *cpc_hd);
void cpc_hd_del(struct cpc_host_device *cpc_hd);
void cpc_hd_rcvd(struct cpc_host_device *cpc_hd, u16 cport_id, u8 *data, size_t length);

#endif
