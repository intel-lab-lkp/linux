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

int ele_uapi_allowed_fw_cmd(struct se_if_device_ctx *dev_ctx, struct se_msg_hdr *header,
			    u32 tx_msg_sz)
{
	struct se_api_msg *msg = container_of(header, struct se_api_msg, header);
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
	case ELE_SESSION_OPEN_REQ:
		/* Might be cleared as part of tear down. */
		ret = dev_ctx->sess_hdl ? -EEXIST : 0;
		break;
	case ELE_SESSION_CLOSE_REQ:
		/* Might be cleared as part of tear down. */
		if (!dev_ctx->sess_hdl) {
			ret = -ENXIO;
			break;
		}
		/*
		 * A close request must target this context's own session. The
		 * handle to close is carried in the payload (data[0]); reject a
		 * request whose buffer is too short to hold it, or whose handle
		 * does not match this context. Checking the buffer size first
		 * also keeps the data[0] read in bounds. This stops one process
		 * from closing - and leaking - another process's session with a
		 * spoofed handle.
		 */
		if (tx_msg_sz < ELE_SESSION_CLOSE_REQ_SZ ||
		    msg->data[0] != dev_ctx->sess_hdl)
			ret = -EINVAL;
		break;
	case ELE_STORAGE_OPEN_REQ:
		/* Might be cleared as part of tear down. */
		ret = dev_ctx->strg_hdl ? -EEXIST : 0;
		break;
	case ELE_STORAGE_CLOSE_REQ:
		/* Might be cleared as part of tear down. */
		if (!dev_ctx->strg_hdl) {
			ret = -ENXIO;
			break;
		}
		/* Same self-ownership check as the session close above. */
		if (tx_msg_sz < ELE_STORAGE_CLOSE_REQ_SZ ||
		    msg->data[0] != dev_ctx->strg_hdl)
			ret = -EINVAL;
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

		/*
		 * Record the storage handle before registering as command
		 * receiver. FW has already allocated the handle; if we assigned
		 * it only after a successful registration, a failing
		 * set_dev_ctx_as_command_receiver() (e.g. -EBUSY) would leave
		 * strg_hdl at 0 while the ioctl still returns success to
		 * userspace. The kernel would then never close the handle on
		 * teardown, leaking it in FW. Storing it first guarantees
		 * cleanup_dev_ctx() closes it regardless of registration.
		 */
		dev_ctx->strg_hdl = rx_msg->data[1];

		rc = set_dev_ctx_as_command_receiver(dev_ctx);
		if (rc)
			dev_err(priv->dev,
				"Failed to register %s as CMD-Receiver: %d\n",
				dev_ctx->devname, rc);
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

int se_close_session(struct se_if_device_ctx *dev_ctx, u32 session_hdl)
{
	struct se_api_msg *tx_msg __free(kfree) = NULL;
	struct se_api_msg *rx_msg __free(kfree) = NULL;
	struct se_if_priv *priv;
	int ret;

	if (!dev_ctx || !dev_ctx->priv)
		return -EINVAL;

	priv = dev_ctx->priv;

	tx_msg = kzalloc(ELE_SESSION_CLOSE_REQ_SZ, GFP_KERNEL);
	if (!tx_msg)
		return -ENOMEM;

	rx_msg = kzalloc(ELE_SESSION_CLOSE_RSP_SZ, GFP_KERNEL);
	if (!rx_msg)
		return -ENOMEM;

	/*
	 * Session close is a FW-API command; format it with the FW API version
	 * so se_val_rsp_hdr_n_status() below (called with is_base_api = false,
	 * i.e. expecting fw_api_ver) does not reject the matching response and
	 * wrongly report the close as failed, which would leak the handle.
	 */
	se_fill_cmd_msg_hdr(priv, (struct se_msg_hdr *)&tx_msg->header,
			    ELE_SESSION_CLOSE_REQ, ELE_SESSION_CLOSE_REQ_SZ, false);

	tx_msg->data[0] = session_hdl;

	/*
	 * Transmit on the caller's own context. Using dev_ctx (rather than
	 * hardcoding priv->priv_dev_ctx) keeps a userspace close() subject to
	 * the going_away check in ele_msg_send_rcv(): if unbind has begun and
	 * freed priv->tx_chan, the send is rejected with -ENODEV instead of
	 * touching the freed mailbox channel. The teardown path passes
	 * priv_dev_ctx so its resync closes are still let through.
	 */
	ret = ele_msg_send_rcv(dev_ctx,
			       tx_msg,
			       ELE_SESSION_CLOSE_REQ_SZ,
			       rx_msg,
			       ELE_SESSION_CLOSE_RSP_SZ);
	if (ret < 0)
		return ret;

	ret = se_val_rsp_hdr_n_status(priv,
				      rx_msg,
				      ELE_SESSION_CLOSE_REQ,
				      ELE_SESSION_CLOSE_RSP_SZ,
				      false);
	return ret;
}

int se_close_storage(struct se_if_device_ctx *dev_ctx, u32 storage_hdl)
{
	struct se_api_msg *tx_msg __free(kfree) = NULL;
	struct se_api_msg *rx_msg __free(kfree) = NULL;
	struct se_if_priv *priv;
	int ret;

	if (!dev_ctx || !dev_ctx->priv)
		return -EINVAL;

	priv = dev_ctx->priv;

	tx_msg = kzalloc(ELE_STORAGE_CLOSE_REQ_SZ, GFP_KERNEL);
	if (!tx_msg)
		return -ENOMEM;

	rx_msg = kzalloc(ELE_STORAGE_CLOSE_RSP_SZ, GFP_KERNEL);
	if (!rx_msg)
		return -ENOMEM;

	/* Same FW-API version handling as se_close_session() above. */
	se_fill_cmd_msg_hdr(priv, (struct se_msg_hdr *)&tx_msg->header,
			    ELE_STORAGE_CLOSE_REQ, ELE_STORAGE_CLOSE_REQ_SZ, false);

	tx_msg->data[0] = storage_hdl;

	/* Transmit on the caller's own context; see se_close_session(). */
	ret = ele_msg_send_rcv(dev_ctx,
			       tx_msg,
			       ELE_STORAGE_CLOSE_REQ_SZ,
			       rx_msg,
			       ELE_STORAGE_CLOSE_RSP_SZ);
	if (ret < 0)
		return ret;

	ret = se_val_rsp_hdr_n_status(priv,
				      rx_msg,
				      ELE_STORAGE_CLOSE_REQ,
				      ELE_STORAGE_CLOSE_RSP_SZ,
				      false);
	return ret;
}
