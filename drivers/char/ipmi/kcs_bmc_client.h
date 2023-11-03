/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (c) 2021, IBM Corp. */

#ifndef __KCS_BMC_CONSUMER_H__
#define __KCS_BMC_CONSUMER_H__

#include <linux/irqreturn.h>
#include <linux/module.h>

#include "kcs_bmc.h"

struct kcs_bmc_driver;

struct kcs_bmc_client_ops {
	irqreturn_t (*event)(struct kcs_bmc_client *client);
};

struct kcs_bmc_client {
	const struct kcs_bmc_client_ops *ops;

	struct kcs_bmc_driver *drv;
	struct kcs_bmc_device *dev;
	struct list_head entry;
};

struct kcs_bmc_driver_ops {
	struct kcs_bmc_client *(*add_device)(struct kcs_bmc_driver *drv,
					     struct kcs_bmc_device *dev);
	void (*remove_device)(struct kcs_bmc_client *client);
};

static inline void kcs_bmc_client_init(struct kcs_bmc_client *client,
				       const struct kcs_bmc_client_ops *ops,
				       struct kcs_bmc_driver *drv,
				       struct kcs_bmc_device *dev)
{
	client->ops = ops;
	client->drv = drv;
	client->dev = dev;
}

struct kcs_bmc_driver {
	struct list_head entry;

	const struct kcs_bmc_driver_ops *ops;
};

int kcs_bmc_register_driver(struct kcs_bmc_driver *drv);
void kcs_bmc_unregister_driver(struct kcs_bmc_driver *drv);

#define module_kcs_bmc_driver(__kcs_bmc_driver) \
	module_driver(__kcs_bmc_driver, kcs_bmc_register_driver, \
		kcs_bmc_unregister_driver)

int kcs_bmc_enable_device(struct kcs_bmc_client *client);
void kcs_bmc_disable_device(struct kcs_bmc_client *client);
u8 kcs_bmc_read_data(struct kcs_bmc_client *client);
void kcs_bmc_write_data(struct kcs_bmc_client *client, u8 data);
u8 kcs_bmc_read_status(struct kcs_bmc_client *client);
void kcs_bmc_write_status(struct kcs_bmc_client *client, u8 data);
void kcs_bmc_update_status(struct kcs_bmc_client *client, u8 mask, u8 val);
#endif
