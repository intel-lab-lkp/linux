// SPDX-License-Identifier: GPL-2.0-or-later
/* Copyright (c) 2021 IBM Corp. */

#include <linux/delay.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/sched/signal.h>
#include <linux/serio.h>
#include <linux/slab.h>

#include "kcs_bmc_client.h"

struct kcs_bmc_serio {
	struct kcs_bmc_client client;
	struct serio *port;

	spinlock_t lock;
};

static inline struct kcs_bmc_serio *client_to_kcs_bmc_serio(struct kcs_bmc_client *client)
{
	return container_of(client, struct kcs_bmc_serio, client);
}

static irqreturn_t kcs_bmc_serio_event(struct kcs_bmc_client *client)
{
	struct kcs_bmc_serio *priv;
	u8 handled = IRQ_NONE;
	u8 status;

	priv = client_to_kcs_bmc_serio(client);

	spin_lock(&priv->lock);

	status = kcs_bmc_read_status(client);

	if (status & KCS_BMC_STR_IBF)
		handled = serio_interrupt(priv->port, kcs_bmc_read_data(client), 0);

	spin_unlock(&priv->lock);

	return handled;
}

static const struct kcs_bmc_client_ops kcs_bmc_serio_client_ops = {
	.event = kcs_bmc_serio_event,
};

static int kcs_bmc_serio_open(struct serio *port)
{
	struct kcs_bmc_serio *priv = port->port_data;

	return kcs_bmc_enable_device(&priv->client);
}

static void kcs_bmc_serio_close(struct serio *port)
{
	struct kcs_bmc_serio *priv = port->port_data;

	kcs_bmc_disable_device(&priv->client);
}

static struct kcs_bmc_client *
kcs_bmc_serio_add_device(struct kcs_bmc_driver *drv, struct kcs_bmc_device *dev)
{
	struct kcs_bmc_serio *priv;
	struct serio *port;
	int rc;

	priv = kzalloc(sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return ERR_PTR(ENOMEM);

	/* Use kzalloc() as the allocation is cleaned up with kfree() via serio_unregister_port() */
	port = kzalloc(sizeof(*port), GFP_KERNEL);
	if (!port) {
		rc = ENOMEM;
		goto cleanup_priv;
	}

	port->id.type = SERIO_8042;
	port->open = kcs_bmc_serio_open;
	port->close = kcs_bmc_serio_close;
	port->port_data = priv;
	port->dev.parent = dev->dev;

	spin_lock_init(&priv->lock);
	priv->port = port;

	kcs_bmc_client_init(&priv->client, &kcs_bmc_serio_client_ops, drv, dev);

	serio_register_port(port);

	pr_info("Initialised serio client for channel %d\n", dev->channel);

	return &priv->client;

cleanup_priv:
	kfree(priv);

	return ERR_PTR(rc);
}

static void kcs_bmc_serio_remove_device(struct kcs_bmc_client *client)
{
	struct kcs_bmc_serio *priv = client_to_kcs_bmc_serio(client);

	/* kfree()s priv->port via put_device() */
	serio_unregister_port(priv->port);
	/* Ensure the IBF IRQ is disabled if we were the active client */
	kcs_bmc_disable_device(&priv->client);
	kfree(priv);
}

static const struct kcs_bmc_driver_ops kcs_bmc_serio_driver_ops = {
	.add_device = kcs_bmc_serio_add_device,
	.remove_device = kcs_bmc_serio_remove_device,
};

static struct kcs_bmc_driver kcs_bmc_serio_driver = {
	.ops = &kcs_bmc_serio_driver_ops,
};

module_kcs_bmc_driver(kcs_bmc_serio_driver);
MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Andrew Jeffery <andrew@aj.id.au>");
MODULE_DESCRIPTION("Adapter driver for serio access to BMC KCS devices");
