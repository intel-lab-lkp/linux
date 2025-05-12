// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2025, Silicon Laboratories, Inc.
 */

#include <linux/atomic.h>
#include <linux/device.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/skbuff.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/unaligned.h>

#include "cpc.h"
#include "interface.h"
#include "protocol.h"
#include "system.h"

/**
 * enum cpc_system_cmd_id - Describes all possible system command id's.
 * @CPC_SYSTEM_CMD_NOOP: Used for testing purposes.
 * @CPC_SYSTEM_CMD_PROP_NOTIFY: Used for notification purposes.
 * @CPC_SYSTEM_CMD_PROP_VALUE_GET: Used to fetch a property.
 * @CPC_SYSTEM_CMD_PROP_VALUE_SET: Used to set a property.
 * @CPC_SYSTEM_CMD_PROP_VALUE_IS: Used for receiving asynchronous properties.
 * @CPC_SYSTEM_CMD_INVALID: Used to indicate a system command was invalid.
 */
enum cpc_system_cmd_id {
	CPC_SYSTEM_CMD_NOOP = 0x00,
	CPC_SYSTEM_CMD_PROP_NOTIFY = 0x01,
	CPC_SYSTEM_CMD_PROP_VALUE_GET = 0x02,
	CPC_SYSTEM_CMD_PROP_VALUE_SET = 0x03,
	CPC_SYSTEM_CMD_PROP_VALUE_IS = 0x06,
	CPC_SYSTEM_CMD_INVALID = 0xFF,
};

/**
 * enum cpc_system_property_id - Describes all possible system property identifiers.
 * @CPC_SYSTEM_PROP_INVALID: Invalid property.
 * @CPC_SYSTEM_PROP_PROTOCOL_VERSION: Protocol version property.
 * @CPC_SYSTEM_PROP_DRV_CAPABILITIES: Driver capabilities property.
 * @CPC_SYSTEM_PROP_CPC_VERSION: CPC version property.
 * @CPC_SYSTEM_PROP_APP_VERSION: Application version property.
 * @CPC_SYSTEM_PROP_RESET_REASON: Reset reason property.
 * @CPC_SYSTEM_PROP_EP_OPEN_NOTIF: Endpoint open notification property.
 */
enum cpc_system_property_id {
	CPC_SYSTEM_PROP_INVALID = 0x00,
	CPC_SYSTEM_PROP_PROTOCOL_VERSION = 0x10,
	CPC_SYSTEM_PROP_DRV_CAPABILITIES = 0x11,
	CPC_SYSTEM_PROP_CPC_VERSION = 0x12,
	CPC_SYSTEM_PROP_APP_VERSION = 0x13,
	CPC_SYSTEM_PROP_RESET_REASON = 0x14,
	CPC_SYSTEM_PROP_EP_OPEN_NOTIF = 0x800,
};

/**
 * enum cpc_system_status - Describes all possible system statuses.
 * @CPC_SYSTEM_STATUS_RESET_POWER_ON: Reset was due to a power reason.
 * @CPC_SYSTEM_STATUS_RESET_EXTERNAL: Reset was due to an external reason.
 * @CPC_SYSTEM_STATUS_RESET_SOFTWARE: Reset was due to a software reason.
 * @CPC_SYSTEM_STATUS_RESET_FAULT: Reset was due to a fault.
 * @CPC_SYSTEM_STATUS_RESET_CRASH: Reset was due to a crash.
 * @CPC_SYSTEM_STATUS_RESET_ASSERT: Reset was due to an assert.
 * @CPC_SYSTEM_STATUS_RESET_OTHER: Reset was due to some other reason.
 * @CPC_SYSTEM_STATUS_RESET_UNKNOWN: Reset was due to an unknown reason.
 * @CPC_SYSTEM_STATUS_RESET_WATCHDOG: Reset was due to a watchdog trigger.
 */
