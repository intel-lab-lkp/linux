// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright 2025 NXP
 */

#include "ele_base_msg.h"
#include "ele_common.h"

int se_chk_tx_msg_hdr(struct se_if_priv *priv, struct se_msg_hdr *header)
{
	if (!header->size || header->size > MAX_WORD_SIZE)
		return -EINVAL;

	if (header->tag != priv->if_defs->cmd_tag &&
	    header->tag != priv->if_defs->rsp_tag)
		return -EINVAL;

	if (header->ver == priv->if_defs->base_api_ver)
		return ele_uapi_allowed_base_cmd(priv, header);
	else if (header->ver == priv->if_defs->fw_api_ver)
		return 0;

	return -EINVAL;
}

/*
 * se_get_msg_chksum() - to calculate checksum word by word.
 *
 * @msg : reference to the input msg-data.
 * @msg_len : reference to the input msg-data length in bytes.
 *            Includes extra 4 bytes (or 1 words) chksum.
 *
 * This function returns the checksum calculated by ORing word by word.
 *
 * Return:
 *  0: if the input length is not 4 byte aligned, or num of words < 5.
 *  chksum: calculated word by word.
 */
u32 se_get_msg_chksum(u32 *msg, u32 msg_len)
{
	u32 nb_words = msg_len / (u32)sizeof(u32);
	u32 chksum = 0;
	u32 i;

	if (nb_words < 5)
		return chksum;

	if (msg_len % SE_MSG_WORD_SZ) {
		pr_err("Msg-len is not 4-byte aligned.");
		return chksum;
	}

	/* nb_words include one checksum word, so skip it. */
	nb_words--;

	for (i = 0; i < nb_words; i++)
		chksum ^= *(msg + i);

	return chksum;
}

void set_se_rcv_msg_timeout(struct se_if_priv *priv, u32 timeout_ms)
{
	priv->se_rcv_msg_timeout_ms = timeout_ms;
}

int ele_msg_rcv(struct se_if_device_ctx *dev_ctx, struct se_clbk_handle *se_clbk_hdl)
{
	bool is_rsp_wait_with_timeout = false;
	bool wait_uninterruptible = false;
	unsigned long remaining_jiffies;
	unsigned long deadline_jiffies;
	unsigned long timeout_jiffies;
	unsigned long flags;
	int ret;

	remaining_jiffies = MAX_SCHEDULE_TIMEOUT;
	if (dev_ctx->priv->cmd_receiver_clbk_hdl.dev_ctx != dev_ctx) {
		is_rsp_wait_with_timeout = true;
		timeout_jiffies = msecs_to_jiffies(dev_ctx->priv->se_rcv_msg_timeout_ms);
		deadline_jiffies = jiffies + timeout_jiffies;
	}
	do {
		if (is_rsp_wait_with_timeout) {
			if (time_after_eq(jiffies, deadline_jiffies)) {
				ret = -ETIMEDOUT;
				break;
			}
			remaining_jiffies = deadline_jiffies - jiffies;
		}

		if (wait_uninterruptible)
			ret = wait_for_completion_timeout(&se_clbk_hdl->done,
							  remaining_jiffies);
		else
			ret = wait_for_completion_interruptible_timeout(&se_clbk_hdl->done,
									remaining_jiffies);
		if (ret == -ERESTARTSYS) {
			/*
			 * Record that a signal was observed, then continue waiting non-
			 * interruptibly until the response arrives or the timeout
			 * expires. The caller can surface the interruption to userspace
			 * after the protocol transaction is brought back to a
			 * synchronized state.
			 */
			if (dev_ctx->priv->waiting_rsp_clbk_hdl.dev_ctx) {
				dev_ctx->priv->waiting_rsp_clbk_hdl.signal_rcvd = true;
				wait_uninterruptible = true;
				continue;
			}
			ret = -EINTR;
			break;
		}

		if (ret == 0) {
			/*
			 * The response buffer belongs to the caller of ele_msg_send_rcv()
			 * and may be freed as soon as this function returns. Clear rx_msg
			 * under clbk_rx_lock so that a late se_if_rx_callback() can
			 * observe that the waiter has timed out and must not copy into
			 * the stale buffer.
			 *
			 * If the completion has not yet been signaled, mark the firmware
			 * path busy. This acts as a circuit breaker: reject new
			 * command/response transactions until the delayed response
			 * arrives and the callback closes the breaker.
			 */

			spin_lock_irqsave(&se_clbk_hdl->clbk_rx_lock, flags);
			se_clbk_hdl->rx_msg = NULL;
			if (!completion_done(&se_clbk_hdl->done))
				atomic_set(&dev_ctx->priv->fw_busy, 1);

			spin_unlock_irqrestore(&se_clbk_hdl->clbk_rx_lock, flags);
			ret = -ETIMEDOUT;
			dev_err(dev_ctx->priv->dev,
				"Fatal Error: SE interface: %s0, hangs indefinitely.\n",
				get_se_if_name(dev_ctx->priv->if_defs->se_if_type));
			break;
		}
		ret = se_clbk_hdl->rx_msg_sz;
		break;
	} while (ret < 0);

	return ret;
}

