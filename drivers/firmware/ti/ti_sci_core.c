// SPDX-License-Identifier: GPL-2.0
/*
 * Texas Instruments System Control Interface Protocol Driver
 *
 * Copyright (C) 2015-2025 Texas Instruments Incorporated - https://www.ti.com/
 *	Nishanth Menon
 */

#define pr_fmt(fmt) "%s: " fmt, __func__

#include <linux/bitmap.h>
#include <linux/clk.h>
#include <linux/cpu.h>
#include <linux/debugfs.h>
#include <linux/export.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/pm_qos.h>
#include <linux/property.h>
#include <linux/slab.h>
#include <linux/suspend.h>
#include <linux/sys_soc.h>
#include <linux/reboot.h>

#include "ti_sci.h"

/* List of all TI SCI devices active in system */
static LIST_HEAD(ti_sci_list);
/* Protection for the entire list */
static DEFINE_MUTEX(ti_sci_list_mutex);

#ifdef CONFIG_DEBUG_FS

/**
 * ti_sci_debug_show() - Helper to dump the debug log
 * @s:	sequence file pointer
 * @unused:	unused.
 *
 * Return: 0
 */
static int ti_sci_debug_show(struct seq_file *s, void *unused)
{
	struct ti_sci_info *info = s->private;

	memcpy_fromio(info->debug_buffer, info->debug_region,
		      info->debug_region_size);
	/*
	 * We don't trust firmware to leave NULL terminated last byte (hence
	 * we have allocated 1 extra 0 byte). Since we cannot guarantee any
	 * specific data format for debug messages, We just present the data
	 * in the buffer as is - we expect the messages to be self explanatory.
	 */
	seq_puts(s, info->debug_buffer);
	return 0;
}

/* Provide the log file operations interface*/
DEFINE_SHOW_ATTRIBUTE(ti_sci_debug);

/**
 * ti_sci_debugfs_create() - Create log debug file
 * @pdev:	platform device pointer
 * @info:	Pointer to SCI entity information
 *
 * Return: 0 if all went fine, else corresponding error.
 */
static int ti_sci_debugfs_create(struct platform_device *pdev,
				 struct ti_sci_info *info)
{
	struct device *dev = &pdev->dev;
	struct resource *res;
	char debug_name[50];

	/* Debug region is optional */
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM,
					   "debug_messages");
	info->debug_region = devm_ioremap_resource(dev, res);
	if (IS_ERR(info->debug_region))
		return 0;
	info->debug_region_size = resource_size(res);

	info->debug_buffer = devm_kcalloc(dev, info->debug_region_size + 1,
					  sizeof(char), GFP_KERNEL);
	if (!info->debug_buffer)
		return -ENOMEM;
	/* Setup NULL termination */
	info->debug_buffer[info->debug_region_size] = 0;

	snprintf(debug_name, sizeof(debug_name), "ti_sci_debug@%s",
		 dev_name(dev));
	info->d = debugfs_create_file(debug_name, 0444, NULL, info,
				      &ti_sci_debug_fops);
	if (IS_ERR(info->d))
		return PTR_ERR(info->d);

	dev_dbg(dev, "Debug region => %p, size = %zu bytes, resource: %pr\n",
		info->debug_region, info->debug_region_size, res);
	return 0;
}

#else /* CONFIG_DEBUG_FS */
static inline int ti_sci_debugfs_create(struct platform_device *dev,
					struct ti_sci_info *info)
{
	return 0;
}

static inline void ti_sci_debugfs_destroy(struct platform_device *dev,
					  struct ti_sci_info *info)
{
}
#endif /* CONFIG_DEBUG_FS */

/**
 * ti_sci_dump_header_dbg() - Helper to dump a message header.
 * @dev:	Device pointer corresponding to the SCI entity
 * @hdr:	pointer to header.
 */
static inline void ti_sci_dump_header_dbg(struct device *dev,
					  struct ti_sci_msg_hdr *hdr)
{
	dev_dbg(dev, "MSGHDR:type=0x%04x host=0x%02x seq=0x%02x flags=0x%08x\n",
		hdr->type, hdr->host, hdr->seq, hdr->flags);
}

/**
 * ti_sci_rx_callback() - mailbox client callback for receive messages
 * @cl:	client pointer
 * @m:	mailbox message
 *
 * Processes one received message to appropriate transfer information and
 * signals completion of the transfer.
 *
 * NOTE: This function will be invoked in IRQ context, hence should be
 * as optimal as possible.
 */
static void ti_sci_rx_callback(struct mbox_client *cl, void *m)
{
	struct ti_sci_info *info = cl_to_ti_sci_info(cl);
	struct device *dev = info->dev;
	struct ti_sci_xfers_info *minfo = &info->minfo;
	struct ti_msgmgr_message *mbox_msg = m;
	struct ti_sci_msg_hdr *hdr = (struct ti_sci_msg_hdr *)mbox_msg->buf;
	struct ti_sci_xfer *xfer;
	u8 xfer_id;

	xfer_id = hdr->seq;

	/*
	 * Are we even expecting this?
	 * NOTE: barriers were implicit in locks used for modifying the bitmap
	 */
	if (!test_bit(xfer_id, minfo->xfer_alloc_table)) {
		dev_err(dev, "Message for %d is not expected!\n", xfer_id);
		return;
	}

	xfer = &minfo->xfer_block[xfer_id];

	/* Is the message of valid length? */
	if (mbox_msg->len > info->desc->max_msg_size) {
		dev_err(dev, "Unable to handle %zu xfer(max %d)\n",
			mbox_msg->len, info->desc->max_msg_size);
		ti_sci_dump_header_dbg(dev, hdr);
		return;
	}
	if (mbox_msg->len < xfer->rx_len) {
		dev_err(dev, "Recv xfer %zu < expected %d length\n",
			mbox_msg->len, xfer->rx_len);
		ti_sci_dump_header_dbg(dev, hdr);
		return;
	}

	ti_sci_dump_header_dbg(dev, hdr);
	/* Take a copy to the rx buffer.. */
	memcpy(xfer->rx_buf, mbox_msg->buf, xfer->rx_len);
	complete(&xfer->done);
}

/**
 * ti_sci_get_one_xfer() - Allocate one message
 * @info:	Pointer to SCI entity information
 * @msg_type:	Message type
 * @msg_flags:	Flag to set for the message
 * @tx_message: transmit message storage location
 * @tx_message_size: transmit message size
 * @rx_message: receive message storage location
 * @rx_message_size: receive message size
 * @caller:	Address of the command function, used in error messages
 *
 * Helper function which is used by various command functions that are
 * exposed to clients of this driver for allocating a message traffic event.
 *
 * This function can sleep depending on pending requests already in the system
 * for the SCI entity. Further, this also holds a spinlock to maintain integrity
 * of internal data structures.
 *
 * Return: 0 if all went fine, else corresponding error.
 */
