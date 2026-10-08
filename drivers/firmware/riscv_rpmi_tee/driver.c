// SPDX-License-Identifier: GPL-2.0-only
/*
 * RISC-V RPMI TEE transport
 *
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#include <linux/mailbox_client.h>
#include <linux/mailbox/riscv-rpmi-message.h>
#include <linux/cleanup.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/rpmi_tee.h>
#include <linux/slab.h>
#include <linux/unaligned.h>

#include "rpmi_tee_private.h"

/**
 * struct rpmi_tee_call_req - TEE_CALL request prefix
 * @sender_id: Calling REE endpoint identifier.
 * @target_id: Destination TEE endpoint identifier.
 * @service: UUID of the target service.
 * @service_data_len: Length of @service_data in bytes.
 * @service_data: Service-defined request data.
 */
struct rpmi_tee_call_req {
	__le32 sender_id;
	__le32 target_id;
	u8 service[UUID_SIZE];
	__le32 service_data_len;
	u8 service_data[];
} __packed;

/**
 * struct rpmi_tee_call_resp - TEE_CALL response prefix
 * @status: RPMI completion status.
 * @service_data_len: Length of @service_data in bytes.
 * @service_data: Service-defined response data.
 */
struct rpmi_tee_call_resp {
	__le32 status;
	__le32 service_data_len;
	u8 service_data[];
} __packed;

struct rpmi_tee_child {
	struct list_head node;
	struct rpmi_tee_device *rdev;
};

/* rpmi_tee_send_with_status() - Send an RPMI TEE service request. */
int rpmi_tee_send_with_status(struct rpmi_tee_transport *priv, u32 service_id,
			      const void *req, size_t req_len, void *resp,
			      size_t *resp_len, s32 *status)
{
	size_t max_resp_len = *resp_len;
	struct rpmi_mbox_message msg;
	int ret;

	if (req_len > priv->mbox.max_msg_data_size ||
	    max_resp_len > priv->mbox.max_msg_data_size)
		return -EMSGSIZE;

	rpmi_mbox_init_send_with_response(&msg, service_id, (void *)req,
					  req_len, resp, max_resp_len);
	ret = rpmi_mbox_send_message_sync(priv->mbox.chan, &msg);
	if (ret)
		return ret;
	/* At least STATUS word should be present. */
	if (msg.data.out_response_len < sizeof(__le32))
		return -EPROTO;

	*resp_len = msg.data.out_response_len;
	*status = (s32)get_unaligned_le32(resp);

	return 0;
}

/**
 * rpmi_tee_send() - Send an RPMI TEE service request
 * @priv: RPMI TEE transport
 * @service_id: RPMI TEE service identifier
 * @req: Request data
 * @req_len: Request data length
 * @resp: Response data buffer, or %NULL for a status-only response
 * @resp_len: On entry, response buffer capacity; on success, response length
 *
 * Pass both @resp and @resp_len as %NULL when the service has no response
 * payload beyond the mandatory RPMI status word.
 *
 * Return: 0 on success, or a negative error code.
 */
static int rpmi_tee_send(struct rpmi_tee_transport *priv, u32 service_id,
			 const void *req, size_t req_len, void *resp,
			 size_t *resp_len)
{
	__le32 status_resp;
	size_t status_resp_len = sizeof(status_resp);
	s32 status;
	int ret;

	if (!resp && !resp_len) {
		resp = &status_resp;
		resp_len = &status_resp_len;
	} else if (!resp || !resp_len) {
		return -EINVAL;
	}

	ret = rpmi_tee_send_with_status(priv, service_id, req, req_len, resp,
					resp_len, &status);
	if (ret)
		return ret;

	if (status == RPMI_ERR_NO_DATA)
		return -ENODATA;

	return rpmi_to_linux_error(status);
}

/**
 * rpmi_tee_get_attr() - Get an RPMI mailbox attribute
 * @priv: RPMI TEE transport
 * @id: Attribute identifier
 * @value: Returned attribute value
 *
 * Return: 0 on success, or a negative error code on failure.
 */
static int rpmi_tee_get_attr(struct rpmi_tee_transport *priv,
			     enum rpmi_mbox_attribute_id id, u32 *value)
{
	struct rpmi_mbox_message msg;
	int ret;

	rpmi_mbox_init_get_attribute(&msg, id);
	ret = rpmi_mbox_send_message_sync(priv->mbox.chan, &msg);
	if (ret)
		return ret;

	*value = msg.attr.value;

	return 0;
}

