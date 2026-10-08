// SPDX-License-Identifier: GPL-2.0-only
/*
 * RISC-V RPMI TEE transport
 *
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#include <linux/unaligned.h>
#include <linux/mailbox_client.h>
#include <linux/mailbox/riscv-rpmi-message.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/rpmi_tee.h>

#include "rpmi_tee_private.h"

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

	priv->mbox.max_msg_data_size = value;

	return 0;
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

	return 0;

out_failed:
	mbox_free_channel(priv->mbox.chan);

	return ret;
}

static void rpmi_tee_transport_remove(struct platform_device *pdev)
{
	struct rpmi_tee_transport *priv = platform_get_drvdata(pdev);

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