int ele_msg_send(struct se_if_device_ctx *dev_ctx,
		 void *tx_msg,
		 int tx_msg_sz)
{
	struct se_msg_hdr *header = tx_msg;
	int err;

	/*
	 * Check that the size passed as argument matches the size
	 * carried in the message.
	 */
	if (header->size << 2 != tx_msg_sz) {
		dev_err(dev_ctx->priv->dev,
			"%s: User buf hdr: 0x%x, sz mismatched with input-sz (%d != %d).",
			dev_ctx->devname, *(u32 *)header, header->size << 2, tx_msg_sz);
		return -EINVAL;
	}

	err = mbox_send_message(dev_ctx->priv->tx_chan, tx_msg);
	if (err < 0) {
		dev_err(dev_ctx->priv->dev,
			"%s: Error: mbox_send_message failure.", dev_ctx->devname);
		return err;
	}

	return tx_msg_sz;
}

/* API used for send/receive blocking call. */
int ele_msg_send_rcv(struct se_if_device_ctx *dev_ctx, void *tx_msg,
		     int tx_msg_sz, void *rx_msg, int exp_rx_msg_sz)
{
	struct se_if_priv *priv = dev_ctx->priv;
	int err;

	guard(mutex)(&priv->se_if_cmd_lock);

	if (atomic_read(&priv->fw_busy)) {
		dev_dbg(priv->dev, "%s: ELE became unresponsive.\n", dev_ctx->devname);
		return -EBUSY;
	}
	reinit_completion(&priv->waiting_rsp_clbk_hdl.done);
	priv->waiting_rsp_clbk_hdl.dev_ctx = dev_ctx;
	priv->waiting_rsp_clbk_hdl.rx_msg_sz = exp_rx_msg_sz;
	priv->waiting_rsp_clbk_hdl.rx_msg = rx_msg;

	err = ele_msg_send(dev_ctx, tx_msg, tx_msg_sz);
	if (err < 0)
		return err;

	err = ele_msg_rcv(dev_ctx, &priv->waiting_rsp_clbk_hdl);

	if (priv->waiting_rsp_clbk_hdl.signal_rcvd) {
		err = -EINTR;
		priv->waiting_rsp_clbk_hdl.signal_rcvd = false;
		dev_err(priv->dev, "%s: Err[0x%x]:Interrupted by signal.",
			dev_ctx->devname, err);
	}
	priv->waiting_rsp_clbk_hdl.rx_msg = NULL;
	priv->waiting_rsp_clbk_hdl.dev_ctx = NULL;

	return err;
}

static bool check_hdr_exception_for_sz(struct se_if_priv *priv,
				       struct se_msg_hdr *header)
{
	/*
	 * List of API(s) header that can be accepte variable length
	 * response buffer.
	 */
	if (header->command == ELE_DEBUG_DUMP_REQ &&
	    header->ver == priv->if_defs->base_api_ver &&
	    header->size >= 2 && header->size <= (ELE_DEBUG_DUMP_RSP_SZ / 4))
		return true;

	return false;
}

/*
 * Callback called by mailbox FW, when data is received.
 */
