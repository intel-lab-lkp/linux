// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2025, Silicon Laboratories, Inc.
 */

#include <linux/module.h>

#include "interface.h"

#define to_cpc_interface(d) container_of(d, struct cpc_interface, dev)

static DEFINE_IDA(cpc_ida);

/**
 * cpc_intf_release() - Actual release of interface.
 * @dev: Device embedded in struct cpc_interface
 *
 * This function should not be called directly, users are expected to use cpc_interface_put()
 * instead. This function will be called when the last reference to the CPC device is released.
 */
static void cpc_intf_release(struct device *dev)
{
	struct cpc_interface *intf = to_cpc_interface(dev);

	ida_free(&cpc_ida, intf->index);
	kfree(intf);
}

/**
 * cpc_interface_alloc() - Allocate memory for new CPC interface.
 *
 * @parent: Parent device.
 * @ops: Callbacks for this device.
 * @priv: Pointer to private structure associated with this device.
 *
 * Context: Process context as allocations are done with @GFP_KERNEL flag
 *
 * Return: allocated CPC interface or %NULL.
 */
struct cpc_interface *cpc_interface_alloc(struct device *parent,
					  const struct cpc_interface_ops *ops,
					  void *priv)
{
	struct cpc_interface *intf;

	intf = kzalloc(sizeof(*intf), GFP_KERNEL);
	if (!intf)
		return NULL;

	intf->index = ida_alloc(&cpc_ida, GFP_KERNEL);
	if (intf->index < 0) {
		kfree(intf);
		return NULL;
	}

	intf->ops = ops;

	intf->dev.parent = parent;
	intf->dev.release = cpc_intf_release;

	device_initialize(&intf->dev);

	dev_set_name(&intf->dev, "cpc%d", intf->index);
	dev_set_drvdata(&intf->dev, priv);

	return intf;
}

/**
 * cpc_interface_register() - Register CPC interface.
 * @intf: CPC device to register.
 *
 * Context: Process context.
 *
 * Return: 0 if successful, otherwise a negative error code.
 */
int cpc_interface_register(struct cpc_interface *intf)
{
	int err;

	err = device_add(&intf->dev);
	if (err)
		return err;

	return 0;
}

/**
 * cpc_interface_unregister() - Unregister a CPC interface.
 * @intf: CPC device to unregister.
 *
 * Context: Process context.
 */
void cpc_interface_unregister(struct cpc_interface *intf)
{
	device_del(&intf->dev);
	cpc_interface_put(intf);
}
