// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2025, Silicon Laboratories, Inc.
 */

#include <linux/string.h>

#include "cpc.h"
#include "interface.h"

/**
 * cpc_ep_release() - Actual release of the CPC endpoint.
 * @dev: Device embedded in struct cpc_endpoint.
 *
 * This function should not be called directly, users are expected to use cpc_endpoint_put().
 */
static void cpc_ep_release(struct device *dev)
{
	struct cpc_endpoint *ep = cpc_endpoint_from_dev(dev);

	cpc_interface_put(ep->intf);
	kfree(ep);
}

/**
 * cpc_endpoint_alloc() - Allocate memory for new CPC endpoint.
 * @intf: CPC interface owning this endpoint.
 * @id: Endpoint ID.
 *
 * Context: Process context as allocations are done with @GFP_KERNEL flag
 *
 * Return: allocated CPC endpoint or %NULL.
 */
struct cpc_endpoint *cpc_endpoint_alloc(struct cpc_interface *intf, u8 id)
{
	struct cpc_endpoint *ep;

	if (!cpc_interface_get(intf))
		return NULL;

	ep = kzalloc(sizeof(*ep), GFP_KERNEL);
	if (!ep) {
		cpc_interface_put(intf);
		return NULL;
	}

	ep->intf = intf;
	ep->id = id;

	ep->dev.parent = &intf->dev;
	ep->dev.bus = &cpc_bus;
	ep->dev.release = cpc_ep_release;

	device_initialize(&ep->dev);

	return ep;
}

static int cpc_ep_check_unique_id(struct device *dev, void *data)
{
	struct cpc_endpoint *ep = cpc_endpoint_from_dev(dev);
	struct cpc_endpoint *new_ep = data;

	if (ep->id == new_ep->id)
		return -EBUSY;

	return 0;
}

static int __cpc_endpoint_register(struct cpc_endpoint *ep)
{
	size_t name_len;
	int err;

	name_len = strnlen(ep->name, sizeof(ep->name));
	if (name_len == 0 || name_len == sizeof(ep->name))
		return -EINVAL;

	err = dev_set_name(&ep->dev, "%s.%d", dev_name(&ep->intf->dev), ep->id);
	if (err) {
		dev_err(&ep->dev, "failed to dev_set_name (%d)\n", err);
		return err;
	}

	err = device_for_each_child(&ep->intf->dev, ep, cpc_ep_check_unique_id);
	if (err)
		return err;

	err = device_add(&ep->dev);
	if (err)
		return err;

	return 0;
}

/**
 * cpc_endpoint_register() - Register an endpoint.
 * @ep: Endpoint to register.
 *
 * Companion function of cpc_endpoint_alloc(). This function adds the endpoint, making it usable by
 * CPC drivers. As this ensures that endpoint ID is unique within a CPC interface and then adds the
 * endpoint, the lock interface is held to prevent concurrent additions.
 *
 * Context: Lock "add_lock" of endpoint's interface.
 *
 * Return: 0 on success, negative errno otherwise.
 */
int cpc_endpoint_register(struct cpc_endpoint *ep)
{
	int err;

	if (!ep || !ep->intf)
		return -EINVAL;

	mutex_lock(&ep->intf->add_lock);
	err = __cpc_endpoint_register(ep);
	mutex_unlock(&ep->intf->add_lock);

	return err;
}

/**
 * cpc_endpoint_new() - Convenience wrapper to allocate and register an endpoint.
 * @intf: The interface the endpoint will be attached to.
 * @id: ID of the endpoint to add.
 * @ep_name: Name of the endpoint to add.
 *
 * Context: Process context, as allocation are done with GFP_KERNEL and interface's lock is
 * acquired.
 *
 * Return: Newly added endpoint, or %NULL in case of error.
 */
struct cpc_endpoint *cpc_endpoint_new(struct cpc_interface *intf, u8 id, const char *ep_name)
{
	struct cpc_endpoint *ep;
	int err;

	ep = cpc_endpoint_alloc(intf, id);
	if (!ep)
		return NULL;

	if (ep_name)
		strscpy(ep->name, ep_name);

	err = cpc_endpoint_register(ep);
	if (err)
		goto put_ep;

	return ep;

put_ep:
	cpc_endpoint_put(ep);

	return NULL;
}

/** cpc_endpoint_unregister() - Unregister an endpoint.
 * @ep: Endpoint registered with cpc_endpoint_new() or cpc_endpoint_register().
 *
 * Unregister an endpoint, its resource will be freed when the last reference to this
 * endpoint is dropped.
 */
void cpc_endpoint_unregister(struct cpc_endpoint *ep)
{
	device_del(&ep->dev);
	put_device(&ep->dev);
}