static struct ti_sci_xfer *ti_sci_get_one_xfer(struct ti_sci_info *info,
					       u16 msg_type, u32 msg_flags,
					       void *tx_message, size_t tx_message_size,
					       void *rx_message, size_t rx_message_size,
					       void *caller)
{
	struct ti_sci_xfers_info *minfo = &info->minfo;
	struct device *dev = info->dev;
	struct ti_sci_xfer *xfer;
	struct ti_sci_msg_hdr *hdr;
	unsigned long flags;
	unsigned long bit_pos;
	u8 xfer_id;
	int ret;
	int timeout;

	/* Ensure we have sane transfer sizes */
	if (rx_message_size > info->desc->max_msg_size ||
	    tx_message_size > info->desc->max_msg_size ||
	    rx_message_size < sizeof(*hdr) || tx_message_size < sizeof(*hdr)) {
		dev_err(dev, "TI-SCI message size out of range (caller: %pS)\n",
			caller);
		return ERR_PTR(-ERANGE);
	}

	/*
	 * Ensure we have only controlled number of pending messages.
	 * Ideally, we might just have to wait a single message, be
	 * conservative and wait 5 times that..
	 */
	timeout = msecs_to_jiffies(info->desc->max_rx_timeout_ms) * 5;
	ret = down_timeout(&minfo->sem_xfer_count, timeout);
	if (ret < 0) {
		dev_err(dev, "TI-SCI timeout waiting for transfer semaphore (caller: %pS)\n",
			caller);
		return ERR_PTR(ret);
	}

	/* Keep the locked section as small as possible */
	spin_lock_irqsave(&minfo->xfer_lock, flags);
	bit_pos = find_first_zero_bit(minfo->xfer_alloc_table,
				      info->desc->max_msgs);
	set_bit(bit_pos, minfo->xfer_alloc_table);
	spin_unlock_irqrestore(&minfo->xfer_lock, flags);

	/*
	 * We already ensured in probe that we can have max messages that can
	 * fit in  hdr.seq - NOTE: this improves access latencies
	 * to predictable O(1) access, BUT, it opens us to risk if
	 * remote misbehaves with corrupted message sequence responses.
	 * If that happens, we are going to be messed up anyways..
	 */
	xfer_id = (u8)bit_pos;

	xfer = &minfo->xfer_block[xfer_id];

	xfer->tx_message.len = tx_message_size;
	xfer->tx_message.chan_rx = info->chan_rx;
	xfer->tx_message.timeout_rx_ms = info->desc->max_rx_timeout_ms;
	xfer->tx_message.buf = tx_message;
	xfer->rx_buf = rx_message;
	xfer->rx_len = (u8)rx_message_size;

	reinit_completion(&xfer->done);

	hdr = (struct ti_sci_msg_hdr *)tx_message;
	hdr->seq = xfer_id;
	hdr->type = msg_type;
	hdr->host = info->host_id;
	hdr->flags = msg_flags;

	return xfer;
}

/**
 * ti_sci_put_one_xfer() - Release a message
 * @minfo:	transfer info pointer
 * @xfer:	message that was reserved by ti_sci_get_one_xfer
 *
 * This holds a spinlock to maintain integrity of internal data structures.
 */
static void ti_sci_put_one_xfer(struct ti_sci_xfers_info *minfo,
				struct ti_sci_xfer *xfer)
{
	unsigned long flags;
	struct ti_sci_msg_hdr *hdr;
	u8 xfer_id;

	hdr = (struct ti_sci_msg_hdr *)xfer->tx_message.buf;
	xfer_id = hdr->seq;

	/*
	 * Keep the locked section as small as possible
	 * NOTE: we might escape with smp_mb and no lock here..
	 * but just be conservative and symmetric.
	 */
	spin_lock_irqsave(&minfo->xfer_lock, flags);
	clear_bit(xfer_id, minfo->xfer_alloc_table);
	spin_unlock_irqrestore(&minfo->xfer_lock, flags);

	/* Increment the count for the next user to get through */
	up(&minfo->sem_xfer_count);
}

/**
 * ti_sci_is_response_ack() - Generic ACK/NACK message checkup
 * @r:	pointer to response buffer
 *
 * Return: true if the response was an ACK, else returns false.
 */
static inline bool ti_sci_is_response_ack(void *r)
{
	struct ti_sci_msg_hdr *hdr = r;

	return hdr->flags & TI_SCI_FLAG_RESP_GENERIC_ACK ? true : false;
}

/**
 * ti_sci_do_xfer() - Do one transfer
 * @handle:	Pointer to TI SCI handle
 * @msg_type:	Message type
 * @msg_flags:	Flag to set for the message
 * @tx_message: transmit message storage location
 * @tx_message_size: transmit message size
 * @rx_message: receive message storage location
 * @rx_message_size: receive message size
 *
 * Return: -EINVAL if @handle is NULL, or
 *         PTR_ERR(@handle) if @handle is an error pointer, or
 *         -ETIMEDOUT in case of no response, or
 *         -ENODEV in case of a NACK response, or
 *         corresponding error if xfer allocation fails, or
 *         corresponding error if transmit fails, or
 *         return 0, if all goes well.
 */
int ti_sci_do_xfer(const struct ti_sci_handle *handle,
		   u16 msg_type, u32 msg_flags,
		   void *tx_message, size_t tx_message_size,
		   void *rx_message, size_t rx_message_size)
{
	struct ti_sci_info *info;
	struct ti_sci_xfer *xfer;
	bool response_expected = !!(msg_flags & (TI_SCI_FLAG_REQ_ACK_ON_PROCESSED |
						 TI_SCI_FLAG_REQ_ACK_ON_RECEIVED));
	int ret;
	int timeout;
	struct device *dev;
	bool done_state = true;
	void *caller;

	/*
	 * Address of the TI SCI command function, used in error messages.
	 * This is only accurate if this function is not inlined, otherwise
	 * the caller of the command function is reported instead.
	 */
	caller = (void *)_RET_IP_;

	if (IS_ERR(handle))
		return PTR_ERR(handle);
	if (!handle)
		return -EINVAL;

	info = handle_to_ti_sci_info(handle);
	dev = info->dev;

	xfer = ti_sci_get_one_xfer(info, msg_type, msg_flags,
				   tx_message, tx_message_size,
				   rx_message, rx_message_size, caller);
	if (IS_ERR(xfer))
		return PTR_ERR(xfer);

	ret = mbox_send_message(info->chan_tx, &xfer->tx_message);
	if (ret < 0) {
		dev_err(dev, "Mbox send fail %d (caller: %pS)\n", ret, caller);
		goto fail;
	}

	ret = 0;

	if (response_expected && system_state <= SYSTEM_RUNNING) {
		/* And we wait for the response. */
		timeout = msecs_to_jiffies(info->desc->max_rx_timeout_ms);
		if (!wait_for_completion_timeout(&xfer->done, timeout))
			ret = -ETIMEDOUT;
	} else if (response_expected) {
		/*
		 * If we are !running, we cannot use wait_for_completion_timeout
		 * during noirq phase, so we must manually poll the completion.
		 */
		ret = read_poll_timeout_atomic(try_wait_for_completion, done_state,
					       done_state, 1,
					       info->desc->max_rx_timeout_ms * 1000,
					       false, &xfer->done);
	}

	if (ret == -ETIMEDOUT)
		dev_err(dev, "Mbox timedout in resp(caller: %pS)\n", caller);

	/*
	 * NOTE: we might prefer not to need the mailbox ticker to manage the
	 * transfer queueing since the protocol layer queues things by itself.
	 * Unfortunately, we have to kick the mailbox framework after we have
	 * received our message.
	 */
	mbox_client_txdone(info->chan_tx, ret);

	/*
	 * If we expect a response and are not otherwise already in an error
	 * state, then ensure that the response is an ACK
	 */
	if (response_expected && ret == 0) {
		if (!ti_sci_is_response_ack(xfer->rx_buf)) {
			dev_warn(dev, "Message response not acknowledged (caller: %pS)\n",
				 caller);
			ret = -ENODEV;
		}
	}

fail:
	ti_sci_put_one_xfer(&info->minfo, xfer);

	return ret;
}

