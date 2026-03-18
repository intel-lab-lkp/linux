// SPDX-License-Identifier: MIT
/*
 * Copyright © 2026 Intel Corporation
 */

#include <linux/bitfield.h>
#include <linux/cleanup.h>
#include <linux/container_of.h>
#include <linux/errno.h>
#include <linux/minmax.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/types.h>

#include "regs/xe_sysctrl_regs.h"
#include "xe_device.h"
#include "xe_device_types.h"
#include "xe_mmio.h"
#include "xe_pm.h"
#include "xe_printk.h"
#include "xe_sysctrl.h"
#include "xe_sysctrl_mailbox.h"
#include "xe_sysctrl_mailbox_types.h"
#include "xe_sysctrl_types.h"

#define MKHI_HDR_GROUP_ID_MASK		GENMASK(7, 0)
#define MKHI_HDR_COMMAND_MASK		GENMASK(14, 8)
#define MKHI_HDR_IS_RESPONSE		BIT(15)
#define MKHI_HDR_RESERVED_MASK		GENMASK(23, 16)
#define MKHI_HDR_RESULT_MASK		GENMASK(31, 24)

#define XE_SYSCTRL_MKHI_HDR_GROUP_ID(hdr) \
	FIELD_GET(MKHI_HDR_GROUP_ID_MASK, le32_to_cpu((hdr)->data))

#define XE_SYSCTRL_MKHI_HDR_COMMAND(hdr) \
	FIELD_GET(MKHI_HDR_COMMAND_MASK, le32_to_cpu((hdr)->data))

#define XE_SYSCTRL_MKHI_HDR_IS_RESPONSE(hdr) \
	FIELD_GET(MKHI_HDR_IS_RESPONSE, le32_to_cpu((hdr)->data))

#define XE_SYSCTRL_MKHI_HDR_RESULT(hdr) \
	FIELD_GET(MKHI_HDR_RESULT_MASK, le32_to_cpu((hdr)->data))

static struct xe_device *sc_to_xe(struct xe_sysctrl *sc)
{
	return container_of(sc, struct xe_device, sc);
}

static bool xe_sysctrl_mailbox_wait_bit_clear(struct xe_sysctrl *sc, u32 bit_mask,
					      unsigned int timeout_ms)
{
	int ret;

	ret = xe_mmio_wait32_not(sc->mmio, SYSCTRL_MB_CTRL, bit_mask, bit_mask,
				 timeout_ms * 1000, NULL, false);

	return ret == 0;
}

static bool xe_sysctrl_mailbox_wait_bit_set(struct xe_sysctrl *sc, u32 bit_mask,
					    unsigned int timeout_ms)
{
	int ret;

	ret = xe_mmio_wait32(sc->mmio, SYSCTRL_MB_CTRL, bit_mask, bit_mask,
			     timeout_ms * 1000, NULL, false);

	return ret == 0;
}

static int xe_sysctrl_mailbox_write_frame(struct xe_sysctrl *sc, const void *frame,
					  size_t len)
{
	static const struct xe_reg regs[] = {
		SYSCTRL_MB_DATA0, SYSCTRL_MB_DATA1, SYSCTRL_MB_DATA2, SYSCTRL_MB_DATA3
	};
	u32 val[SYSCTRL_MB_FRAME_SIZE / sizeof(u32)] = {0};
	u32 dw = DIV_ROUND_UP(len, sizeof(u32));
	u32 i;

	memcpy(val, frame, len);

	for (i = 0; i < dw; i++)
		xe_mmio_write32(sc->mmio, regs[i], val[i]);

	return 0;
}

static int xe_sysctrl_mailbox_read_frame(struct xe_sysctrl *sc, void *frame,
					 size_t len)
{
	static const struct xe_reg regs[] = {
		SYSCTRL_MB_DATA0, SYSCTRL_MB_DATA1, SYSCTRL_MB_DATA2, SYSCTRL_MB_DATA3
	};
	u32 val[SYSCTRL_MB_FRAME_SIZE / sizeof(u32)] = {0};
	u32 dw = DIV_ROUND_UP(len, sizeof(u32));
	u32 i;

	for (i = 0; i < dw; i++)
		val[i] = xe_mmio_read32(sc->mmio, regs[i]);

	memcpy(frame, val, len);

	return 0;
}