void se_if_rx_callback(struct mbox_client *mbox_cl, void *msg)
{
	struct se_clbk_handle *se_clbk_hdl;
	struct device *dev = mbox_cl->dev;
	struct se_msg_hdr *header;
	bool sz_mismatch = false;
	struct se_if_priv *priv;
	unsigned long flags;
	u32 rx_msg_sz;

	priv = dev_get_drvdata(dev);

	/* The function can be called with NULL msg */
	if (!msg) {
		dev_err(dev, "Message is invalid\n");
		return;
	}

	header = msg;
	rx_msg_sz = header->size << 2;

	/* Incoming command: wake up the receiver if any. */
	if (header->tag == priv->if_defs->cmd_tag) {
		se_clbk_hdl = &priv->cmd_receiver_clbk_hdl;
		spin_lock_irqsave(&se_clbk_hdl->clbk_rx_lock, flags);
		if (!se_clbk_hdl->dev_ctx || !se_clbk_hdl->rx_msg) {
			spin_unlock_irqrestore(&se_clbk_hdl->clbk_rx_lock, flags);
			dev_warn(dev, "No command receiver registered for message: %.8x\n",
				 *((u32 *)header));
			return;
		}

		/*
		 * cmd_tag messages are delivered only to the explicitly registered
		 * command receiver. Unlike the synchronous response waiter path, the
		 * command receiver uses a dedicated long-lived buffer installed by
		 * SE_IOCTL_ENABLE_CMD_RCV and is not subject to the timeout/circuit-
		 * breaker handling used for rsp_tag messages.
		 */
		dev_dbg(dev, "Selecting cmd receiver:%s for mesg header:0x%x.",
			se_clbk_hdl->dev_ctx->devname,  *(u32 *)header);

		/*
		 * Pre-allocated buffer of MAX_NVM_MSG_LEN
		 * as the NVM command are initiated by FW.
		 * Size is revealed as part of this call function.
		 */

		if (rx_msg_sz > MAX_NVM_MSG_LEN) {
			/* Store the response buffer maxsize in local variable.*/
			rx_msg_sz = MAX_NVM_MSG_LEN;
			sz_mismatch = true;
		}

		se_clbk_hdl->rx_msg_sz = rx_msg_sz;
		memcpy(se_clbk_hdl->rx_msg, msg, se_clbk_hdl->rx_msg_sz);
		complete(&se_clbk_hdl->done);
		spin_unlock_irqrestore(&se_clbk_hdl->clbk_rx_lock, flags);
		if (sz_mismatch)
			dev_err(dev,
				"%s: CMD-RCVER NVM: hdr(0x%x) with different sz(%d != %d).\n",
				se_clbk_hdl->dev_ctx->devname, *(u32 *)header,
				(header->size << 2), rx_msg_sz);
	} else if (header->tag == priv->if_defs->rsp_tag) {
		bool exception_for_sz_mismatch = check_hdr_exception_for_sz(priv, header);
		u32 exp_rx_msg_sz = 0;

		/*
		 * waiting_rsp_clbk_hdl.rx_msg is owned by the synchronous sender in
		 * ele_msg_send_rcv(). After timeout or error, that path clears rx_msg
		 * under clbk_rx_lock before returning to its caller, which may then free
		 * the buffer. Check rx_msg under the same lock here so a delayed response
		 * can be detected and dropped instead of copying into freed memory.
		 *
		 * A late response also closes the firmware-busy circuit breaker, allowing
		 * future command/response transactions to proceed again.
		 */
		se_clbk_hdl = &priv->waiting_rsp_clbk_hdl;
		exp_rx_msg_sz = se_clbk_hdl->rx_msg_sz;
		spin_lock_irqsave(&se_clbk_hdl->clbk_rx_lock, flags);
		if (!se_clbk_hdl->rx_msg) {
			/* Close circuit breaker on spinlock race */
			atomic_set(&priv->fw_busy, 0);
			spin_unlock_irqrestore(&se_clbk_hdl->clbk_rx_lock, flags);
			dev_info(dev, "ELE responded (late), recovery FW available.");
			return;
		}
		dev_dbg(dev, "Selecting resp waiter:%s for mesg header:0x%x.",
			se_clbk_hdl->dev_ctx->devname, *(u32 *)header);

		/*
		 * For rsp_tag traffic, the sender provides the expected response
		 * buffer size. If firmware returns a different size, clamp the copy
		 * length to the caller's buffer capacity before memcpy() and report the
		 * mismatch after dropping the spinlock.
		 */
		if (rx_msg_sz != exp_rx_msg_sz) {
			if (!exception_for_sz_mismatch)
				sz_mismatch = true;

			se_clbk_hdl->rx_msg_sz = min(rx_msg_sz, exp_rx_msg_sz);
		}
		memcpy(se_clbk_hdl->rx_msg, msg, se_clbk_hdl->rx_msg_sz);
		complete(&se_clbk_hdl->done);
		spin_unlock_irqrestore(&se_clbk_hdl->clbk_rx_lock, flags);

		if (sz_mismatch)
			dev_err(dev,
				"%s: Rsp to CMD: hdr(0x%x) with different sz(%d != %d).\n",
				se_clbk_hdl->dev_ctx->devname, *(u32 *)header,
				(header->size << 2), exp_rx_msg_sz);
	} else {
		dev_err(dev, "Failed to select a device for message: %.8x\n",
			*((u32 *)header));
	}
}