/* Validate the RPMI mailbox transport and cache its message size. */
static int rpmi_tee_check_transport(struct rpmi_tee_transport *priv)
{
	u32 value;
	int ret;

	ret = rpmi_tee_get_attr(priv, RPMI_MBOX_ATTR_SPEC_VERSION, &value);
	if (ret)
		return ret;
	if (value < RPMI_MKVER(1, 0))
		return -EPROTONOSUPPORT;

	ret = rpmi_tee_get_attr(priv, RPMI_MBOX_ATTR_SERVICEGROUP_ID, &value);
	if (ret)
		return ret;
	if (value != RPMI_SRVGRP_TEE)
		return -ENODEV;

	ret = rpmi_tee_get_attr(priv, RPMI_MBOX_ATTR_SERVICEGROUP_VERSION,
				&value);
	if (ret)
		return ret;
	if (value < RPMI_MKVER(1, 0))
		return -EPROTONOSUPPORT;

	ret = rpmi_tee_get_attr(priv, RPMI_MBOX_ATTR_MAX_MSG_DATA_SIZE, &value);
	if (ret)
		return ret;
	/* The mandatory TEE_CALL request and response must fit the mailbox. */
	if (value < sizeof(struct rpmi_tee_call_req) ||
	    value < sizeof(struct rpmi_tee_call_resp))
		return -EMSGSIZE;

	priv->mbox.max_msg_data_size = value;
	priv->max_call_req_size = value - sizeof(struct rpmi_tee_call_req);
	priv->max_call_resp_size = value - sizeof(struct rpmi_tee_call_resp);

	return 0;
}

/* RPMI TEE SERVICE GRP API. */

/* Return the transport that owns @rdev. */
static struct rpmi_tee_transport *
rpmi_tee_device_to_transport(struct rpmi_tee_device *rdev)
{
	return dev_get_drvdata(rdev->dev.parent);
}

static int rpmi_tee_op_msg_limits_get(struct rpmi_tee_device *rdev,
				      struct rpmi_tee_msg_limits *limits)
{
	struct rpmi_tee_transport *priv = rpmi_tee_device_to_transport(rdev);

	if (!limits)
		return -EINVAL;

	limits->max_req_size = priv->max_call_req_size;
	limits->max_resp_size = priv->max_call_resp_size;

	return 0;
}

/**
 * rpmi_tee_op_call - Invoke a service offered by a TEE endpoint
 * @rdev: TEE service device.
 * @req: Service-defined request data.
 * @req_len: Length of @req in bytes.
 * @resp: Buffer for service-defined response data.
 * @resp_len: On entry, capacity of @resp; on success, response length.
 *
 * MPXY can return -ENOSPC after the TEE has processed the request when the
 * response exceeds the supplied buffer. Callers must not blindly retry a
 * non-idempotent request in that case.
 *
 * Return: 0 on success, or a negative error code.
 */
static int rpmi_tee_op_call(struct rpmi_tee_device *rdev, const void *req,
			    size_t req_len, void *resp, size_t *resp_len)
{
	struct rpmi_tee_transport *priv = rpmi_tee_device_to_transport(rdev);
	size_t call_req_len, call_resp_len;
	u32 service_data_len;
	int ret;

	if (!resp_len || (!req && req_len) || (!resp && *resp_len))
		return -EINVAL;

	/* TEE_CALL payload must fit the mailbox. */
	if (req_len > priv->max_call_req_size ||
	    *resp_len > priv->max_call_resp_size)
		return -EMSGSIZE;

	call_req_len = sizeof(struct rpmi_tee_call_req) + req_len;
	call_resp_len = sizeof(struct rpmi_tee_call_resp) + *resp_len;

	struct rpmi_tee_call_req *call_req __free(kfree) =
		kzalloc(call_req_len, GFP_KERNEL);
	if (!call_req)
		return -ENOMEM;

	struct rpmi_tee_call_resp *call_resp __free(kfree) =
		kzalloc(call_resp_len, GFP_KERNEL);
	if (!call_resp)
		return -ENOMEM;

	call_req->sender_id = cpu_to_le32(priv->self_id);
	call_req->target_id = cpu_to_le32(rdev->endpoint_id);
	export_uuid(call_req->service, &rdev->uuid);
	call_req->service_data_len = cpu_to_le32(req_len);
	if (req_len)
		memcpy(call_req->service_data, req, req_len);
	/* Make TEE CALL. */
	ret = rpmi_tee_send(priv, RPMI_TEE_SRV_CALL, call_req, call_req_len,
			    call_resp, &call_resp_len);
	if (ret)
		return ret;

	if (call_resp_len < sizeof(*call_resp))
		return -EPROTO;
	service_data_len = get_unaligned_le32(&call_resp->service_data_len);
	/* Verify the firmware-reported length fits the actual response. */
	if (service_data_len != call_resp_len - sizeof(*call_resp))
		return -EPROTO;

	if (service_data_len)
		memcpy(resp, call_resp->service_data, service_data_len);
	*resp_len = service_data_len;

	return 0;
}

static const struct rpmi_tee_info_ops rpmi_tee_info_ops = {
	.msg_limits_get = rpmi_tee_op_msg_limits_get,
};

