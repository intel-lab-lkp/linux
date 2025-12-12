// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2025, Silicon Laboratories, Inc.
 */

#include <linux/err.h>
#include <linux/greybus.h>
#include <linux/module.h>

#include "host.h"

static struct cpc_host_device *gb_hd_to_cpc_hd(struct gb_host_device *hd)
{
	return (struct cpc_host_device *)&hd->hd_priv;
}

static int cpc_gb_message_send(struct gb_host_device *gb_hd, u16 cport_id,
			       struct gb_message *message, gfp_t gfp_mask)
{
	struct cpc_host_device *cpc_hd = gb_hd_to_cpc_hd(gb_hd);

	return cpc_hd->driver->message_send(cpc_hd, cport_id, message, gfp_mask);
}

static void cpc_gb_message_cancel(struct gb_message *message)
{
	/* Not implemented */
}

static struct gb_hd_driver cpc_gb_driver = {
	.hd_priv_size = sizeof(struct cpc_host_device),
	.message_send = cpc_gb_message_send,
	.message_cancel = cpc_gb_message_cancel,
};

struct cpc_host_device *cpc_hd_create(struct cpc_hd_driver *driver, struct device *parent)
{
	struct cpc_host_device *cpc_hd;
	struct gb_host_device *hd;

	if ((!driver->message_send) || (!driver->message_cancel)) {
		dev_err(parent, "missing mandatory callbacks\n");
		return ERR_PTR(-EINVAL);
	}

	hd = gb_hd_create(&cpc_gb_driver, parent, GB_CPC_MSG_SIZE_MAX, GB_CPC_NUM_CPORTS);
	if (IS_ERR(hd))
		return (struct cpc_host_device *)hd;

	cpc_hd = gb_hd_to_cpc_hd(hd);
	cpc_hd->gb_hd = hd;
	cpc_hd->driver = driver;

	return cpc_hd;
}
EXPORT_SYMBOL_GPL(cpc_hd_create);

int cpc_hd_add(struct cpc_host_device *cpc_hd)
{
	return gb_hd_add(cpc_hd->gb_hd);
}
EXPORT_SYMBOL_GPL(cpc_hd_add);

void cpc_hd_put(struct cpc_host_device *cpc_hd)
{
	return gb_hd_put(cpc_hd->gb_hd);
}
EXPORT_SYMBOL_GPL(cpc_hd_put);

void cpc_hd_del(struct cpc_host_device *cpc_hd)
{
	return gb_hd_del(cpc_hd->gb_hd);
}
EXPORT_SYMBOL_GPL(cpc_hd_del);

void cpc_hd_rcvd(struct cpc_host_device *cpc_hd, u16 cport_id, u8 *data, size_t length)
{
	greybus_data_rcvd(cpc_hd->gb_hd, cport_id, data, length);
}
EXPORT_SYMBOL_GPL(cpc_hd_rcvd);

MODULE_DESCRIPTION("Greybus over CPC");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Silicon Laboratories, Inc.");
