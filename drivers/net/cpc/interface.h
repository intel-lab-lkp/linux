/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2025, Silicon Laboratories, Inc.
 */

#ifndef __CPC_INTERFACE_H
#define __CPC_INTERFACE_H

#include <linux/device.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/skbuff.h>

struct cpc_interface;
struct cpc_interface_ops;

/**
 * struct cpc_interface - Representation of a CPC interface.
 * @dev: Device structure for bookkeeping..
 * @add_lock: Lock to serialize addition of new endpoints.
 * @ops: Callbacks for this device.
 * @index: Device index.
 * @lock: Protect access to endpoint list.
 * @eps: List of endpoints managed by this device.
 */
struct cpc_interface {
	struct device dev;

	/* Prevent concurrent addition of new devices */
	struct mutex add_lock;

	const struct cpc_interface_ops *ops;

	int index;

	struct mutex lock;	/* Protect eps from concurrent access. */
	struct list_head eps;
};

/**
 * struct cpc_interface_ops - Callbacks from CPC core to physical bus driver.
 * @wake_tx: Called by CPC core to wake up the transmit task of that interface.
 * @csum: Callback to calculate checksum over the payload.
 *
 * This structure contains various callbacks that the bus (SDIO, SPI) driver must implement.
 */
struct cpc_interface_ops {
	int (*wake_tx)(struct cpc_interface *intf);
	void (*csum)(struct sk_buff *skb);
};

struct cpc_interface *cpc_interface_alloc(struct device *parent,
					  const struct cpc_interface_ops *ops,
					  void *priv);

int cpc_interface_register(struct cpc_interface *intf);
void cpc_interface_unregister(struct cpc_interface *intf);

struct cpc_endpoint *cpc_interface_get_endpoint(struct cpc_interface *intf, u8 ep_id);

/**
 * cpc_interface_get() - Get a reference to interface and return its pointer.
 * @intf: Interface to get.
 *
 * Return: Interface pointer with its reference counter incremented, or %NULL.
 */
static inline struct cpc_interface *cpc_interface_get(struct cpc_interface *intf)
{
	if (!intf || !get_device(&intf->dev))
		return NULL;
	return intf;
}

/**
 * cpc_interface_put() - Release reference to an interface.
 * @intf: CPC interface
 *
 * Context: Process context.
 */
static inline void cpc_interface_put(struct cpc_interface *intf)
{
	if (intf)
		put_device(&intf->dev);
}

/**
 * cpc_interface_get_priv() - Get driver data associated with this interface.
 * @intf: Interface pointer.
 *
 * Return: Driver data, set at allocation via cpc_interface_alloc().
 */
static inline void *cpc_interface_get_priv(struct cpc_interface *intf)
{
	if (!intf)
		return NULL;
	return dev_get_drvdata(&intf->dev);
}

#endif