enum cpc_system_status {
	CPC_SYSTEM_STATUS_RESET_POWER_ON = 112,
	CPC_SYSTEM_STATUS_RESET_EXTERNAL = 113,
	CPC_SYSTEM_STATUS_RESET_SOFTWARE = 114,
	CPC_SYSTEM_STATUS_RESET_FAULT = 115,
	CPC_SYSTEM_STATUS_RESET_CRASH = 116,
	CPC_SYSTEM_STATUS_RESET_ASSERT = 117,
	CPC_SYSTEM_STATUS_RESET_OTHER = 118,
	CPC_SYSTEM_STATUS_RESET_UNKNOWN = 119,
	CPC_SYSTEM_STATUS_RESET_WATCHDOG = 120,
};

/**
 * struct cpc_system_cmd - Wire representation of the system command.
 * @id: Identifier of the command.
 * @seq: Command sequence number.
 * @len: Length of the payload in bytes.
 * @payload: Variable length payload.
 */
struct cpc_system_cmd {
	u8 id;
	u8 seq;
	__le16 len;
	u8 payload[];
} __packed;

/**
 * struct cpc_system_cmd_property - Wire representation of the system
 * command property.
 * @id: Identifier of the property.
 * @payload: Variable length property value.
 */
struct cpc_system_cmd_property {
	__le32 id;
	u8 payload[];
} __packed;

struct cpc_system_cmd_handle {
	u8 seq;
	struct list_head node;
};

struct cpc_system_cmd_ctx {
	struct mutex lock;	/* Synchronize access to command list */
	u8 next_seq;
	struct list_head list;

	struct work_struct init_work;
	struct work_struct rx_work;

	struct sk_buff_head rx_queue;

	struct cpc_endpoint *ep;
};

static bool __cpc_system_cmd_pop_handle(struct cpc_system_cmd_ctx *cmd_ctx,
					struct cpc_system_cmd_handle **cmd_handle)
{
	*cmd_handle = list_first_entry_or_null(&cmd_ctx->list, struct cpc_system_cmd_handle, node);

	if (*cmd_handle)
		list_del(&(*cmd_handle)->node);

	return !!*cmd_handle;
}

static void cpc_system_cmd_init(struct cpc_system_cmd_ctx *cmd_ctx)
{
	mutex_init(&cmd_ctx->lock);
	INIT_LIST_HEAD(&cmd_ctx->list);
	cmd_ctx->next_seq = 0;
}

static void cpc_system_cmd_clear(struct cpc_system_cmd_ctx *cmd_ctx)
{
	struct cpc_system_cmd_handle *cmd_handle;

	mutex_lock(&cmd_ctx->lock);
	while (__cpc_system_cmd_pop_handle(cmd_ctx, &cmd_handle))
		kfree(cmd_handle);

	cmd_ctx->next_seq = 0;
	mutex_unlock(&cmd_ctx->lock);
}

static bool cpc_system_cmd_find_handle(struct cpc_system_cmd_ctx *cmd_ctx,
				       u8 cmd_seq,
				       struct cpc_system_cmd_handle **cmd_handle)
{
	mutex_lock(&cmd_ctx->lock);

	list_for_each_entry((*cmd_handle), &cmd_ctx->list, node) {
		if ((*cmd_handle)->seq == cmd_seq) {
			list_del(&(*cmd_handle)->node);
			mutex_unlock(&cmd_ctx->lock);
			return true;
		}
	}

	mutex_unlock(&cmd_ctx->lock);

	return false;
}

static void cpc_system_on_reset_reason(struct cpc_endpoint *ep, struct sk_buff *skb)
{
	u32 reset_reason;

	if (skb->len != sizeof(reset_reason)) {
		dev_err(&ep->dev, "reset reason has invalid length (%d)\n", skb->len);
		return;
	}

	reset_reason = get_unaligned_le32(skb->data);

	switch (reset_reason) {
	case CPC_SYSTEM_STATUS_RESET_POWER_ON:
	case CPC_SYSTEM_STATUS_RESET_EXTERNAL:
	case CPC_SYSTEM_STATUS_RESET_SOFTWARE:
	case CPC_SYSTEM_STATUS_RESET_FAULT:
	case CPC_SYSTEM_STATUS_RESET_CRASH:
	case CPC_SYSTEM_STATUS_RESET_ASSERT:
	case CPC_SYSTEM_STATUS_RESET_OTHER:
	case CPC_SYSTEM_STATUS_RESET_UNKNOWN:
	case CPC_SYSTEM_STATUS_RESET_WATCHDOG:
		dev_dbg(&ep->dev, "reset reason: %d\n", reset_reason);
		break;
	default:
		dev_dbg(&ep->dev, "undefined reset reason: %d\n", reset_reason);
		break;
	}
}