/**
 * ti_sci_cmd_get_revision() - command to get the revision of the SCI entity
 * @info:	Pointer to SCI entity information
 *
 * Updates the SCI information in the internal data structure.
 *
 * Return: 0 if all went fine, else return appropriate error.
 */
static int ti_sci_cmd_get_revision(struct ti_sci_info *info)
{
	struct ti_sci_handle *handle = &info->handle;
	struct ti_sci_version_info *ver = &handle->version;
	struct ti_sci_msg_hdr req;
	struct ti_sci_msg_resp_version resp;
	int ret;

	ret = ti_sci_do_xfer(handle, TI_SCI_MSG_VERSION,
			     TI_SCI_FLAG_REQ_ACK_ON_PROCESSED,
			     &req, sizeof(req),
			     &resp, sizeof(resp));
	if (ret)
		return ret;

	ver->abi_major = resp.abi_major;
	ver->abi_minor = resp.abi_minor;
	ver->firmware_revision = resp.firmware_revision;
	strscpy(ver->firmware_description, resp.firmware_description,
		sizeof(ver->firmware_description));

	return 0;
}

/**
 * ti_sci_cmd_prepare_sleep() - Prepare system for system suspend
 * @handle:		pointer to TI SCI handle
 * @mode:		Device identifier
 * @ctx_lo:		Low part of address for context save
 * @ctx_hi:		High part of address for context save
 * @debug_flags:	Debug flags to pass to firmware
 *
 * Return: 0 if all went well, else returns appropriate error value.
 */
static int ti_sci_cmd_prepare_sleep(const struct ti_sci_handle *handle, u8 mode,
				    u32 ctx_lo, u32 ctx_hi, u32 debug_flags)
{
	u32 msg_flags = mode == TISCI_MSG_VALUE_SLEEP_MODE_PARTIAL_IO ?
			TI_SCI_FLAG_REQ_GENERIC_NORESPONSE :
			TI_SCI_FLAG_REQ_ACK_ON_PROCESSED;
	struct ti_sci_msg_req_prepare_sleep req;
	struct ti_sci_msg_hdr resp;
	struct device *dev;
	int ret = 0;

	if (IS_ERR(handle))
		return PTR_ERR(handle);
	if (!handle)
		return -EINVAL;

	dev = handle_to_ti_sci_info(handle)->dev;

	req.mode = mode;
	req.ctx_lo = ctx_lo;
	req.ctx_hi = ctx_hi;
	req.debug_flags = debug_flags;

	ret = ti_sci_do_xfer(handle, TI_SCI_MSG_PREPARE_SLEEP,
			     msg_flags,
			     &req, sizeof(req),
			     &resp, sizeof(resp));
	if (ret) {
		dev_err(dev, "Failed to prepare sleep\n");
		return ret;
	}

	return 0;
}

/**
 * ti_sci_msg_cmd_query_fw_caps() - Get the FW/SoC capabilities
 * @handle:		Pointer to TI SCI handle
 * @fw_caps:		Each bit in fw_caps indicating one FW/SOC capability
 *
 * Check if the firmware supports any optional low power modes.
 * Old revisions of TIFS (< 08.04) will NACK the request which results in
 * -ENODEV being returned.
 *
 * Return: 0 if all went well, else returns appropriate error value.
 */
static int ti_sci_msg_cmd_query_fw_caps(const struct ti_sci_handle *handle,
					u64 *fw_caps)
{
	struct ti_sci_msg_hdr req;
	struct ti_sci_msg_resp_query_fw_caps resp;
	struct device *dev;
	int ret = 0;

	if (IS_ERR(handle))
		return PTR_ERR(handle);
	if (!handle)
		return -EINVAL;

	dev = handle_to_ti_sci_info(handle)->dev;

	ret = ti_sci_do_xfer(handle, TI_SCI_MSG_QUERY_FW_CAPS,
			     TI_SCI_FLAG_REQ_ACK_ON_PROCESSED,
			     &req, sizeof(req),
			     &resp, sizeof(resp));
	if (ret) {
		dev_err(dev, "Failed to get capabilities\n");
		return ret;
	}

	if (fw_caps)
		*fw_caps = resp.fw_caps;

	return 0;
}

/**
 * ti_sci_cmd_set_io_isolation() - Enable IO isolation in LPM
 * @handle:		Pointer to TI SCI handle
 * @state:		The desired state of the IO isolation
 *
 * Return: 0 if all went well, else returns appropriate error value.
 */
static int ti_sci_cmd_set_io_isolation(const struct ti_sci_handle *handle,
				       u8 state)
{
	struct ti_sci_msg_req_set_io_isolation req;
	struct ti_sci_msg_hdr resp;
	struct device *dev;
	int ret = 0;

	if (IS_ERR(handle))
		return PTR_ERR(handle);
	if (!handle)
		return -EINVAL;

	dev = handle_to_ti_sci_info(handle)->dev;

	req.state = state;

	ret = ti_sci_do_xfer(handle, TI_SCI_MSG_SET_IO_ISOLATION,
			     TI_SCI_FLAG_REQ_ACK_ON_PROCESSED,
			     &req, sizeof(req),
			     &resp, sizeof(resp));
	if (ret) {
		dev_err(dev, "Failed to set IO isolation\n");
		return ret;
	}

	return 0;
}