static const struct rpmi_tee_msg_ops rpmi_tee_msg_ops = {
	.call = rpmi_tee_op_call,
};

static const struct rpmi_tee_ops rpmi_tee_ops = {
	.info_ops = &rpmi_tee_info_ops,
	.msg_ops = &rpmi_tee_msg_ops,
};

static void rpmi_tee_unregister_devices(struct rpmi_tee_transport *priv)
{
	struct rpmi_tee_child *child, *tmp;

	list_for_each_entry_safe(child, tmp, &priv->devices, node) {
		list_del(&child->node);
		rpmi_tee_device_unregister(child->rdev);
		kfree(child);
	}
}

static struct rpmi_tee_device *
rpmi_tee_find_device(struct rpmi_tee_transport *priv, const uuid_t *uuid,
		     u32 endpoint_id)
{
	struct rpmi_tee_child *child;

	list_for_each_entry(child, &priv->devices, node) {
		if (child->rdev->endpoint_id == endpoint_id &&
		    uuid_equal(&child->rdev->uuid, uuid))
			return child->rdev;
	}

	return NULL;
}

static int
rpmi_tee_register_devices(struct rpmi_tee_transport *priv,
			  const struct rpmi_tee_discovered_endpoint *ep)
{
	u32 i;

	for (i = 0; i < ep->service_count; i++) {
		const uuid_t *uuid = &ep->services[i];

		/* Discard duplicate devices in the same endpoint. */
		if (rpmi_tee_find_device(priv, uuid, ep->ep_id))
			continue;

		struct rpmi_tee_child *child __free(kfree) =
			kzalloc_obj(*child, GFP_KERNEL);
		if (!child)
			return -ENOMEM;

		child->rdev =
			rpmi_tee_device_register(uuid, ep->ep_id,
						 &rpmi_tee_ops, priv->dev);
		if (IS_ERR(child->rdev))
			return PTR_ERR(child->rdev);

		list_add_tail(&no_free_ptr(child)->node, &priv->devices);
	}

	return 0;
}

static int rpmi_tee_setup_endpoints(struct rpmi_tee_transport *priv)
{
	struct rpmi_tee_discovered_endpoint *ep;
	struct rpmi_tee_discovery system;
	int ret;

	ret = rpmi_tee_discover_endpoints(priv, &system);
	if (ret)
		return ret;

	list_for_each_entry(ep, &system.eps, node) {
		ret = rpmi_tee_register_devices(priv, ep);
		if (ret) {
			rpmi_tee_unregister_devices(priv);
			break;
		}
	}

	rpmi_tee_free_discovery(&system);

	return ret;
}

static int rpmi_tee_transport_probe(struct platform_device *pdev)
{
	struct rpmi_tee_transport *priv;
	int ret;

	priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->dev = &pdev->dev;
	platform_set_drvdata(pdev, priv);
	INIT_LIST_HEAD(&priv->devices);
	priv->mbox.client.dev = &pdev->dev;
	priv->mbox.client.tx_sync = true;
	priv->mbox.chan = mbox_request_channel(&priv->mbox.client, 0);
	if (IS_ERR(priv->mbox.chan))
		return dev_err_probe(&pdev->dev, PTR_ERR(priv->mbox.chan),
				     "failed to request mailbox channel\n");

	ret = rpmi_tee_check_transport(priv);
	if (ret) {
		dev_err_probe(&pdev->dev, ret,
			      "invalid RPMI TEE mailbox channel\n");
		goto out_failed;
	}

	ret = rpmi_tee_setup_endpoints(priv);
	if (ret) {
		dev_err_probe(&pdev->dev, ret,
			      "failed to discover RPMI TEE services\n");
		goto out_failed;
	}

	return 0;

out_failed:
	mbox_free_channel(priv->mbox.chan);

	return ret;
}

static void rpmi_tee_transport_remove(struct platform_device *pdev)
{
	struct rpmi_tee_transport *priv = platform_get_drvdata(pdev);

	rpmi_tee_unregister_devices(priv);
	mbox_free_channel(priv->mbox.chan);
}

static const struct of_device_id rpmi_tee_transport_match[] = {
	{ .compatible = "riscv,rpmi-tee" },
	{ }
};
MODULE_DEVICE_TABLE(of, rpmi_tee_transport_match);

static struct platform_driver rpmi_tee_transport_driver = {
	.probe = rpmi_tee_transport_probe,
	.remove = rpmi_tee_transport_remove,
	.driver = {
		.name = "riscv-rpmi-tee",
		.of_match_table = rpmi_tee_transport_match,
	},
};

module_platform_driver(rpmi_tee_transport_driver);

MODULE_DESCRIPTION("RISC-V RPMI TEE service group transport");
MODULE_LICENSE("GPL");