static void xe_sysctrl_mailbox_clear_response(struct xe_sysctrl *sc)
{
	xe_mmio_rmw32(sc->mmio, SYSCTRL_MB_CTRL, SYSCTRL_MB_CTRL_RUN_BUSY_OUT, 0);
}

static int xe_sysctrl_mailbox_prepare_command(struct xe_device *xe,
					      u8 group_id, u8 command,
					      const void *data_in, size_t data_in_len,
					      u8 **mbox_cmd, size_t *cmd_size)
{
	struct xe_sysctrl_mailbox_mkhi_msg_hdr *mkhi_hdr;
	size_t size;
	u8 *buffer;

	if (data_in_len > SYSCTRL_MB_MAX_MESSAGE_SIZE - sizeof(*mkhi_hdr)) {
		xe_err(xe, "sysctrl: Input data too large: %zu bytes\n", data_in_len);
		return -EINVAL;
	}

	size = sizeof(*mkhi_hdr) + data_in_len;

	buffer = kmalloc(size, GFP_KERNEL);
	if (!buffer)
		return -ENOMEM;

	mkhi_hdr = (struct xe_sysctrl_mailbox_mkhi_msg_hdr *)buffer;
	mkhi_hdr->data = cpu_to_le32(FIELD_PREP(MKHI_HDR_GROUP_ID_MASK, group_id) |
				     FIELD_PREP(MKHI_HDR_COMMAND_MASK, command & 0x7F) |
				     FIELD_PREP(MKHI_HDR_IS_RESPONSE, 0) |
				     FIELD_PREP(MKHI_HDR_RESERVED_MASK, 0) |
				     FIELD_PREP(MKHI_HDR_RESULT_MASK, 0));

	if (data_in && data_in_len)
		memcpy(buffer + sizeof(*mkhi_hdr), data_in, data_in_len);

	*mbox_cmd = buffer;
	*cmd_size = size;

	return 0;
}

static int xe_sysctrl_mailbox_send_frames(struct xe_sysctrl *sc,
					  const u8 *mbox_cmd,
					  size_t cmd_size, unsigned int timeout_ms)
{
	struct xe_device *xe = sc_to_xe(sc);
	u32 ctrl_reg, total_frames, frame;
	size_t bytes_sent, frame_size;

	total_frames = DIV_ROUND_UP(cmd_size, SYSCTRL_MB_FRAME_SIZE);

	if (!xe_sysctrl_mailbox_wait_bit_clear(sc, SYSCTRL_MB_CTRL_RUN_BUSY, timeout_ms)) {
		xe_err(xe, "sysctrl: Mailbox busy\n");
		return -EBUSY;
	}

	sc->phase_bit ^= 1;
	bytes_sent = 0;

	for (frame = 0; frame < total_frames; frame++) {
		frame_size = min_t(size_t, cmd_size - bytes_sent, SYSCTRL_MB_FRAME_SIZE);

		if (xe_sysctrl_mailbox_write_frame(sc, mbox_cmd + bytes_sent, frame_size)) {
			xe_err(xe, "sysctrl: Failed to write frame %u\n", frame);
			sc->phase_bit = 0;
			return -EIO;
		}

		ctrl_reg = SYSCTRL_MB_CTRL_RUN_BUSY |
			   FIELD_PREP(MKHI_FRAME_CURRENT_MASK, frame) |
			   FIELD_PREP(MKHI_FRAME_TOTAL_MASK, total_frames - 1) |
			   SYSCTRL_MB_CTRL_MKHI_CMD |
			   (sc->phase_bit ? MKHI_FRAME_PHASE : 0);

		xe_mmio_write32(sc->mmio, SYSCTRL_MB_CTRL, ctrl_reg);

		if (!xe_sysctrl_mailbox_wait_bit_clear(sc, SYSCTRL_MB_CTRL_RUN_BUSY, timeout_ms)) {
			xe_err(xe, "sysctrl: Frame %u acknowledgment timeout\n", frame);
			sc->phase_bit = 0;
			return -ETIMEDOUT;
		}

		bytes_sent += frame_size;
	}

	return 0;
}