/**
 * ti_sci_cmd_lpm_abort() - Abort entry to LPM by clearing selection of LPM to enter
 * @dev:	Device pointer corresponding to the SCI entity
 *
 * Return: 0 if all went well, else returns appropriate error value.
 */
static int ti_sci_cmd_lpm_abort(struct device *dev)
{
	struct ti_sci_info *info = dev_get_drvdata(dev);
	struct ti_sci_msg_hdr req;
	struct ti_sci_msg_hdr resp;
	int ret = 0;

	ret = ti_sci_do_xfer(&info->handle, TI_SCI_MSG_LPM_ABORT,
			     TI_SCI_FLAG_REQ_ACK_ON_PROCESSED,
			     &req, sizeof(req),
			     &resp, sizeof(resp));
	if (ret)
		return ret;

	return 0;
}

static int ti_sci_cmd_core_reboot(const struct ti_sci_handle *handle)
{
	struct ti_sci_msg_req_reboot req;
	struct ti_sci_msg_hdr resp;
	int ret = 0;

	ret = ti_sci_do_xfer(handle, TI_SCI_MSG_SYS_RESET,
			     TI_SCI_FLAG_REQ_ACK_ON_PROCESSED,
			     &req, sizeof(req),
			     &resp, sizeof(resp));
	if (ret)
		return ret;

	return 0;
}

/**
 * ti_sci_cmd_proc_request() - Command to request a physical processor control
 * @handle:	Pointer to TI SCI handle
 * @proc_id:	Processor ID this request is for
 *
 * Return: 0 if all went well, else returns appropriate error value.
 */
static int ti_sci_cmd_proc_request(const struct ti_sci_handle *handle,
				   u8 proc_id)
{
	struct ti_sci_msg_req_proc_request req;
	struct ti_sci_msg_hdr resp;
	int ret = 0;

	req.processor_id = proc_id;

	ret = ti_sci_do_xfer(handle, TI_SCI_MSG_PROC_REQUEST,
			     TI_SCI_FLAG_REQ_ACK_ON_PROCESSED,
			     &req, sizeof(req),
			     &resp, sizeof(resp));
	if (ret)
		return ret;

	return 0;
}

/**
 * ti_sci_cmd_proc_release() - Command to release a physical processor control
 * @handle:	Pointer to TI SCI handle
 * @proc_id:	Processor ID this request is for
 *
 * Return: 0 if all went well, else returns appropriate error value.
 */
static int ti_sci_cmd_proc_release(const struct ti_sci_handle *handle,
				   u8 proc_id)
{
	struct ti_sci_msg_req_proc_release req;
	struct ti_sci_msg_hdr resp;
	int ret = 0;

	req.processor_id = proc_id;

	ret = ti_sci_do_xfer(handle, TI_SCI_MSG_PROC_RELEASE,
			     TI_SCI_FLAG_REQ_ACK_ON_PROCESSED,
			     &req, sizeof(req),
			     &resp, sizeof(resp));
	if (ret)
		return ret;

	return 0;
}

/**
 * ti_sci_cmd_proc_handover() - Command to handover a physical processor
 *				control to a host in the processor's access
 *				control list.
 * @handle:	Pointer to TI SCI handle
 * @proc_id:	Processor ID this request is for
 * @host_id:	Host ID to get the control of the processor
 *
 * Return: 0 if all went well, else returns appropriate error value.
 */
static int ti_sci_cmd_proc_handover(const struct ti_sci_handle *handle,
				    u8 proc_id, u8 host_id)
{
	struct ti_sci_msg_req_proc_handover req;
	struct ti_sci_msg_hdr resp;
	int ret = 0;

	req.processor_id = proc_id;
	req.host_id = host_id;

	ret = ti_sci_do_xfer(handle, TI_SCI_MSG_PROC_HANDOVER,
			     TI_SCI_FLAG_REQ_ACK_ON_PROCESSED,
			     &req, sizeof(req),
			     &resp, sizeof(resp));
	if (ret)
		return ret;

	return 0;
}

/**
 * ti_sci_cmd_proc_set_config() - Command to set the processor boot
 *				    configuration flags
 * @handle:		Pointer to TI SCI handle
 * @proc_id:		Processor ID this request is for
 * @bootvector:		Processor Boot vector (start address)
 * @config_flags_set:	Configuration flags to be set
 * @config_flags_clear:	Configuration flags to be cleared.
 *
 * Return: 0 if all went well, else returns appropriate error value.
 */
static int ti_sci_cmd_proc_set_config(const struct ti_sci_handle *handle,
				      u8 proc_id, u64 bootvector,
				      u32 config_flags_set,
				      u32 config_flags_clear)
{
	struct ti_sci_msg_req_set_config req;
	struct ti_sci_msg_hdr resp;
	int ret = 0;

	req.processor_id = proc_id;
	req.bootvector_low = bootvector & TI_SCI_ADDR_LOW_MASK;
	req.bootvector_high = (bootvector & TI_SCI_ADDR_HIGH_MASK) >>
				TI_SCI_ADDR_HIGH_SHIFT;
	req.config_flags_set = config_flags_set;
	req.config_flags_clear = config_flags_clear;

	ret = ti_sci_do_xfer(handle, TI_SCI_MSG_SET_CONFIG,
			     TI_SCI_FLAG_REQ_ACK_ON_PROCESSED,
			     &req, sizeof(req),
			     &resp, sizeof(resp));
	if (ret)
		return ret;

	return 0;
}

/**
 * ti_sci_cmd_proc_set_control() - Command to set the processor boot
 *				     control flags
 * @handle:			Pointer to TI SCI handle
 * @proc_id:			Processor ID this request is for
 * @control_flags_set:		Control flags to be set
 * @control_flags_clear:	Control flags to be cleared
 *
 * Return: 0 if all went well, else returns appropriate error value.
 */
static int ti_sci_cmd_proc_set_control(const struct ti_sci_handle *handle,
				       u8 proc_id, u32 control_flags_set,
				       u32 control_flags_clear)
{
	struct ti_sci_msg_req_set_ctrl req;
	struct ti_sci_msg_hdr resp;
	int ret = 0;

	req.processor_id = proc_id;
	req.control_flags_set = control_flags_set;
	req.control_flags_clear = control_flags_clear;

	ret = ti_sci_do_xfer(handle, TI_SCI_MSG_SET_CTRL,
			     TI_SCI_FLAG_REQ_ACK_ON_PROCESSED,
			     &req, sizeof(req),
			     &resp, sizeof(resp));
	if (ret)
		return ret;

	return 0;
}

/**
 * ti_sci_cmd_proc_get_status() - Command to get the processor boot status
 * @handle:	Pointer to TI SCI handle
 * @proc_id:	Processor ID this request is for
 * @bv:		Processor Boot vector (start address)
 * @cfg_flags:	Processor specific configuration flags
 * @ctrl_flags:	Processor specific control flags
 * @sts_flags:	Processor specific status flags
 *
 * Return: 0 if all went well, else returns appropriate error value.
 */
