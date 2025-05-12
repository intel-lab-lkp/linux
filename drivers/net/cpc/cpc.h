/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2025, Silicon Laboratories, Inc.
 */

#ifndef __CPC_H
#define __CPC_H

#include <linux/device.h>
#include <linux/skbuff.h>
#include <linux/types.h>

#define CPC_ENDPOINT_NAME_MAX_LEN 128

struct cpc_driver;
struct cpc_interface;
struct cpc_endpoint;

extern const struct bus_type cpc_bus;

/**
 * struct cpc_endpoint_tcb - endpoint's transmission control block
 * @lock: synchronize tcb access
 * @send_wnd: send window, maximum number of frames that the remote can accept
 *            TX frames should have a sequence in the range
 *            [send_una; send_una + send_wnd].
 * @send_nxt: send next, the next sequence number that will be used for transmission
 * @send_una: send unacknowledged, the oldest unacknowledged sequence number
 * @ack: current acknowledge number
 * @seq: current sequence number
 * @mtu: maximum transmission unit
 */
struct cpc_endpoint_tcb {
	struct mutex lock; /* Synchronize access to all other attributes. */
	u8 send_wnd;
	u8 send_nxt;
	u8 send_una;
	u8 ack;
	u8 seq;
};

/** struct cpc_endpoint_ops - Endpoint's callbacks.
 * @rx: Data availability is provided with a skb owned by the driver.
 */
struct cpc_endpoint_ops {
	void (*rx)(struct cpc_endpoint *ep, struct sk_buff *skb);
};

/**
 * struct cpc_endpoint - Representation of CPC endpointl
 * @dev: Driver model representation of the device.
 * @name: Endpoint name, used for matching with corresponding driver.
 * @id: Endpoint id, uniquely identifies an endpoint within a CPC device.
 * @intf: Pointer to CPC device this endpoint belongs to.
 * @list_node: list_head member for linking in a CPC device.
 * @tcb: Transmission control block.
 * @pending_ack_queue: Contain frames pending on an acknowledge.
 * @holding_queue: Contains frames that were not pushed to the transport layer
 *                 due to having insufficient space in the transmit window.
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
	struct cpc_endpoint_ops *ops;

	struct cpc_endpoint_tcb tcb;

	struct sk_buff_head pending_ack_queue;
	struct sk_buff_head holding_queue;
};

struct cpc_endpoint *cpc_endpoint_alloc(struct cpc_interface *intf, u8 id);
int cpc_endpoint_register(struct cpc_endpoint *ep);
struct cpc_endpoint *cpc_endpoint_new(struct cpc_interface *intf, u8 id, const char *ep_name);

void cpc_endpoint_unregister(struct cpc_endpoint *ep);

int cpc_endpoint_write(struct cpc_endpoint *ep, struct sk_buff *skb);
void cpc_endpoint_set_ops(struct cpc_endpoint *ep, struct cpc_endpoint_ops *ops);

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

/*---------------------------------------------------------------------------*/

/**
 * struct cpc_driver - CPC endpoint driver.
 * @driver: Internal driver for the device driver model.
 * @probe: Binds this driver to the endpoint.
 * @remove: Unbinds this driver from the endpoint.
 *
 * This represents a device driver that uses an endpoint to communicate with a remote application at
 * the other side of the CPC interface. The way to communicate with the remote is abstracted by the
 * interface, and drivers don't have to care if other endpoints are present or not.
 */
struct cpc_driver {
	struct device_driver driver;

	int (*probe)(struct cpc_endpoint *ep);
	void (*remove)(struct cpc_endpoint *ep);
};

int __cpc_driver_register(struct cpc_driver *cpc_drv, struct module *owner);
void cpc_driver_unregister(struct cpc_driver *cpc_drv);

/* Convenience macro with THIS_MODULE */
#define cpc_driver_register(driver) \
	__cpc_driver_register(driver, THIS_MODULE)

/**
 * cpc_driver_from_drv - Upcast from a device driver.
 * @drv: Reference to a device driver.
 *
 * @return: Reference to the cpc driver.
 */
static inline struct cpc_driver *cpc_driver_from_drv(const struct device_driver *drv)
{
	return container_of(drv, struct cpc_driver, driver);
}

/*---------------------------------------------------------------------------*/

struct sk_buff *cpc_skb_alloc(size_t payload_len, gfp_t priority);
void cpc_skb_set_ctx(struct sk_buff *skb,
		     void (*destructor)(struct sk_buff *skb),
		     void *ctx);
void *cpc_skb_get_ctx(struct sk_buff *skb);

#endif
