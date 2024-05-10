// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright 2024 NXP
 */

#include "ele_base_msg.h"
#include "ele_common.h"

u32 plat_add_msg_crc(u32 *msg, u32 msg_len)
{
	u32 nb_words = msg_len / (u32)sizeof(u32);
	u32 crc = 0;
	u32 i;

	for (i = 0; i < nb_words - 1; i++)
		crc ^= *(msg + i);

	return crc;
}

int imx_ele_msg_rcv(struct se_if_priv *priv)
{
	u32 wait;
	int err;

	wait = msecs_to_jiffies(1000);
	if (!wait_for_completion_timeout(&priv->done, wait)) {
		dev_err(priv->dev,
				"Error: wait_for_completion timed out.\n");
		err = -ETIMEDOUT;
	}

	mutex_unlock(&priv->se_if_cmd_lock);
	priv->no_dev_ctx_used = false;

	return err;
}

int imx_ele_msg_send(struct se_if_priv *priv, void *mssg)
{
	bool is_cmd_lock_tobe_taken = false;
	int err;

	if (!priv->waiting_rsp_dev || priv->no_dev_ctx_used) {
		is_cmd_lock_tobe_taken = true;
		mutex_lock(&priv->se_if_cmd_lock);
	}
	scoped_guard(mutex, &priv->se_if_lock);

	err = mbox_send_message(priv->tx_chan, mssg);
	if (err < 0) {
		dev_err(priv->dev, "Error: mbox_send_message failure.\n");
		if (is_cmd_lock_tobe_taken)
			mutex_unlock(&priv->se_if_cmd_lock);
		return err;
	}
	err = 0;

	return err;
}

int imx_ele_msg_send_rcv(struct se_if_priv *priv, void *mssg)
{
	int err;

	priv->no_dev_ctx_used = true;
	err = imx_ele_msg_send(priv, mssg);
	if (err)
		goto exit;

	err = imx_ele_msg_rcv(priv);

exit:
	return err;
}

int imx_ele_miscdev_msg_rcv(struct se_if_device_ctx *dev_ctx)
{
	struct se_msg_hdr header = {0};
	int err;

	err = wait_event_interruptible(dev_ctx->wq, dev_ctx->pending_hdr != 0);
	if (err)
		dev_err(dev_ctx->dev,
			"%s: Err[0x%x]:Interrupted by signal.\n",
			dev_ctx->miscdev.name, err);

	header = *((struct se_msg_hdr *) (&dev_ctx->temp_resp[0]));

	if (header.tag == dev_ctx->priv->rsp_tag)
		mutex_unlock(&dev_ctx->priv->se_if_cmd_lock);

	return err;
}

int imx_ele_miscdev_msg_send(struct se_if_device_ctx *dev_ctx,
			     void *tx_msg, int tx_msg_sz)
{
	struct se_if_priv *priv = dev_ctx->priv;
	struct se_msg_hdr header = {0};
	int err;

	header = *((struct se_msg_hdr *) tx_msg);

	/*
	 * Check that the size passed as argument matches the size
	 * carried in the message.
	 */
	err = header.size << 2;

	if (err != tx_msg_sz) {
		err = -EINVAL;
		dev_err(priv->dev,
			"%s: User buffer too small\n",
				dev_ctx->miscdev.name);
		goto exit;
	}
	/* Check the message is valid according to tags */
	if (header.tag == priv->cmd_tag)
		priv->waiting_rsp_dev = dev_ctx;
	else if (header.tag == priv->rsp_tag) {
		/* Check the device context can send the command */
		if (dev_ctx != priv->cmd_receiver_dev) {
			dev_err(priv->dev,
				"%s: Channel not configured to send resp to FW.",
				dev_ctx->miscdev.name);
			err = -EPERM;
			goto exit;
		}
	} else {
		dev_err(priv->dev,
			"%s: The message does not have a valid TAG\n",
				dev_ctx->miscdev.name);
		err = -EINVAL;
		goto exit;
	}
	err = imx_ele_msg_send(priv, tx_msg);
exit:
	return err;
}

/*
 * Callback called by mailbox FW, when data is received.
 */