static int ti_sci_cmd_proc_get_status(const struct ti_sci_handle *handle,
				      u8 proc_id, u64 *bv, u32 *cfg_flags,
				      u32 *ctrl_flags, u32 *sts_flags)
{
	struct ti_sci_msg_resp_get_status resp;
	struct ti_sci_msg_req_get_status req;
	int ret = 0;

	req.processor_id = proc_id;

	ret = ti_sci_do_xfer(handle, TI_SCI_MSG_GET_STATUS,
			     TI_SCI_FLAG_REQ_ACK_ON_PROCESSED,
			     &req, sizeof(req),
			     &resp, sizeof(resp));
	if (ret)
		return ret;

	*bv = (resp.bootvector_low & TI_SCI_ADDR_LOW_MASK) |
	      (((u64)resp.bootvector_high << TI_SCI_ADDR_HIGH_SHIFT) &
	       TI_SCI_ADDR_HIGH_MASK);
	*cfg_flags = resp.config_flags;
	*ctrl_flags = resp.control_flags;
	*sts_flags = resp.status_flags;

	return 0;
}

/*
 * ti_sci_setup_ops() - Setup the operations structures
 * @info:	pointer to TISCI pointer
 */
static void ti_sci_setup_ops(struct ti_sci_info *info)
{
	struct ti_sci_ops *ops = &info->handle.ops;
	struct ti_sci_dev_ops *dops = &ops->dev_ops;
	struct ti_sci_clk_ops *cops = &ops->clk_ops;
	struct ti_sci_pm_ops *pmops = &ops->pm_ops;
	struct ti_sci_rm_core_ops *rm_core_ops = &ops->rm_core_ops;
	struct ti_sci_rm_irq_ops *iops = &ops->rm_irq_ops;
	struct ti_sci_rm_ringacc_ops *rops = &ops->rm_ring_ops;
	struct ti_sci_rm_psil_ops *psilops = &ops->rm_psil_ops;
	struct ti_sci_rm_udmap_ops *udmap_ops = &ops->rm_udmap_ops;
	struct ti_sci_proc_ops *pops = &ops->proc_ops;

	ti_sci_setup_device_ops(dops);
	ti_sci_setup_clock_ops(cops);

	if (info->fw_caps & MSG_FLAG_CAPS_LPM_DM_MANAGED) {
		pr_debug("detected DM managed LPM in fw_caps\n");
		ti_sci_setup_pm_ops(pmops);
	}

	ti_sci_setup_rm_core_ops(rm_core_ops);
	ti_sci_setup_rm_irq_ops(iops);
	ti_sci_setup_rm_ringacc_ops(rops);
	ti_sci_setup_rm_psil_ops(psilops);
	ti_sci_setup_rm_udmap_ops(udmap_ops);

	pops->request = ti_sci_cmd_proc_request;
	pops->release = ti_sci_cmd_proc_release;
	pops->handover = ti_sci_cmd_proc_handover;
	pops->set_config = ti_sci_cmd_proc_set_config;
	pops->set_control = ti_sci_cmd_proc_set_control;
	pops->get_status = ti_sci_cmd_proc_get_status;
}

/**
 * ti_sci_get_handle() - Get the TI SCI handle for a device
 * @dev:	Pointer to device for which we want SCI handle
 *
 * NOTE: The function does not track individual clients of the framework
 * and is expected to be maintained by caller of TI SCI protocol library.
 * ti_sci_put_handle must be balanced with successful ti_sci_get_handle
 * Return: pointer to handle if successful, else:
 * -EPROBE_DEFER if the instance is not ready
 * -ENODEV if the required node handler is missing
 * -EINVAL if invalid conditions are encountered.
 */
const struct ti_sci_handle *ti_sci_get_handle(struct device *dev)
{
	struct device_node *ti_sci_np;
	struct ti_sci_handle *handle = NULL;
	struct ti_sci_info *info;

	if (!dev) {
		pr_err("I need a device pointer\n");
		return ERR_PTR(-EINVAL);
	}
	ti_sci_np = of_get_parent(dev->of_node);
	if (!ti_sci_np) {
		dev_err(dev, "No OF information\n");
		return ERR_PTR(-EINVAL);
	}

	mutex_lock(&ti_sci_list_mutex);
	list_for_each_entry(info, &ti_sci_list, node) {
		if (ti_sci_np == info->dev->of_node) {
			handle = &info->handle;
			info->users++;
			break;
		}
	}
	mutex_unlock(&ti_sci_list_mutex);
	of_node_put(ti_sci_np);

	if (!handle)
		return ERR_PTR(-EPROBE_DEFER);

	return handle;
}
EXPORT_SYMBOL_GPL(ti_sci_get_handle);

/**
 * ti_sci_put_handle() - Release the handle acquired by ti_sci_get_handle
 * @handle:	Handle acquired by ti_sci_get_handle
 *
 * NOTE: The function does not track individual clients of the framework
 * and is expected to be maintained by caller of TI SCI protocol library.
 * ti_sci_put_handle must be balanced with successful ti_sci_get_handle
 *
 * Return: 0 is successfully released
 * if an error pointer was passed, it returns the error value back,
 * if null was passed, it returns -EINVAL;
 */
int ti_sci_put_handle(const struct ti_sci_handle *handle)
{
	struct ti_sci_info *info;

	if (IS_ERR(handle))
		return PTR_ERR(handle);
	if (!handle)
		return -EINVAL;

	info = handle_to_ti_sci_info(handle);
	mutex_lock(&ti_sci_list_mutex);
	if (!WARN_ON(!info->users))
		info->users--;
	mutex_unlock(&ti_sci_list_mutex);

	return 0;
}
EXPORT_SYMBOL_GPL(ti_sci_put_handle);

static void devm_ti_sci_release(struct device *dev, void *res)
{
	const struct ti_sci_handle **ptr = res;
	const struct ti_sci_handle *handle = *ptr;
	int ret;

	ret = ti_sci_put_handle(handle);
	if (ret)
		dev_err(dev, "failed to put handle %d\n", ret);
}

/**
 * devm_ti_sci_get_handle() - Managed get handle
 * @dev:	device for which we want SCI handle for.
 *
 * NOTE: This releases the handle once the device resources are
 * no longer needed. MUST NOT BE released with ti_sci_put_handle.
 * The function does not track individual clients of the framework
 * and is expected to be maintained by caller of TI SCI protocol library.
 *
 * Return: 0 if all went fine, else corresponding error.
 */
