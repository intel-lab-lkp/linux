// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2025, Silicon Laboratories, Inc.
 */

#include <linux/device/driver.h>
#include <linux/module.h>

#include "cpc.h"
#include "header.h"

/**
 * cpc_skb_alloc() - Allocate an skb with a specific headroom for CPC headers.
 * @payload_len: Length of the payload.
 * @priority: GFP priority to use for memory allocation.
 *
 * Return: Pointer to the skb on success, otherwise NULL.
 */
struct sk_buff *cpc_skb_alloc(size_t payload_len, gfp_t priority)
{
	struct sk_buff *skb;

	skb = alloc_skb(payload_len + CPC_HEADER_SIZE, priority);
	if (skb)
		skb_reserve(skb, CPC_HEADER_SIZE);

	return skb;
}

/**
 * cpc_skb_set_ctx() - Set the skb context.
 * @skb: Frame.
 * @destructor: Destructor callback.
 * @ctx: Context pointer, might be NULL.
 */
void cpc_skb_set_ctx(struct sk_buff *skb,
		     void (*destructor)(struct sk_buff *skb),
		     void *ctx)
{
	skb->destructor = destructor;

	if (ctx)
		memcpy(&skb->cb[0], &ctx, sizeof(void *));
}

/**
 * cpc_skb_get_ctx() - Get the skb context.
 * @skb: Frame.
 *
 * Return: Context pointer.
 */
void *cpc_skb_get_ctx(struct sk_buff *skb)
{
	void *ctx;

	memcpy(&ctx, &skb->cb[0], sizeof(void *));

	return ctx;
}

static int cpc_bus_match(struct device *dev, const struct device_driver *driver)
{
	struct cpc_driver *cpc_drv = cpc_driver_from_drv(driver);
	struct cpc_endpoint *cpc_ep = cpc_endpoint_from_dev(dev);

	return strcmp(cpc_drv->driver.name, cpc_ep->name) == 0;
}

static int cpc_bus_probe(struct device *dev)
{
	struct cpc_driver *cpc_drv = cpc_driver_from_drv(dev->driver);
	struct cpc_endpoint *ep = cpc_endpoint_from_dev(dev);

	return cpc_drv->probe(ep);
}

static void cpc_bus_remove(struct device *dev)
{
	struct cpc_driver *cpc_drv = cpc_driver_from_drv(dev->driver);
	struct cpc_endpoint *ep = cpc_endpoint_from_dev(dev);

	cpc_drv->remove(ep);
}

const struct bus_type cpc_bus = {
	.name = KBUILD_MODNAME,
	.match = cpc_bus_match,
	.probe = cpc_bus_probe,
	.remove = cpc_bus_remove,
};

/**
 * __cpc_driver_register() - Register driver to the cpc bus.
 * @cpc_drv: Reference to the cpc driver.
 * @owner: Reference to this module's owner.
 *
 * @return: 0 on success, otherwise a negative error code.
 */
int __cpc_driver_register(struct cpc_driver *cpc_drv, struct module *owner)
{
	cpc_drv->driver.bus = &cpc_bus;
	cpc_drv->driver.owner = owner;

	return driver_register(&cpc_drv->driver);
}

/**
 * cpc_driver_unregister() - Unregister driver from the cpc bus.
 * @cpc_drv: Reference to the cpc driver.
 */
void cpc_driver_unregister(struct cpc_driver *cpc_drv)
{
	driver_unregister(&cpc_drv->driver);
}

static int __init cpc_init(void)
{
	int err;

	err = bus_register(&cpc_bus);

	return err;
}
module_init(cpc_init);

static void __exit cpc_exit(void)
{
	bus_unregister(&cpc_bus);
}
module_exit(cpc_exit);

MODULE_DESCRIPTION("Silicon Labs CPC Protocol");
MODULE_AUTHOR("Damien Riégel <damien.riegel@silabs.com>");
MODULE_LICENSE("GPL");