int se_val_rsp_hdr_n_status(struct se_if_priv *priv, struct se_api_msg *msg,
			    u8 msg_id, u8 sz, bool is_base_api)
{
	struct se_msg_hdr *header = &msg->header;
	u32 status;

	if (header->tag != priv->if_defs->rsp_tag) {
		dev_err(priv->dev, "MSG[0x%x] Hdr: Resp tag mismatch. (0x%x != 0x%x)",
			msg_id, header->tag, priv->if_defs->rsp_tag);
		return -EINVAL;
	}

	if (header->command != msg_id) {
		dev_err(priv->dev, "MSG Header: Cmd id mismatch. (0x%x != 0x%x)",
			header->command, msg_id);
		return -EINVAL;
	}

	if ((sz % 4) || (header->size != (sz >> 2) &&
			 !check_hdr_exception_for_sz(priv, header))) {
		dev_err(priv->dev, "MSG[0x%x] Hdr: Cmd size mismatch. (0x%x != 0x%x)",
			msg_id, header->size, (sz >> 2));
		return -EINVAL;
	}

	if (is_base_api && header->ver != priv->if_defs->base_api_ver) {
		dev_err(priv->dev,
			"MSG[0x%x] Hdr: Base API Vers mismatch. (0x%x != 0x%x)",
			msg_id, header->ver, priv->if_defs->base_api_ver);
		return -EINVAL;
	} else if (!is_base_api && header->ver != priv->if_defs->fw_api_ver) {
		dev_err(priv->dev,
			"MSG[0x%x] Hdr: FW API Vers mismatch. (0x%x != 0x%x)",
			msg_id, header->ver, priv->if_defs->fw_api_ver);
		return -EINVAL;
	}

	status = RES_STATUS(msg->data[0]);
	if (status != priv->if_defs->success_tag) {
		dev_err(priv->dev, "Command Id[%x], Response Failure = 0x%x",
			header->command, status);
		return -EPERM;
	}

	return 0;
}

int se_save_imem_state(struct se_if_priv *priv, struct se_imem_buf *imem)
{
	struct ele_dev_info s_info = {0};
	int ret;

	ret = ele_get_info(priv, &s_info);
	if (ret) {
		dev_err(priv->dev, "Failed to get info from ELE.\n");
		return ret;
	}

	/* Check for the imem-state before continue to save imem state. */
	if (s_info.d_addn_info.imem_state == ELE_IMEM_STATE_BAD)
		return -EIO;

	/*
	 * EXPORT command will save encrypted IMEM to given address,
	 * so later in resume, IMEM can be restored from the given
	 * address.
	 *
	 * Size must be at least 64 kB.
	 */
	ret = ele_service_swap(priv, imem->phyaddr, ELE_IMEM_SIZE, ELE_IMEM_EXPORT);
	if (ret < 0) {
		dev_err(priv->dev, "Failed to export IMEM.");
		imem->size = 0;
	} else {
		dev_dbg(priv->dev,
			"Exported %d bytes of encrypted IMEM.",
			ret);
		imem->size = ret;
	}

	return ret > 0 ? 0 : ret;
}

int se_restore_imem_state(struct se_if_priv *priv, struct se_imem_buf *imem)
{
	struct ele_dev_info s_info;
	int ret;

	/* get info from ELE */
	ret = ele_get_info(priv, &s_info);
	if (ret) {
		dev_err(priv->dev, "Failed to get info from ELE.");
		return ret;
	}
	imem->state = s_info.d_addn_info.imem_state;

	/* Check for the imem-state and imem-size before continue to
	 * restore imem state.
	 */
	if (s_info.d_addn_info.imem_state != ELE_IMEM_STATE_BAD || !imem->size)
		return -EIO;

	/*
	 * IMPORT command will restore IMEM from the given
	 * address, here size is the actual size returned by ELE
	 * during the export operation
	 */
	ret = ele_service_swap(priv, imem->phyaddr, imem->size, ELE_IMEM_IMPORT);
	if (ret) {
		dev_err(priv->dev, "Failed to import IMEM");
		return ret;
	}

	/*
	 * After importing IMEM, check if IMEM state is equal to 0xCA
	 * to ensure IMEM is fully loaded and
	 * ELE functionality can be used.
	 */
	ret = ele_get_info(priv, &s_info);
	if (ret) {
		dev_err(priv->dev, "Failed to get info from ELE.");
		return ret;
	}
	imem->state = s_info.d_addn_info.imem_state;

	if (s_info.d_addn_info.imem_state == ELE_IMEM_STATE_OK)
		dev_dbg(priv->dev, "Successfully restored IMEM.");
	else
		dev_err(priv->dev, "Failed to restore IMEM.");

	return ret;
}