static void cpc_system_prop_value_is(struct cpc_endpoint *ep, struct sk_buff *skb)
{
	u32 id;

	if (skb->len < sizeof(id)) {
		dev_warn(&ep->dev, "command property with invalid length (%d)\n", skb->len);
		return;
	}

	id = get_unaligned_le32(skb_pull_data(skb, sizeof(id)));

	switch (id) {
	case CPC_SYSTEM_PROP_RESET_REASON:
		cpc_system_on_reset_reason(ep, skb);
		break;
	default:
		dev_dbg(&ep->dev, "unsupported command property identifier (%d)\n", id);
		break;
	}
}

static void cpc_system_on_prop_value_is(struct cpc_endpoint *ep, struct sk_buff *skb, u8 seq)
{
	struct cpc_system_cmd_ctx *cmd_ctx = cpc_endpoint_get_drvdata(ep);
	struct cpc_system_cmd_handle *cmd_handle;

	if (cpc_system_cmd_find_handle(cmd_ctx, seq, &cmd_handle)) {
		cpc_system_prop_value_is(ep, skb);
		kfree(cmd_handle);
	} else {
		dev_warn(&ep->dev, "unknown command sequence (%u)\n", seq);
	}
}

static void cpc_system_on_ep_open_notification(struct cpc_endpoint *ep, struct sk_buff *skb)
{
	struct cpc_endpoint *new_ep;
	u8 ep_id;

	/*
	 * Payload should contain at least the endpoint ID
	 * and the null terminating byte of the string.
	 */
	if (skb->len < sizeof(ep_id) + 1) {
		dev_warn(&ep->dev, "open with invalid length (%d)\n", skb->len);
		return;
	}

	ep_id = *(u8 *)skb_pull_data(skb, sizeof(ep_id));

	if (skb->data[skb->len - 1] != '\0') {
		dev_warn(&ep->dev, "non nul-terminated endpoint name for ep%u\n", ep_id);
		return;
	}

	if (skb->len > sizeof(ep->name)) {
		dev_warn(&ep->dev, "oversized endpoint name (%d) for ep%u\n", skb->len, ep_id);
		return;
	}

	dev_dbg(&ep->dev, "open notification for ep%u: '%s'\n", ep_id, skb->data);

	new_ep = cpc_endpoint_alloc(ep->intf, ep_id);
	if (!new_ep)
		return;

	memcpy(new_ep->name, skb->data, skb->len);

	/* Register the new endpoint in the same interface that received the notification. */
	cpc_endpoint_register(new_ep);
}

static void cpc_system_on_prop_notify(struct cpc_endpoint *ep, struct sk_buff *skb)
{
	u32 id;

	if (skb->len < sizeof(id)) {
		dev_warn(&ep->dev, "command property with invalid length (%d)\n", skb->len);
		return;
	}

	id = get_unaligned_le32(skb_pull_data(skb, sizeof(id)));

	switch (id) {
	case CPC_SYSTEM_PROP_EP_OPEN_NOTIF:
		cpc_system_on_ep_open_notification(ep, skb);
		break;
	default:
		dev_dbg(&ep->dev, "unsupported command property identifier (%d)\n", id);
		break;
	}
}

