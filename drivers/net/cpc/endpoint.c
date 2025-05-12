// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2025, Silicon Laboratories, Inc.
 */

#include <linux/string.h>

#include "cpc.h"
#include "header.h"
#include "interface.h"
#include "protocol.h"

/**
 * cpc_ep_release() - Actual release of the CPC endpoint.
 * @dev: Device embedded in struct cpc_endpoint.
 *
 * This function should not be called directly, users are expected to use cpc_endpoint_put().
 */
static void cpc_ep_release(struct device *dev)
{
	struct cpc_endpoint *ep = cpc_endpoint_from_dev(dev);

	skb_queue_purge(&ep->pending_ack_queue);
	skb_queue_purge(&ep->holding_queue);

	cpc_interface_put(ep->intf);
	kfree(ep);
}

/**
 * cpc_endpoint_tcb_reset() - Reset endpoint's TCB to initial values.
 * @ep: endpoint pointer
 */
static void cpc_endpoint_tcb_reset(struct cpc_endpoint *ep)
{
	ep->tcb.seq = ep->id;
	ep->tcb.ack = 0;
	ep->tcb.mtu = 0;
	ep->tcb.send_nxt = ep->id;
	ep->tcb.send_una = ep->id;
	ep->tcb.send_wnd = 1;
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

	mutex_init(&ep->tcb.lock);
	cpc_endpoint_tcb_reset(ep);

	init_completion(&ep->conn);
	skb_queue_head_init(&ep->pending_ack_queue);
	skb_queue_head_init(&ep->holding_queue);

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

/**
 * cpc_endpoint_set_ops() - Set callbacks for this endpoint.
 * @ep: Endpoint
 * @ops: New callbacks to set. If already set, override pre-existing value.
 */
void cpc_endpoint_set_ops(struct cpc_endpoint *ep, struct cpc_endpoint_ops *ops)
{
	if (test_bit(CPC_ENDPOINT_UP, &ep->flags))
		return;

	if (ep)
		ep->ops = ops;
}

/**
 * cpc_endpoint_connect - Connect to the remote endpoint.
 * @ep: Endpoint handle.
 *
 * @return: 0 on success, otherwise a negative error code.
 */
int cpc_endpoint_connect(struct cpc_endpoint *ep)
{
	unsigned long timeout = msecs_to_jiffies(2000);
	int err;

	if (!ep->ops || !ep->ops->rx)
		return -EINVAL;

	if (test_bit(CPC_ENDPOINT_UP, &ep->flags))
		return 0;

	cpc_interface_add_rx_endpoint(ep);

	mutex_lock(&ep->tcb.lock);
	skb_queue_purge(&ep->pending_ack_queue);
	skb_queue_purge(&ep->holding_queue);
	cpc_endpoint_tcb_reset(ep);
	mutex_unlock(&ep->tcb.lock);

	err = cpc_protocol_send_syn(ep);
	if (err)
		goto remove_from_ep_list;

	timeout = wait_for_completion_timeout(&ep->conn, timeout);
	if (timeout == 0) {
		err = -ETIMEDOUT;
		mutex_lock(&ep->tcb.lock);
		skb_queue_purge(&ep->pending_ack_queue);
		mutex_unlock(&ep->tcb.lock);

		goto remove_from_ep_list;
	}

	return 0;

remove_from_ep_list:
	cpc_interface_remove_rx_endpoint(ep);

	return err;
}

void __cpc_endpoint_disconnect(struct cpc_endpoint *ep, bool send_rst)
{
	if (!test_and_clear_bit(CPC_ENDPOINT_UP, &ep->flags))
		return;

	cpc_interface_remove_rx_endpoint(ep);

	if (send_rst) {
		/*
		 * It makes sense to wait on the RECEIVING bit only when send_rst is true as this
		 * means the operation was initiated by the user and can happen concurrently with
		 * the RX work function. If a RST is received from the remote and
		 * __cpc_endpoint_disconnect from the RX work function, then it's safe to assume
		 * that this frame won't trigger a call to ep->ops->rx function.
		 */
		int err;

		err = wait_on_bit_timeout(&ep->flags,
					  CPC_ENDPOINT_RECEIVING,
					  TASK_INTERRUPTIBLE,
					  msecs_to_jiffies(1000));
		if (!err)
			dev_warn(&ep->dev, "Timeout when disconnecting.\n");

		cpc_protocol_send_rst(ep->intf, ep->id);
	}
}

/**
 * cpc_endpoint_disconnect - Disconnect endpoint from remote.
 * @ep: Endpoint handle.
 *
 * Close the connection with the remote device. When that function returns, no more packets will be
 * received from the remote.
 *
 * Context: Must be called from process context, endpoint's interface lock is held.
 */
void cpc_endpoint_disconnect(struct cpc_endpoint *ep)
{
	__cpc_endpoint_disconnect(ep, true);
}

/**
 * cpc_endpoint_write - Write a DATA frame.
 * @ep: Endpoint handle.
 * @skb: Frame to send.
 *
 * @return: 0 on success, otherwise a negative error code.
 */
int cpc_endpoint_write(struct cpc_endpoint *ep, struct sk_buff *skb)
{
	struct cpc_header hdr;
	int err;

	mutex_lock(&ep->tcb.lock);

	if (skb->len > ep->tcb.mtu) {
		err = -EINVAL;
		goto out;
	}

	if (ep->intf->ops->csum)
		ep->intf->ops->csum(skb);

	memset(&hdr, 0, sizeof(hdr));
	hdr.ctrl = cpc_header_get_ctrl(CPC_FRAME_TYPE_DATA, true);
	hdr.ep_id = ep->id;
	hdr.recv_wnd = CPC_HEADER_MAX_RX_WINDOW;
	hdr.seq = ep->tcb.seq;
	hdr.dat.payload_len = skb->len;

	err = __cpc_protocol_write(ep, &hdr, skb);

out:
	mutex_unlock(&ep->tcb.lock);

	return err;
}