static int xe_sysctrl_mailbox_process_frame(struct xe_sysctrl *sc, void *out,
					    size_t frame_size, unsigned int timeout_ms,
					    bool *done)
{
	u32 curr_frame, total_frames, ctrl_reg;
	struct xe_device *xe = sc_to_xe(sc);
	int ret;

	if (!xe_sysctrl_mailbox_wait_bit_set(sc, SYSCTRL_MB_CTRL_RUN_BUSY_OUT, timeout_ms)) {
		xe_err(xe, "sysctrl: Response frame timeout\n");
		return -ETIMEDOUT;
	}

	ctrl_reg = xe_mmio_read32(sc->mmio, SYSCTRL_MB_CTRL);
	total_frames = FIELD_GET(MKHI_FRAME_TOTAL_MASK, ctrl_reg);
	curr_frame = FIELD_GET(MKHI_FRAME_CURRENT_MASK, ctrl_reg);

	ret = xe_sysctrl_mailbox_read_frame(sc, out, frame_size);
	if (ret)
		return ret;

	xe_sysctrl_mailbox_clear_response(sc);

	if (curr_frame == total_frames)
		*done = true;

	return 0;
}

static int xe_sysctrl_mailbox_receive_frames(struct xe_sysctrl *sc,
					     const struct xe_sysctrl_mailbox_mkhi_msg_hdr *req,
					     void *data_out, size_t data_out_len,
					     size_t *rdata_len, unsigned int timeout_ms)
{
	struct xe_sysctrl_mailbox_mkhi_msg_hdr *mkhi_hdr;
	struct xe_device *xe = sc_to_xe(sc);
	size_t frame_size, remain;
	bool done = false;
	u8 *out;
	int ret = 0;

	remain = sizeof(*mkhi_hdr) + data_out_len;
	u8 *buffer __free(kfree) = kzalloc(remain, GFP_KERNEL);
	if (!buffer)
		return -ENOMEM;

	out = buffer;
	while (!done && remain) {
		frame_size = min_t(size_t, remain, SYSCTRL_MB_FRAME_SIZE);

		ret = xe_sysctrl_mailbox_process_frame(sc, out, frame_size, timeout_ms,
						       &done);
		if (ret)
			return ret;

		remain -= frame_size;
		out += frame_size;
	}

	mkhi_hdr = (struct xe_sysctrl_mailbox_mkhi_msg_hdr *)buffer;

	if (!XE_SYSCTRL_MKHI_HDR_IS_RESPONSE(mkhi_hdr) ||
	    XE_SYSCTRL_MKHI_HDR_GROUP_ID(mkhi_hdr) != XE_SYSCTRL_MKHI_HDR_GROUP_ID(req) ||
	    XE_SYSCTRL_MKHI_HDR_COMMAND(mkhi_hdr) != XE_SYSCTRL_MKHI_HDR_COMMAND(req)) {
		xe_err(xe, "sysctrl: Response header mismatch\n");
		return -EPROTO;
	}

	if (XE_SYSCTRL_MKHI_HDR_RESULT(mkhi_hdr) != 0) {
		xe_err(xe, "sysctrl: Firmware error: 0x%02lx\n",
		       XE_SYSCTRL_MKHI_HDR_RESULT(mkhi_hdr));
		return -EIO;
	}

	memcpy(data_out, mkhi_hdr + 1, data_out_len);
	*rdata_len = out - buffer - sizeof(*mkhi_hdr);

	return ret;
}