static void cpc_system_on_data(struct cpc_endpoint *ep, struct sk_buff *skb)
{
	struct cpc_system_cmd *system_cmd;
	u16 payload_len;
	u8 seq;

	if (skb->len < sizeof(*system_cmd)) {
		dev_warn(&ep->dev, "command with invalid length (%d)\n", skb->len);
		kfree_skb(skb);
		return;
	}

	system_cmd = skb_pull_data(skb, sizeof(*system_cmd));
	payload_len = le16_to_cpu(system_cmd->len);
	seq = system_cmd->seq;

	if (skb->len != payload_len) {
		dev_warn(&ep->dev,
			 "command payload length does not match (%d != %d)\n",
			 skb->len, payload_len);
		kfree_skb(skb);
		return;
	}

	switch (system_cmd->id) {
	case CPC_SYSTEM_CMD_PROP_VALUE_IS:
		cpc_system_on_prop_value_is(ep, skb, seq);
		break;
	case CPC_SYSTEM_CMD_PROP_NOTIFY:
		cpc_system_on_prop_notify(ep, skb);
		break;
	default:
		dev_dbg(&ep->dev, "unsupported system command identifier (%d)\n", system_cmd->id);
		break;
	}

	kfree_skb(skb);
}

static void cpc_system_rx_work(struct work_struct *work)
{
	struct cpc_system_cmd_ctx *cmd_ctx = container_of(work, struct cpc_system_cmd_ctx, rx_work);
	struct sk_buff *skb;

	while ((skb = skb_dequeue(&cmd_ctx->rx_queue)))
		cpc_system_on_data(cmd_ctx->ep, skb);
}

static void cpc_system_rx(struct cpc_endpoint *ep, struct sk_buff *skb)
{
	struct cpc_system_cmd_ctx *cmd_ctx = cpc_endpoint_get_drvdata(ep);

	skb_queue_tail(&cmd_ctx->rx_queue, skb);
	schedule_work(&cmd_ctx->rx_work);
}

static void cpc_system_init_work(struct work_struct *work)
{
	struct cpc_system_cmd_ctx *cmd_ctx = container_of(work,
							  struct cpc_system_cmd_ctx,
							  init_work);

	cpc_endpoint_connect(cmd_ctx->ep);
}

static struct cpc_endpoint_ops system_ops = {
	.rx = cpc_system_rx,
};

static int cpc_system_probe(struct cpc_endpoint *ep)
{
	struct cpc_system_cmd_ctx *cmd_ctx;

	cmd_ctx = kzalloc(sizeof(*cmd_ctx), GFP_KERNEL);
	if (!cmd_ctx)
		return -ENOMEM;

	cpc_system_cmd_init(cmd_ctx);

	INIT_WORK(&cmd_ctx->init_work, cpc_system_init_work);
	INIT_WORK(&cmd_ctx->rx_work, cpc_system_rx_work);

	skb_queue_head_init(&cmd_ctx->rx_queue);

	cmd_ctx->ep = ep;

	cpc_endpoint_set_drvdata(ep, cmd_ctx);
	cpc_endpoint_set_ops(ep, &system_ops);

	/* Defer connection of the system endpoint to not block in probe(). */
	schedule_work(&cmd_ctx->init_work);

	return 0;
}

static void cpc_system_remove(struct cpc_endpoint *ep)
{
	struct cpc_system_cmd_ctx *cmd_ctx = cpc_endpoint_get_drvdata(ep);

	cpc_endpoint_disconnect(ep);

	cpc_system_cmd_clear(cmd_ctx);

	flush_work(&cmd_ctx->init_work);
	flush_work(&cmd_ctx->rx_work);

	skb_queue_purge(&cmd_ctx->rx_queue);

	kfree(cmd_ctx);
}

static struct cpc_driver system_driver = {
	.driver = {
		.name = CPC_SYSTEM_ENDPOINT_NAME,
	},
	.probe = cpc_system_probe,
	.remove = cpc_system_remove,
};

/**
 * cpc_system_drv_register - Register the system endpoint driver.
 *
 * @return: 0 on success, otherwise a negative error code.
 */
int cpc_system_drv_register(void)
{
	return cpc_driver_register(&system_driver);
}

/**
 * cpc_system_drv_unregister - Unregister the system endpoint driver.
 */
void cpc_system_drv_unregister(void)
{
	cpc_driver_unregister(&system_driver);
}