void se_if_rx_callback(struct mbox_client *mbox_cl, void *msg)
{
	struct device *dev = mbox_cl->dev;
	struct se_if_device_ctx *dev_ctx;
	struct se_api_msg *rx_msg;
	bool is_response = false;
	struct se_if_priv *priv;
	struct se_msg_hdr header;

	priv = dev_get_drvdata(dev);
	if (!priv) {
		dev_err(dev, "SE-MU Priv data is NULL;");
		return;
	}

	/* The function can be called with NULL msg */
	if (!msg) {
		dev_err(dev, "Message is invalid\n");
		return;
	}

	header.tag = ((u8 *)msg)[TAG_OFFSET];
	header.command = ((u8 *)msg)[CMD_OFFSET];
	header.size = ((u8 *)msg)[SZ_OFFSET];
	header.ver = ((u8 *)msg)[VER_OFFSET];

	/* Incoming command: wake up the receiver if any. */
	if (header.tag == priv->cmd_tag) {
		dev_dbg(dev, "Selecting cmd receiver\n");
		dev_ctx = priv->cmd_receiver_dev;
	} else if (header.tag == priv->rsp_tag) {
		if (priv->waiting_rsp_dev) {
			dev_dbg(dev, "Selecting rsp waiter\n");
			dev_ctx = priv->waiting_rsp_dev;
			is_response = true;
		} else {
			/*
			 * Reading the EdgeLock Enclave response
			 * to the command, sent by other
			 * linux kernel services.
			 */
			spin_lock(&priv->lock);
			memcpy(&priv->rx_msg, msg, header.size << 2);

			complete(&priv->done);
			spin_unlock(&priv->lock);
			return;
		}
	} else {
		dev_err(dev, "Failed to select a device for message: %.8x\n",
				*((u32 *) &header));
		return;
	}
	/* Init reception */
	rx_msg = kzalloc(header.size << 2, GFP_KERNEL);
	if (rx_msg)
		memcpy(rx_msg, msg, header.size << 2);

	dev_ctx->temp_resp = (u32 *)rx_msg;
	dev_ctx->temp_resp_size = header.size;

	/* Allow user to read */
	dev_ctx->pending_hdr = 1;
	wake_up_interruptible(&dev_ctx->wq);

	if (is_response)
		priv->waiting_rsp_dev = NULL;
}

int validate_rsp_hdr(struct se_if_priv *priv, u32 header,
		     uint8_t msg_id, uint8_t sz, bool is_base_api)
{
	int ret = -EINVAL;
	u32 size;
	u32 cmd;
	u32 tag;
	u32 ver;

	tag = MSG_TAG(header);
	cmd = MSG_COMMAND(header);
	size = MSG_SIZE(header);
	ver = MSG_VER(header);

	do {
		if (tag != priv->rsp_tag) {
			dev_err(priv->dev,
				"MSG[0x%x] Hdr: Resp tag mismatch. (0x%x != 0x%x)",
				msg_id, tag, priv->rsp_tag);
			break;
		}

		if (cmd != msg_id) {
			dev_err(priv->dev,
				"MSG Header: Cmd id mismatch. (0x%x != 0x%x)",
				cmd, msg_id);
			break;
		}

		if (size != (sz >> 2)) {
			dev_err(priv->dev,
				"MSG[0x%x] Hdr: Cmd size mismatch. (0x%x != 0x%x)",
				msg_id, size, (sz >> 2));
			break;
		}

		if (is_base_api && (ver != priv->base_api_ver)) {
			dev_err(priv->dev,
				"MSG[0x%x] Hdr: Base API Vers mismatch. (0x%x != 0x%x)",
				msg_id, ver, priv->base_api_ver);
			break;
		} else if (!is_base_api && ver != priv->fw_api_ver) {
			dev_err(priv->dev,
				"MSG[0x%x] Hdr: FW API Vers mismatch. (0x%x != 0x%x)",
				msg_id, ver, priv->fw_api_ver);
			break;
		}

		ret = 0;

	} while (false);

	return ret;
}

int se_save_imem_state(struct device *dev)
{
	struct se_if_priv *priv = dev_get_drvdata(dev);
	int ret;

	/* EXPORT command will save encrypted IMEM to given address,
	 * so later in resume, IMEM can be restored from the given
	 * address.
	 *
	 * Size must be at least 64 kB.
	 */
	ret = ele_service_swap(dev,
			       priv->imem.phyaddr,
			       ELE_IMEM_SIZE,
			       ELE_IMEM_EXPORT);
	if (ret < 0)
		dev_err(dev, "Failed to export IMEM\n");
	else
		dev_info(dev,
			"Exported %d bytes of encrypted IMEM\n",
			ret);

	return ret;
}

int se_restore_imem_state(struct device *dev)
{
	struct se_if_priv *priv = dev_get_drvdata(dev);
	struct soc_info s_info;
	int ret;

	/* get info from ELE */
	ret = ele_get_info(dev, &s_info);
	if (ret) {
		dev_err(dev, "Failed to get info from ELE.\n");
		return ret;
	}

	/* Get IMEM state, if 0xFE then import IMEM */
	if (s_info.imem_state == ELE_IMEM_STATE_BAD) {
		/* IMPORT command will restore IMEM from the given
		 * address, here size is the actual size returned by ELE
		 * during the export operation
		 */
		ret = ele_service_swap(dev,
				       priv->imem.phyaddr,
				       priv->imem.size,
				       ELE_IMEM_IMPORT);
		if (ret) {
			dev_err(dev, "Failed to import IMEM\n");
			goto exit;
		}
	} else
		goto exit;

	/* After importing IMEM, check if IMEM state is equal to 0xCA
	 * to ensure IMEM is fully loaded and
	 * ELE functionality can be used.
	 */
	ret = ele_get_info(dev, &s_info);
	if (ret) {
		dev_err(dev, "Failed to get info from ELE.\n");
		goto exit;
	}

	if (s_info.imem_state == ELE_IMEM_STATE_OK)
		dev_info(dev, "Successfully restored IMEM\n");
	else
		dev_err(dev, "Failed to restore IMEM\n");

exit:
	return ret;
}