static int xe_sysctrl_mailbox_send_command(struct xe_sysctrl *sc,
					   const u8 *mbox_cmd, size_t cmd_size,
					   void *data_out, size_t data_out_len,
					   size_t *rdata_len, unsigned int timeout_ms)
{
	const struct xe_sysctrl_mailbox_mkhi_msg_hdr *mkhi_hdr;
	size_t received;
	int ret;

	ret = xe_sysctrl_mailbox_send_frames(sc, mbox_cmd, cmd_size, timeout_ms);
	if (ret)
		return ret;

	if (!data_out || !rdata_len)
		return 0;

	mkhi_hdr = (const struct xe_sysctrl_mailbox_mkhi_msg_hdr *)mbox_cmd;

	ret = xe_sysctrl_mailbox_receive_frames(sc, mkhi_hdr, data_out, data_out_len,
						&received, timeout_ms);
	if (ret)
		return ret;

	*rdata_len = received;

	return 0;
}

/**
 * xe_sysctrl_mailbox_init - Initialize System Controller mailbox interface
 * @sc: System controller structure
 *
 * Initialize system controller mailbox interface for communication.
 */
void xe_sysctrl_mailbox_init(struct xe_sysctrl *sc)
{
	u32 ctrl_reg;

	ctrl_reg = xe_mmio_read32(sc->mmio, SYSCTRL_MB_CTRL);
	sc->phase_bit = (ctrl_reg & MKHI_FRAME_PHASE) ? 1 : 0;
}

/**
 * xe_sysctrl_send_command - Send command to System Controller via mailbox
 * @xe: XE device instance
 * @cmd: Pointer to xe_sysctrl_mailbox_command structure
 * @rdata_len: Pointer to store actual response data size (can be NULL)
 *
 * Send a command to the System Controller using MKHI protocol. Handles
 * command preparation, fragmentation, transmission, and response reception.
 *
 * Return: 0 on success, negative error code on failure
 */
int xe_sysctrl_send_command(struct xe_device *xe,
			    struct xe_sysctrl_mailbox_command *cmd,
			    size_t *rdata_len)
{
	struct xe_sysctrl *sc;
	u8 group_id, command_code;
	u8 *mbox_cmd = NULL;
	size_t cmd_size = 0;
	int ret = 0;

	if (!xe) {
		pr_err("sysctrl: Invalid device handle\n");
		return -EINVAL;
	}

	if (!xe->info.has_sysctrl)
		return -ENODEV;

	sc = &xe->sc;

	if (!cmd) {
		xe_err(xe, "sysctrl: Invalid command buffer\n");
		return -EINVAL;
	}

	group_id = XE_SYSCTRL_APP_HDR_GROUP_ID(&cmd->header);
	command_code = XE_SYSCTRL_APP_HDR_COMMAND(&cmd->header);

	if (!cmd->data_in && cmd->data_in_len) {
		xe_err(xe, "sysctrl: Invalid input parameters\n");
		return -EINVAL;
	}

	if (!cmd->data_out && cmd->data_out_len) {
		xe_err(xe, "sysctrl: Invalid output parameters\n");
		return -EINVAL;
	}

	might_sleep();

	ret = xe_sysctrl_mailbox_prepare_command(xe, group_id, command_code,
						 cmd->data_in, cmd->data_in_len,
						 &mbox_cmd, &cmd_size);
	if (ret) {
		xe_err(xe, "sysctrl: Failed to prepare command: %d\n", ret);
		return ret;
	}

	guard(xe_pm_runtime)(xe);

	guard(mutex)(&sc->cmd_lock);

	ret = xe_sysctrl_mailbox_send_command(sc, mbox_cmd, cmd_size,
					      cmd->data_out, cmd->data_out_len, rdata_len,
					      SYSCTRL_MB_DEFAULT_TIMEOUT_MS);
	if (ret)
		xe_err(xe, "sysctrl: Mailbox command failed: %d\n", ret);

	kfree(mbox_cmd);

	return ret;
}