const struct ti_sci_handle *devm_ti_sci_get_handle(struct device *dev)
{
	const struct ti_sci_handle **ptr;
	const struct ti_sci_handle *handle;

	ptr = devres_alloc(devm_ti_sci_release, sizeof(*ptr), GFP_KERNEL);
	if (!ptr)
		return ERR_PTR(-ENOMEM);
	handle = ti_sci_get_handle(dev);

	if (!IS_ERR(handle)) {
		*ptr = handle;
		devres_add(dev, ptr);
	} else {
		devres_free(ptr);
	}

	return handle;
}
EXPORT_SYMBOL_GPL(devm_ti_sci_get_handle);

/**
 * ti_sci_get_by_phandle() - Get the TI SCI handle using DT phandle
 * @np:		device node
 * @property:	property name containing phandle on TISCI node
 *
 * NOTE: The function does not track individual clients of the framework
 * and is expected to be maintained by caller of TI SCI protocol library.
 * ti_sci_put_handle must be balanced with successful ti_sci_get_by_phandle
 * Return: pointer to handle if successful, else:
 * -EPROBE_DEFER if the instance is not ready
 * -ENODEV if the required node handler is missing
 * -EINVAL if invalid conditions are encountered.
 */
const struct ti_sci_handle *ti_sci_get_by_phandle(struct device_node *np,
						  const char *property)
{
	struct ti_sci_handle *handle = NULL;
	struct device_node *ti_sci_np;
	struct ti_sci_info *info;

	if (!np) {
		pr_err("I need a device pointer\n");
		return ERR_PTR(-EINVAL);
	}

	ti_sci_np = of_parse_phandle(np, property, 0);
	if (!ti_sci_np)
		return ERR_PTR(-ENODEV);

	mutex_lock(&ti_sci_list_mutex);
	list_for_each_entry(info, &ti_sci_list, node) {
		if (ti_sci_np == info->dev->of_node) {
			handle = &info->handle;
			info->users++;
			break;
		}
	}
	mutex_unlock(&ti_sci_list_mutex);
	of_node_put(ti_sci_np);

	if (!handle)
		return ERR_PTR(-EPROBE_DEFER);

	return handle;
}
EXPORT_SYMBOL_GPL(ti_sci_get_by_phandle);

/**
 * devm_ti_sci_get_by_phandle() - Managed get handle using phandle
 * @dev:	Device pointer requesting TISCI handle
 * @property:	property name containing phandle on TISCI node
 *
 * NOTE: This releases the handle once the device resources are
 * no longer needed. MUST NOT BE released with ti_sci_put_handle.
 * The function does not track individual clients of the framework
 * and is expected to be maintained by caller of TI SCI protocol library.
 *
 * Return: 0 if all went fine, else corresponding error.
 */
const struct ti_sci_handle *devm_ti_sci_get_by_phandle(struct device *dev,
						       const char *property)
{
	const struct ti_sci_handle *handle;
	const struct ti_sci_handle **ptr;

	ptr = devres_alloc(devm_ti_sci_release, sizeof(*ptr), GFP_KERNEL);
	if (!ptr)
		return ERR_PTR(-ENOMEM);
	handle = ti_sci_get_by_phandle(dev_of_node(dev), property);

	if (!IS_ERR(handle)) {
		*ptr = handle;
		devres_add(dev, ptr);
	} else {
		devres_free(ptr);
	}

	return handle;
}
EXPORT_SYMBOL_GPL(devm_ti_sci_get_by_phandle);

/*
 * Iterate all device nodes that have a wakeup-source property and check if one
 * of the possible phandles points to a Partial-IO system state. If it
 * does resolve the device node to an actual device and check if wakeup is
 * enabled.
 */
static bool ti_sci_partial_io_wakeup_enabled(struct ti_sci_info *info)
{
	struct device_node *wakeup_node = NULL;

	for_each_node_with_property(wakeup_node, "wakeup-source") {
		struct of_phandle_iterator it;
		int err;

		of_for_each_phandle(&it, err, wakeup_node, "wakeup-source", NULL, 0) {
			struct platform_device *pdev;
			bool may_wakeup;

			/*
			 * Continue if idle-state-name is not off-wake. Return
			 * value is the index of the string which should be 0 if
			 * off-wake is present.
			 */
			if (of_property_match_string(it.node, "idle-state-name", "off-wake"))
				continue;

			pdev = of_find_device_by_node(wakeup_node);
			if (!pdev)
				continue;

			may_wakeup = device_may_wakeup(&pdev->dev);
			put_device(&pdev->dev);

			if (may_wakeup) {
				dev_dbg(info->dev, "%pOF identified as wakeup source for Partial-IO\n",
					wakeup_node);
				of_node_put(it.node);
				of_node_put(wakeup_node);
				return true;
			}
		}
	}

	return false;
}

static int ti_sci_sys_off_handler(struct sys_off_data *data)
{
	struct ti_sci_info *info = data->cb_data;
	const struct ti_sci_handle *handle = &info->handle;
	bool enter_partial_io = ti_sci_partial_io_wakeup_enabled(info);
	int ret;

	if (!enter_partial_io)
		return NOTIFY_DONE;

	dev_info(info->dev, "Entering Partial-IO because a powered wakeup-enabled device was found.\n");

	ret = ti_sci_cmd_prepare_sleep(handle, TISCI_MSG_VALUE_SLEEP_MODE_PARTIAL_IO, 0, 0, 0);
	if (ret) {
		dev_err(info->dev,
			"Failed to enter Partial-IO %pe, trying to do an emergency restart\n",
			ERR_PTR(ret));
		emergency_restart();
	}

	mdelay(5000);
	emergency_restart();

	return NOTIFY_DONE;
}

static int tisci_reboot_handler(struct sys_off_data *data)
{
	struct ti_sci_info *info = data->cb_data;
	const struct ti_sci_handle *handle = &info->handle;

	ti_sci_cmd_core_reboot(handle);

	/* call fail OR pass, we should not be here in the first place */
	return NOTIFY_BAD;
}

static int ti_sci_prepare_system_suspend(struct ti_sci_info *info)
{
	/*
	 * Map and validate the target Linux suspend state to TISCI LPM.
	 * Default is to let Device Manager select the low power mode.
	 */
	switch (pm_suspend_target_state) {
	case PM_SUSPEND_MEM:
		if (info->fw_caps & MSG_FLAG_CAPS_LPM_DM_MANAGED) {
			/*
			 * For the DM_MANAGED mode the context is reserved for
			 * internal use and can be 0
			 */
			return ti_sci_cmd_prepare_sleep(&info->handle,
							TISCI_MSG_VALUE_SLEEP_MODE_DM_MANAGED,
							0, 0, 0);
		} else if (info->fw_caps & MSG_FLAG_CAPS_LPM_BOARDCFG_MANAGED) {
			/* Nothing to do in the BOARDCFG_MANAGED mode */
			return 0;
		} else {
			/* DM Managed and BoardCfg Managed are not supported by the firmware. */
			dev_err(info->dev, "Suspend to memory is not supported by the firmware\n");
			return -EOPNOTSUPP;
		}
		break;
	default:
		/*
		 * Do not fail if we don't have action to take for a
		 * specific suspend mode.
		 */
		return 0;
	}
}

