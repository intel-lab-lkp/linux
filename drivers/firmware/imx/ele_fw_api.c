// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright 2026 NXP
 */

#include "se_ctrl.h"
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

	scoped_guard(mutex, &priv->modify_lock)
		if (dev_ctx == priv->cmd_receiver_clbk_hdl.dev_ctx)
			is_cmd_receiver = true;

	if (header->tag == priv->if_defs->cmd_tag) {
		if (is_cmd_receiver && !se_cmd_receiver_allowed_cmd(header->command))
			return -EOPNOTSUPP;
	}

	if (header->tag == priv->if_defs->rsp_tag && !is_cmd_receiver)
		return -EOPNOTSUPP;

	return 0;
}

void fw_api_specific_ops(struct se_if_device_ctx *dev_ctx, struct se_msg_hdr *header)
{
	if (header->command == ELE_STORAGE_OPEN_REQ) {
		int rc = 0;

		rc = set_dev_ctx_as_command_receiver(dev_ctx);
		if (rc)
			dev_err(dev_ctx->priv->dev,
				"Failed to register %s as CMD-Receiver: %d\n",
				dev_ctx->devname, rc);
	}
	if (header->command == ELE_STORAGE_CLOSE_REQ) {
		scoped_guard(mutex, &dev_ctx->priv->modify_lock)
			unset_dev_ctx_as_command_receiver(dev_ctx);
	}
}

