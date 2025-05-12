// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2025, Silicon Laboratories, Inc.
 */

#include <linux/module.h>

#include "cpc.h"
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

	mutex_init(&intf->add_lock);
	mutex_init(&intf->lock);
	INIT_LIST_HEAD(&intf->eps);

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

static int cpc_intf_unregister_ep(struct device *dev, void *null)
{
	cpc_endpoint_unregister(cpc_endpoint_from_dev(dev));
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
	/* Iterate in reverse order so that system endpoint is removed last. */
	device_for_each_child_reverse(&intf->dev, NULL, cpc_intf_unregister_ep);

	device_del(&intf->dev);
	cpc_interface_put(intf);
}

/**
 * __cpc_interface_get_endpoint() - get endpoint registered in CPC device with this id without lock
 * @intf: CPC device to probe
 * @ep_id: endpoint ID that's being looked for
 *
 * Get an endpoint by its ID if present in a CPC device. Endpoint's ref count is incremented and
 * should be decremented with cpc_endpoint_put() when done.
 *
 * Context: This function doesn't lock device's endpoint list, caller is responsible for that.
 *
 * Return: a struct cpc_endpoint pointer or NULL if not found.
 */
static struct cpc_endpoint *__cpc_interface_get_endpoint(struct cpc_interface *intf, u8 ep_id)
{
	struct cpc_endpoint *ep_it;

	list_for_each_entry(ep_it, &intf->eps, list_node) {
		if (ep_it->id == ep_id)
			return cpc_endpoint_get(ep_it);
	}

	return NULL;
}

/**
 * cpc_interface_get_endpoint() - get endpoint registered in CPC device with this id
 * @intf: CPC device to probe
 * @ep_id: endpoint ID that's being looked for
 *
 * Context: This function locks device's endpoint list.
 *
 * Return: a struct cpc_endpoint pointer or NULL if not found.
 */
struct cpc_endpoint *cpc_interface_get_endpoint(struct cpc_interface *intf, u8 ep_id)
{
	struct cpc_endpoint *ep;

	mutex_lock(&intf->lock);
	ep = __cpc_interface_get_endpoint(intf, ep_id);
	mutex_unlock(&intf->lock);

	return ep;
}
