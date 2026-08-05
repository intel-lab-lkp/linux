// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright 2026 NXP
 */

#include "se_ctrl.h"
#include "ele_common.h"
#include "ele_fw_api.h"

static bool se_cmd_receiver_allowed_cmd(u8 cmd)
{
	switch (cmd) {
	case ELE_SESSION_CLOSE_REQ:
	case ELE_STORAGE_CLOSE_REQ:
	case ELE_STORAGE_MASTER_IMPORT_REQ:
		return true;
	default:
		return false;
	}
}

int ele_uapi_allowed_fw_cmd(struct se_if_device_ctx *dev_ctx, struct se_msg_hdr *header)
{
	struct se_if_priv *priv = dev_ctx->priv;
	bool is_cmd_receiver = false;
	int ret = 0;

	scoped_guard(mutex, &priv->modify_lock)
		if (dev_ctx == priv->cmd_receiver_clbk_hdl.dev_ctx)
			is_cmd_receiver = true;

	if (header->tag == priv->if_defs->cmd_tag) {
		if (is_cmd_receiver && !se_cmd_receiver_allowed_cmd(header->command))
			return -EOPNOTSUPP;
	}

	if (header->tag == priv->if_defs->rsp_tag && !is_cmd_receiver)
		return -EOPNOTSUPP;

	switch (header->command) {
	case ELE_SESSION_CLOSE_REQ:
		/* Might be cleared as part of tear down. */
		ret = dev_ctx->sess_hdl ? 0 : -ENXIO;
		break;
	case ELE_STORAGE_CLOSE_REQ:
		/* Might be cleared as part of tear down. */
		ret = dev_ctx->strg_hdl ? 0 : -ENXIO;
		break;
	}

	return ret;
}

void fw_api_specific_ops(struct se_if_device_ctx *dev_ctx, struct se_api_msg *rx_msg)
{
	struct se_msg_hdr *header = &rx_msg->header;
	struct se_if_priv *priv = dev_ctx->priv;

	switch (header->command) {
	case ELE_SESSION_OPEN_REQ:
		dev_ctx->sess_hdl = rx_msg->data[1];
		break;
	case ELE_SESSION_CLOSE_REQ:
		dev_ctx->sess_hdl = 0;
		break;
	case ELE_STORAGE_CLOSE_REQ:
		scoped_guard(mutex, &priv->modify_lock)
			unset_dev_ctx_as_command_receiver(dev_ctx);
		dev_ctx->strg_hdl = 0;
		break;
	case ELE_STORAGE_OPEN_REQ: {
		int rc = 0;

		rc = set_dev_ctx_as_command_receiver(dev_ctx);
		if (rc) {
			dev_err(priv->dev,
				"Failed to register %s as CMD-Receiver: %d\n",
				dev_ctx->devname, rc);
			break;
		}
		dev_ctx->strg_hdl = rx_msg->data[1];
		break;
	}
	default:
		dev_dbg(priv->dev, "%s: Unknown command = 0x%x.",
			dev_ctx->devname, header->command);
	}
}

/*
 * Return true when tx_msg is one of the close requests the driver issues
 * from its own teardown path (session/storage close). ele_msg_send_rcv()
 * uses this to let those close messages through even after going_away is
 * set, so the kernel can still resynchronise session/storage state with FW.
 */
bool is_msg_xchng_for_tdown(void *tx_msg)
{
	struct se_msg_hdr *header = &((struct se_api_msg *)tx_msg)->header;

	return (header->command == ELE_SESSION_CLOSE_REQ ||
		header->command == ELE_STORAGE_CLOSE_REQ);
}

int se_close_session(struct se_if_priv *priv, u32 session_hdl)
{
	struct se_api_msg *tx_msg __free(kfree) = NULL;
	struct se_api_msg *rx_msg __free(kfree) = NULL;
	int ret;

	if (!priv || !priv->priv_dev_ctx) {
		ret = -EINVAL;
		goto exit;
	}

	tx_msg = kzalloc(ELE_SESSION_CLOSE_REQ_SZ, GFP_KERNEL);
	if (!tx_msg) {
		ret = -ENOMEM;
		goto exit;
	}

	rx_msg = kzalloc(ELE_SESSION_CLOSE_RSP_SZ, GFP_KERNEL);
	if (!rx_msg) {
		ret = -ENOMEM;
		goto exit;
	}

	se_fill_cmd_msg_hdr(priv, (struct se_msg_hdr *)&tx_msg->header,
			    ELE_SESSION_CLOSE_REQ, ELE_SESSION_CLOSE_REQ_SZ, true);

	tx_msg->data[0] = session_hdl;

	ret = ele_msg_send_rcv(priv->priv_dev_ctx,
			       tx_msg,
			       ELE_SESSION_CLOSE_REQ_SZ,
			       rx_msg,
			       ELE_SESSION_CLOSE_RSP_SZ);
	if (ret < 0)
		goto exit;

	ret = se_val_rsp_hdr_n_status(priv,
				      rx_msg,
				      ELE_SESSION_CLOSE_REQ,
				      ELE_SESSION_CLOSE_RSP_SZ,
				      false);
exit:
	return ret;
}

int se_close_storage(struct se_if_priv *priv, u32 storage_hdl)
{
	struct se_api_msg *tx_msg __free(kfree) = NULL;
	struct se_api_msg *rx_msg __free(kfree) = NULL;
	int ret;

	if (!priv || !priv->priv_dev_ctx) {
		ret = -EINVAL;
		goto exit;
	}

	tx_msg = kzalloc(ELE_STORAGE_CLOSE_REQ_SZ, GFP_KERNEL);
	if (!tx_msg) {
		ret = -ENOMEM;
		goto exit;
	}

	rx_msg = kzalloc(ELE_STORAGE_CLOSE_RSP_SZ, GFP_KERNEL);
	if (!rx_msg) {
		ret = -ENOMEM;
		goto exit;
	}

	se_fill_cmd_msg_hdr(priv, (struct se_msg_hdr *)&tx_msg->header,
			    ELE_STORAGE_CLOSE_REQ, ELE_STORAGE_CLOSE_REQ_SZ, true);

	tx_msg->data[0] = storage_hdl;

	ret = ele_msg_send_rcv(priv->priv_dev_ctx,
			       tx_msg,
			       ELE_STORAGE_CLOSE_REQ_SZ,
			       rx_msg,
			       ELE_STORAGE_CLOSE_RSP_SZ);
	if (ret < 0)
		goto exit;

	ret = se_val_rsp_hdr_n_status(priv,
				      rx_msg,
				      ELE_STORAGE_CLOSE_REQ,
				      ELE_STORAGE_CLOSE_RSP_SZ,
				      false);
exit:
	return ret;
}