static int ti_sci_suspend(struct device *dev)
{
	struct ti_sci_info *info = dev_get_drvdata(dev);
	struct device *cpu_dev, *cpu_dev_max = NULL;
	struct ti_sci_pm_ops *pmops = &info->handle.ops.pm_ops;
	s32 val, cpu_lat = 0;
	u16 cpu_lat_ms;
	int i, ret;

	if (info->fw_caps & MSG_FLAG_CAPS_LPM_DM_MANAGED) {
		for_each_possible_cpu(i) {
			cpu_dev = get_cpu_device(i);
			val = dev_pm_qos_read_value(cpu_dev, DEV_PM_QOS_RESUME_LATENCY);
			if (val != PM_QOS_RESUME_LATENCY_NO_CONSTRAINT) {
				cpu_lat = max(cpu_lat, val);
				cpu_dev_max = cpu_dev;
			}
		}
		if (cpu_dev_max) {
			/*
			 * PM QoS latency unit is usecs, device manager uses msecs.
			 * Convert to msecs and round down for device manager.
			 */
			cpu_lat_ms = cpu_lat / USEC_PER_MSEC;
			dev_dbg(cpu_dev_max, "%s: sending max CPU latency=%u ms\n", __func__,
				cpu_lat_ms);
			ret = pmops->set_latency_constraint(&info->handle,
							    cpu_lat_ms,
							    TISCI_MSG_CONSTRAINT_SET);
			if (ret)
				return ret;
		}
	}

	ret = ti_sci_prepare_system_suspend(info);
	if (ret)
		return ret;

	return 0;
}

static int ti_sci_suspend_noirq(struct device *dev)
{
	struct ti_sci_info *info = dev_get_drvdata(dev);
	int ret = 0;

	if (info->fw_caps & MSG_FLAG_CAPS_IO_ISOLATION) {
		ret = ti_sci_cmd_set_io_isolation(&info->handle, TISCI_MSG_VALUE_IO_ENABLE);
		if (ret)
			return ret;
	}

	return 0;
}

static int ti_sci_resume_noirq(struct device *dev)
{
	struct ti_sci_info *info = dev_get_drvdata(dev);
	struct ti_sci_msg_req_manage_irq *irq_desc;
	struct ti_sci_irq *irq;
	struct ti_sci_pm_ops *pmops = &info->handle.ops.pm_ops;
	struct hlist_node *tmp_node;
	int ret = 0, err = 0, i;
	u32 source;
	u64 time;
	u8 pin;
	u8 mode;

	if (info->fw_caps & MSG_FLAG_CAPS_IO_ISOLATION) {
		ret = ti_sci_cmd_set_io_isolation(&info->handle, TISCI_MSG_VALUE_IO_DISABLE);
		if (ret)
			return ret;
	}

	switch (pm_suspend_target_state) {
	case PM_SUSPEND_MEM:
		if (info->fw_caps & MSG_FLAG_CAPS_LPM_IRQ_CONTEXT_LOST) {
			hash_for_each_safe(info->irqs, i, tmp_node, irq, node) {
				irq_desc = &irq->desc;
				ret = ti_sci_manage_irq(&info->handle,
							irq_desc->valid_params,
							irq_desc->src_id,
							irq_desc->src_index,
							irq_desc->dst_id,
							irq_desc->dst_host_irq,
							irq_desc->ia_id,
							irq_desc->vint,
							irq_desc->global_event,
							irq_desc->vint_status_bit,
							irq_desc->secondary_host,
							TI_SCI_MSG_SET_IRQ);
				if (ret) {
					dev_err(dev, "failed to restore IRQ with valid_params = 0x%x from src = %d, index = %d, to dst = %d, irq = %d, via ia_id = %d, vint = %d, global event = %d,status_bit = %d\n",
						irq_desc->valid_params,
						irq_desc->src_id,
						irq_desc->src_index,
						irq_desc->dst_id,
						irq_desc->dst_host_irq,
						irq_desc->ia_id,
						irq_desc->vint,
						irq_desc->global_event,
						irq_desc->vint_status_bit);
					err = ret;
				}
			}
		}

		if (info->fw_caps & MSG_FLAG_CAPS_LPM_CLK_CONTEXT_LOST)
			clk_restore_context();
		break;
	default:
		break;
	}

	if (info->fw_caps & MSG_FLAG_CAPS_LPM_DM_MANAGED) {
		ret = pmops->lpm_wake_reason(&info->handle, &source, &time, &pin,
					     &mode);
		/* Do not fail to resume on error as the wake reason is not critical */
		if (!ret)
			dev_info(dev, "ti_sci: wakeup source:0x%x, pin:0x%x, mode:0x%x\n",
				 source, pin, mode);
	}

	return err;
}

static void ti_sci_pm_complete(struct device *dev)
{
	struct ti_sci_info *info = dev_get_drvdata(dev);

	if (info->fw_caps & MSG_FLAG_CAPS_LPM_ABORT) {
		if (ti_sci_cmd_lpm_abort(dev))
			dev_err(dev, "LPM clear selection failed.\n");
	}
}

static const struct dev_pm_ops ti_sci_pm_ops = {
	.suspend = pm_sleep_ptr(ti_sci_suspend),
	.suspend_noirq = pm_sleep_ptr(ti_sci_suspend_noirq),
	.resume_noirq = pm_sleep_ptr(ti_sci_resume_noirq),
	.complete = pm_sleep_ptr(ti_sci_pm_complete),
};

/* Description for K2G */
static const struct ti_sci_desc ti_sci_pmmc_k2g_desc = {
	.default_host_id = 2,
	/* Conservative duration */
	.max_rx_timeout_ms = 1000,
	/* Limited by MBOX_TX_QUEUE_LEN. K2G can handle up to 128 messages! */
	.max_msgs = 20,
	.max_msg_size = 64,
};

/* Description for AM654 */
static const struct ti_sci_desc ti_sci_pmmc_am654_desc = {
	.default_host_id = 12,
	/* Conservative duration */
	.max_rx_timeout_ms = 10000,
	/* Limited by MBOX_TX_QUEUE_LEN. K2G can handle up to 128 messages! */
	.max_msgs = 20,
	.max_msg_size = 60,
};

static const struct of_device_id ti_sci_of_match[] = {
	{.compatible = "ti,k2g-sci", .data = &ti_sci_pmmc_k2g_desc},
	{.compatible = "ti,am654-sci", .data = &ti_sci_pmmc_am654_desc},
	{ /* Sentinel */ },
};
MODULE_DEVICE_TABLE(of, ti_sci_of_match);

