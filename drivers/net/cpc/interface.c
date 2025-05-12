// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2025, Silicon Laboratories, Inc.
 */

#include <linux/module.h>

#include "cpc.h"
#include "header.h"
#include "interface.h"
#include "protocol.h"

#define to_cpc_interface(d) container_of(d, struct cpc_interface, dev)

static DEFINE_IDA(cpc_ida);

static void cpc_interface_rx_work(struct work_struct *work)
{
	struct cpc_interface *intf = container_of(work, struct cpc_interface, rx_work);
	enum cpc_frame_type type;
	struct cpc_endpoint *ep;
	struct sk_buff *skb;
	u8 ep_id;

	while ((skb = skb_dequeue(&intf->rx_queue))) {
		cpc_header_get_type(skb->data, &type);
		ep_id = cpc_header_get_ep_id(skb->data);

		ep = cpc_interface_get_endpoint(intf, ep_id);
		if (!ep) {
			if (type != CPC_FRAME_TYPE_RST) {
				dev_dbg(&intf->dev, "ep%u not allocated (%d)\n", ep_id, type);
				cpc_protocol_send_rst(intf, ep_id);
			}
			kfree_skb(skb);
			continue;
		}

		switch (type) {
		case CPC_FRAME_TYPE_DATA:
			cpc_protocol_on_data(ep, skb);
			break;
		case CPC_FRAME_TYPE_SYN:
			cpc_protocol_on_syn(ep, skb);
			break;
		case CPC_FRAME_TYPE_RST:
			dev_dbg(&ep->dev, "reset\n");
			kfree_skb(skb);
			cpc_protocol_on_rst(ep);
			break;
		}

		cpc_endpoint_put(ep);
	}
}

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

	flush_work(&intf->rx_work);

	destroy_workqueue(intf->workq);

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

	intf->workq = alloc_workqueue(KBUILD_MODNAME "_wq", WQ_HIGHPRI, 0);
	if (!intf->workq) {
		ida_free(&cpc_ida, intf->index);
		kfree(intf);

		return ERR_PTR(-ENOMEM);
	}

	mutex_init(&intf->add_lock);
	mutex_init(&intf->lock);
	INIT_LIST_HEAD(&intf->eps);

	INIT_WORK(&intf->rx_work, cpc_interface_rx_work);
	skb_queue_head_init(&intf->rx_queue);
	skb_queue_head_init(&intf->tx_queue);

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

/**
 * cpc_interface_add_rx_endpoint() - Set an endpoint as being available for receiving frames.
 * @ep: Endpoint.
 */
void cpc_interface_add_rx_endpoint(struct cpc_endpoint *ep)
{
	struct cpc_interface *intf = ep->intf;

	mutex_lock(&intf->lock);
	list_add_tail(&ep->list_node, &intf->eps);
	mutex_unlock(&intf->lock);
}

/**
 * cpc_interface_remove_rx_endpoint() - Unet an endpoint as being available for receiving frames.
 * @ep: Endpoint.
 */
void cpc_interface_remove_rx_endpoint(struct cpc_endpoint *ep)
{
	struct cpc_interface *intf = ep->intf;

	mutex_lock(&intf->lock);
	list_del(&ep->list_node);
	mutex_unlock(&intf->lock);
}

/**
 * cpc_interface_receive_frame - queue a received frame for processing
 * @intf: pointer to the CPC device
 * @skb: received frame
 *
 * Context: This queues the sk_buff in a list and schedule the work task to process the list.
 */
void cpc_interface_receive_frame(struct cpc_interface *intf, struct sk_buff *skb)
{
	skb_queue_tail(&intf->rx_queue, skb);
	queue_work(intf->workq, &intf->rx_work);
}

/**
 * cpc_interface_send_frame() - Queue a socket buffer for transmission.
 * @intf: Interface to send SKB over.
 * @ops: SKB to send.
 *
 * Queue SKB in interface's transmit queue and signal the interface. Interface is expected to use
 * cpc_interface_dequeue() to get the next SKB to transmit.
 */
void cpc_interface_send_frame(struct cpc_interface *intf, struct sk_buff *skb)
{
	skb_queue_tail(&intf->tx_queue, skb);
	intf->ops->wake_tx(intf);
}

/**
 * cpc_interface_dequeue() - Get the next SKB that was queued for transmission.
 * @intf: Interface.
 *
 * Get an SKB that was previously queued by cpc_interface_send_frame().
 *
 * Return: An SKB, or %NULL if queue was empty.
 */
struct sk_buff *cpc_interface_dequeue(struct cpc_interface *intf)
{
	return skb_dequeue(&intf->tx_queue);
}

/**
 * cpc_interface_tx_queue_empty() - Check if transmit queue is empty.
 * @intf: Interface.
 *
 * Return: True if transmit queue is empty, false otherwise.
 */
bool cpc_interface_tx_queue_empty(struct cpc_interface *intf)
{
	return skb_queue_empty_lockless(&intf->tx_queue);
}
