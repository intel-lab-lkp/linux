/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2025, Silicon Laboratories, Inc.
 */

#ifndef __CPC_H
#define __CPC_H

#include <linux/device.h>
#include <linux/types.h>

#define CPC_ENDPOINT_NAME_MAX_LEN 128

struct cpc_driver;
struct cpc_interface;
struct cpc_endpoint;

/**
 * struct cpc_endpoint - Representation of CPC endpointl
 * @dev: Driver model representation of the device.
 * @name: Endpoint name, used for matching with corresponding driver.
 * @id: Endpoint id, uniquely identifies an endpoint within a CPC device.
 * @intf: Pointer to CPC device this endpoint belongs to.
 * @list_node: list_head member for linking in a CPC device.
 *
 * Each endpoint can send and receive data without consideration of the other endpoints sharing the
 * same physical link.
 */
struct cpc_endpoint {
	struct device dev;

	char name[CPC_ENDPOINT_NAME_MAX_LEN];
	u8 id;

	struct cpc_interface *intf;
	struct list_head list_node;
};

struct cpc_endpoint *cpc_endpoint_alloc(struct cpc_interface *intf, u8 id);
int cpc_endpoint_register(struct cpc_endpoint *ep);
struct cpc_endpoint *cpc_endpoint_new(struct cpc_interface *intf, u8 id, const char *ep_name);

void cpc_endpoint_unregister(struct cpc_endpoint *ep);

/**
 * cpc_endpoint_from_dev() - Upcast from a device pointer.
 * @dev: Reference to a device.
 *
 * Return: Reference to the cpc endpoint.
 */
static inline struct cpc_endpoint *cpc_endpoint_from_dev(const struct device *dev)
{
	return container_of(dev, struct cpc_endpoint, dev);
}

/**
 * cpc_endpoint_get() - Get a reference to endpoint and return its pointer.
 * @ep: Endpoint to get.
 *
 * Return: Endpoint pointer with its reference counter incremented, or %NULL.
 */
static inline struct cpc_endpoint *cpc_endpoint_get(struct cpc_endpoint *ep)
{
	if (!ep || !get_device(&ep->dev))
		return NULL;
	return ep;
}

/**
 * cpc_endpoint_put() - Release reference to an endpoint.
 * @ep: CPC endpoint, allocated by cpc_endpoint_alloc().
 *
 * Context: Process context.
 */
static inline void cpc_endpoint_put(struct cpc_endpoint *ep)
{
	if (ep)
		put_device(&ep->dev);
}

/**
 * cpc_endpoint_get_drvdata() - Get driver data associated with this endpoint.
 * @ep: Endpoint.
 *
 * Return: Driver data, set by cpc_endpoint_set_drvdata().
 */
static inline void *cpc_endpoint_get_drvdata(struct cpc_endpoint *ep)
{
	return dev_get_drvdata(&ep->dev);
}

/**
 * cpc_endpoint_set_drvdata() - Set driver data for this endpoint.
 * @ep: Endpoint.
 */
static inline void cpc_endpoint_set_drvdata(struct cpc_endpoint *ep, void *data)
{
	dev_set_drvdata(&ep->dev, data);
}

#endif