static int ti_sci_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	const struct ti_sci_desc *desc;
	struct ti_sci_info *info = NULL;
	struct ti_sci_xfers_info *minfo;
	struct mbox_client *cl;
	int ret = -EINVAL;
	int i;
	u32 h_id;

	desc = device_get_match_data(dev);

	info = devm_kzalloc(dev, sizeof(*info), GFP_KERNEL);
	if (!info)
		return -ENOMEM;

	info->dev = dev;
	info->desc = desc;
	ret = of_property_read_u32(dev->of_node, "ti,host-id", &h_id);
	/* if the property is not present in DT, use a default from desc */
	if (ret < 0) {
		info->host_id = info->desc->default_host_id;
	} else {
		if (!h_id) {
			dev_warn(dev, "Host ID 0 is reserved for firmware\n");
			info->host_id = info->desc->default_host_id;
		} else {
			info->host_id = h_id;
		}
	}

	INIT_LIST_HEAD(&info->node);
	minfo = &info->minfo;

	/*
	 * Pre-allocate messages
	 * NEVER allocate more than what we can indicate in hdr.seq
	 * if we have data description bug, force a fix..
	 */
	if (WARN_ON(desc->max_msgs >=
		    1 << 8 * sizeof(((struct ti_sci_msg_hdr *)0)->seq)))
		return -EINVAL;

	minfo->xfer_block = devm_kcalloc(dev,
					 desc->max_msgs,
					 sizeof(*minfo->xfer_block),
					 GFP_KERNEL);
	if (!minfo->xfer_block)
		return -ENOMEM;

	minfo->xfer_alloc_table = devm_bitmap_zalloc(dev,
						     desc->max_msgs,
						     GFP_KERNEL);
	if (!minfo->xfer_alloc_table)
		return -ENOMEM;

	/* Initialize the xfer completions */
	for (i = 0; i < desc->max_msgs; i++)
		init_completion(&minfo->xfer_block[i].done);

	ret = ti_sci_debugfs_create(pdev, info);
	if (ret)
		dev_warn(dev, "Failed to create debug file\n");

	platform_set_drvdata(pdev, info);

	cl = &info->cl;
	cl->dev = dev;
	cl->tx_block = false;
	cl->rx_callback = ti_sci_rx_callback;
	cl->knows_txdone = true;

	spin_lock_init(&minfo->xfer_lock);
	sema_init(&minfo->sem_xfer_count, desc->max_msgs);

	info->chan_rx = mbox_request_channel_byname(cl, "rx");
	if (IS_ERR(info->chan_rx)) {
		ret = PTR_ERR(info->chan_rx);
		goto out;
	}

	info->chan_tx = mbox_request_channel_byname(cl, "tx");
	if (IS_ERR(info->chan_tx)) {
		ret = PTR_ERR(info->chan_tx);
		goto out;
	}
	ret = ti_sci_cmd_get_revision(info);
	if (ret) {
		dev_err(dev, "Unable to communicate with TISCI(%d)\n", ret);
		goto out;
	}

	ti_sci_msg_cmd_query_fw_caps(&info->handle, &info->fw_caps);
	dev_dbg(dev, "Detected firmware capabilities: %s%s%s%s%s%s%s%s\n",
		info->fw_caps & MSG_FLAG_CAPS_GENERIC ? "Generic" : "",
		info->fw_caps & MSG_FLAG_CAPS_LPM_PARTIAL_IO ? " Partial-IO" : "",
		info->fw_caps & MSG_FLAG_CAPS_LPM_DM_MANAGED ? " DM-Managed" : "",
		info->fw_caps & MSG_FLAG_CAPS_LPM_ABORT ? " LPM-Abort" : "",
		info->fw_caps & MSG_FLAG_CAPS_IO_ISOLATION ? " IO-Isolation" : "",
		info->fw_caps & MSG_FLAG_CAPS_LPM_BOARDCFG_MANAGED ? " BoardConfig-Managed" : "",
		info->fw_caps & MSG_FLAG_CAPS_LPM_IRQ_CONTEXT_LOST ? " IRQ-Context-Lost" : "",
		info->fw_caps & MSG_FLAG_CAPS_LPM_CLK_CONTEXT_LOST ? " Clk-Context-Lost" : ""
	);

	ti_sci_setup_ops(info);

	ret = devm_register_restart_handler(dev, tisci_reboot_handler, info);
	if (ret) {
		dev_err(dev, "reboot registration fail(%d)\n", ret);
		goto out;
	}

	if (info->fw_caps & MSG_FLAG_CAPS_LPM_PARTIAL_IO) {
		ret = devm_register_sys_off_handler(dev,
						    SYS_OFF_MODE_POWER_OFF,
						    SYS_OFF_PRIO_FIRMWARE,
						    ti_sci_sys_off_handler,
						    info);
		if (ret) {
			dev_err(dev, "Failed to register sys_off_handler %pe\n",
				ERR_PTR(ret));
			goto out;
		}
	}

	dev_info(dev, "ABI: %d.%d (firmware rev 0x%04x '%s')\n",
		 info->handle.version.abi_major, info->handle.version.abi_minor,
		 info->handle.version.firmware_revision,
		 info->handle.version.firmware_description);

	mutex_lock(&ti_sci_list_mutex);
	list_add_tail(&info->node, &ti_sci_list);
	mutex_unlock(&ti_sci_list_mutex);

	ret = devm_mutex_init(dev, &info->irq_lock);
	if (ret)
		goto out;

	if (info->fw_caps & MSG_FLAG_CAPS_LPM_IRQ_CONTEXT_LOST)
		hash_init(info->irqs);

	ret = of_platform_populate(dev->of_node, NULL, NULL, dev);
	if (ret) {
		dev_err(dev, "platform_populate failed %pe\n", ERR_PTR(ret));
		of_platform_depopulate(dev);
		mutex_lock(&ti_sci_list_mutex);
		list_del(&info->node);
		mutex_unlock(&ti_sci_list_mutex);
		goto out;
	}
	return 0;

out:
	if (!IS_ERR(info->chan_tx))
		mbox_free_channel(info->chan_tx);
	if (!IS_ERR(info->chan_rx))
		mbox_free_channel(info->chan_rx);
	debugfs_remove(info->d);
	return ret;
}

static struct platform_driver ti_sci_driver = {
	.probe = ti_sci_probe,
	.driver = {
		   .name = "ti-sci",
		   .of_match_table = ti_sci_of_match,
		   .suppress_bind_attrs = true,
		   .pm = &ti_sci_pm_ops,
	},
};
module_platform_driver(ti_sci_driver);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("TI System Control Interface(SCI) driver");
MODULE_AUTHOR("Nishanth Menon");
MODULE_ALIAS("platform:ti-sci");
