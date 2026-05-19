// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2023, 2024 NXP Semiconductor, Inc.
 *
 */
#include <linux/dev_printk.h>
#include <linux/module.h>
#include <soc/cadence/cdns-mhdp-helper.h>

/* Mailbox helper functions */
static int mhdp_mailbox_read(struct cdns_mhdp_base *base)
{
	int ret, empty;

	lockdep_assert_held(&base->mailbox_mutex);

	ret = readx_poll_timeout(readl, base->regs + CDNS_MAILBOX_EMPTY,
				 empty, !empty, MAILBOX_RETRY_US,
				 MAILBOX_TIMEOUT_US);
	if (ret < 0)
		return ret;

	return readl(base->regs + CDNS_MAILBOX_RX_DATA) & 0xff;
}

static int mhdp_mailbox_write(struct cdns_mhdp_base *base, u8 val)
{
	int ret, full;

	lockdep_assert_held(&base->mailbox_mutex);

	ret = readx_poll_timeout(readl, base->regs + CDNS_MAILBOX_FULL,
				 full, !full, MAILBOX_RETRY_US,
				 MAILBOX_TIMEOUT_US);
	if (ret < 0)
		return ret;

	writel(val, base->regs + CDNS_MAILBOX_TX_DATA);

	return 0;
}

static int mhdp_mailbox_read_secure(struct cdns_mhdp_base *base)
{
	int ret, empty;

	lockdep_assert_held(&base->mailbox_mutex);

	ret = readx_poll_timeout(readl, base->sapb_regs + CDNS_MAILBOX_EMPTY,
				 empty, !empty, MAILBOX_RETRY_US,
				 MAILBOX_TIMEOUT_US);
	if (ret < 0)
		return ret;

	return readl(base->sapb_regs + CDNS_MAILBOX_RX_DATA) & 0xff;
}

static int mhdp_mailbox_write_secure(struct cdns_mhdp_base *base, u8 val)
{
	int ret, full;

	lockdep_assert_held(&base->mailbox_mutex);

	ret = readx_poll_timeout(readl, base->sapb_regs + CDNS_MAILBOX_FULL,
				 full, !full, MAILBOX_RETRY_US,
				 MAILBOX_TIMEOUT_US);
	if (ret < 0)
		return ret;

	writel(val, base->sapb_regs + CDNS_MAILBOX_TX_DATA);

	return 0;
}

static int mhdp_mailbox_recv_header(struct cdns_mhdp_base *base,
				    u8 module_id, u8 opcode,
				    u16 req_size, bool secure)
{
	u32 mbox_size, i;
	u8 header[4];
	int ret;

	/* read the header of the message */
	for (i = 0; i < sizeof(header); i++) {
		if (secure)
			ret = mhdp_mailbox_read_secure(base);
		else
			ret = mhdp_mailbox_read(base);
		if (ret < 0)
			return ret;

		header[i] = ret;
	}

	mbox_size = get_unaligned_be16(header + 2);

	/*
	 * If the message in mailbox is not what we want, we need to
	 * clear the mailbox by reading its contents.
	 * Response data length for HDCP TX HDCP_TRAN_IS_REC_ID_VALID depend on
	 * case.
	 */
	if (opcode != header[0] ||
	    module_id != header[1] ||
	   (opcode != HDCP_TRAN_IS_REC_ID_VALID && req_size != mbox_size)) {
		for (i = 0; i < mbox_size; i++) {
			if (secure)
				ret = mhdp_mailbox_read_secure(base);
			else
				ret = mhdp_mailbox_read(base);
			if (ret < 0)
				break;
		}

		return -EINVAL;
	}

	return 0;
}

static int mhdp_mailbox_recv_data(struct cdns_mhdp_base *base,
				  u8 *buff, u16 buff_size, bool secure)
{
	u32 i;
	int ret;

	for (i = 0; i < buff_size; i++) {
		if (secure)
			ret = mhdp_mailbox_read_secure(base);
		else
			ret = mhdp_mailbox_read(base);
		if (ret < 0)
			return ret;

		buff[i] = ret;
	}

	return 0;
}

static int mhdp_mailbox_send(struct cdns_mhdp_base *base, u8 module_id,
			     u8 opcode, u16 size, u8 *message, bool secure)
{
	u8 header[4];
	int ret, i;

	header[0] = opcode;
	header[1] = module_id;
	put_unaligned_be16(size, header + 2);

	for (i = 0; i < sizeof(header); i++) {
		if (secure)
			ret = mhdp_mailbox_write_secure(base, header[i]);
		else
			ret = mhdp_mailbox_write(base, header[i]);
		if (ret)
			return ret;
	}

	for (i = 0; i < size; i++) {
		if (secure)
			ret = mhdp_mailbox_write_secure(base, message[i]);
		else
			ret = mhdp_mailbox_write(base, message[i]);
		if (ret)
			return ret;
	}

	return 0;
}

/**
 * cdns_mhdp_mailbox_send - Sends a message via the MHDP mailbox.
 *
 * This function sends a message via the MHDP mailbox.
 *
 * @base: Pointer to the CDNS MHDP base structure.
 * @module_id: ID of the module to send the message to.
 * @opcode: Operation code of the message.
 * @size: Size of the message data.
 * @message: Pointer to the message data.
 *
 * Returns: 0 on success, negative error code on failure.
 */
int cdns_mhdp_mailbox_send(struct cdns_mhdp_base *base, u8 module_id,
			   u8 opcode, u16 size, u8 *message)
{
	guard(mutex)(&base->mailbox_mutex);

	return mhdp_mailbox_send(base, module_id, opcode, size, message, false);
}
EXPORT_SYMBOL_GPL(cdns_mhdp_mailbox_send);

/**
 * cdns_mhdp_mailbox_send_recv - Sends a message and receives a response.
 *
 * This function sends a message via the mailbox and then receives a response.
 *
 * @base: Pointer to the CDNS MHDP base structure.
 * @module_id: ID of the module to send the message to.
 * @opcode: Operation code of the message.
 * @msg_size: Size of the message data.
 * @msg: Pointer to the message data.
 * @resp_size: Size of the response buffer.
 * @resp: Pointer to the response buffer.
 *
 * Returns: 0 on success, negative error code on failure.
 */
int cdns_mhdp_mailbox_send_recv(struct cdns_mhdp_base *base,
				u8 module_id, u8 opcode,
				u16 msg_size, u8 *msg,
				u16 resp_size, u8 *resp)
{
	int ret;

	guard(mutex)(&base->mailbox_mutex);

	ret = mhdp_mailbox_send(base, module_id, opcode, msg_size, msg, false);
	if (ret) {
		dev_err(base->dev, "ModuleID=%d, CMD=%d send failed: %d\n",
			module_id, opcode, ret);
		return ret;
	}

	ret = mhdp_mailbox_recv_header(base, module_id, opcode, resp_size, false);
	if (ret) {
		dev_err(base->dev, "ModuleID=%d, CMD=%d recv header failed: %d\n",
			module_id, opcode, ret);
		return ret;
	}

	ret = mhdp_mailbox_recv_data(base, resp, resp_size, false);
	if (ret)
		dev_err(base->dev, "ModuleID=%d, CMD=%d recv data failed: %d\n",
			module_id, opcode, ret);
	return ret;
}
EXPORT_SYMBOL_GPL(cdns_mhdp_mailbox_send_recv);

/**
 * cdns_mhdp_mailbox_send_recv_multi - Sends a message and receives multiple
 * responses.
 *
 * This function sends a message to a specified module via the MHDP mailbox and
 * then receives multiple responses from the module.
 *
 * @base: Pointer to the CDNS MHDP base structure.
 * @module_id: ID of the module to send the message to.
 * @opcode: Operation code of the message.
 * @msg_size: Size of the message data.
 * @msg: Pointer to the message data.
 * @opcode_resp: Operation code of the response.
 * @resp1_size: Size of the first response buffer.
 * @resp1: Pointer to the first response buffer.
 * @resp2_size: Size of the second response buffer.
 * @resp2: Pointer to the second response buffer.
 *
 * Returns: 0 on success, negative error code on failure.
 */
int cdns_mhdp_mailbox_send_recv_multi(struct cdns_mhdp_base *base,
				      u8 module_id, u8 opcode,
				      u16 msg_size, u8 *msg,
				      u8 opcode_resp,
				      u16 resp1_size, u8 *resp1,
				      u16 resp2_size, u8 *resp2)
{
	int ret;

	guard(mutex)(&base->mailbox_mutex);

	ret = mhdp_mailbox_send(base, module_id, opcode, msg_size, msg, false);
	if (ret) {
		dev_err(base->dev, "ModuleID=%d, CMD=%d send failed: %d\n",
			module_id, opcode, ret);
		return ret;
	}

	ret = mhdp_mailbox_recv_header(base, module_id, opcode_resp,
				       resp1_size + resp2_size, false);
	if (ret) {
		dev_err(base->dev, "ModuleID=%d, Resp_CMD=%d recv header failed: %d\n",
			module_id, opcode_resp, ret);
		return ret;
	}

	ret = mhdp_mailbox_recv_data(base, resp1, resp1_size, false);
	if (ret) {
		dev_err(base->dev, "ModuleID=%d, Resp_CMD=%d recv data1 failed: %d\n",
			module_id, opcode_resp, ret);
		return ret;
	}

	ret = mhdp_mailbox_recv_data(base, resp2, resp2_size, false);
	if (ret)
		dev_err(base->dev, "ModuleID=%d, Resp_CMD=%d recv data2 failed: %d\n",
			module_id, opcode_resp, ret);
	return ret;
}
EXPORT_SYMBOL_GPL(cdns_mhdp_mailbox_send_recv_multi);

/**
 * cdns_mhdp_secure_mailbox_send - Sends a secure message via the mailbox.
 *
 * This function sends a secure message to a specified module via the MHDP
 * mailbox.
 *
 * @base: Pointer to the CDNS MHDP base structure.
 * @module_id: ID of the module to send the message to.
 * @opcode: Operation code of the message.
 * @size: Size of the message data.
 * @message: Pointer to the message data.
 *
 * Returns: 0 on success, negative error code on failure.
 */
int cdns_mhdp_secure_mailbox_send(struct cdns_mhdp_base *base, u8 module_id,
				  u8 opcode, u16 size, u8 *message)
{
	guard(mutex)(&base->mailbox_mutex);

	return mhdp_mailbox_send(base, module_id, opcode, size, message, true);
}
EXPORT_SYMBOL_GPL(cdns_mhdp_secure_mailbox_send);

/**
 * cdns_mhdp_secure_mailbox_send_recv - Sends a secure message and receives a
 * response.
 *
 * This function sends a secure message to a specified module via the mailbox
 * and then receives a response from the module.
 *
 * @base: Pointer to the CDNS MHDP base structure.
 * @module_id: ID of the module to send the message to.
 * @opcode: Operation code of the message.
 * @msg_size: Size of the message data.
 * @msg: Pointer to the message data.
 * @resp_size: Size of the response buffer.
 * @resp: Pointer to the response buffer.
 *
 * Returns: 0 on success, negative error code on failure.
 */
int cdns_mhdp_secure_mailbox_send_recv(struct cdns_mhdp_base *base,
				       u8 module_id, u8 opcode,
				       u16 msg_size, u8 *msg,
				       u16 resp_size, u8 *resp)
{
	int ret;

	guard(mutex)(&base->mailbox_mutex);

	ret = mhdp_mailbox_send(base, module_id, opcode, msg_size, msg, true);
	if (ret) {
		dev_err(base->dev, "ModuleID=%d, CMD=%d send failed: %d\n",
			module_id, opcode, ret);
		return ret;
	}

	ret = mhdp_mailbox_recv_header(base, module_id, opcode, resp_size, true);
	if (ret) {
		dev_err(base->dev, "ModuleID=%d, CMD=%d recv header failed: %d\n",
			module_id, opcode, ret);
		return ret;
	}

	ret = mhdp_mailbox_recv_data(base, resp, resp_size, true);
	if (ret)
		dev_err(base->dev, "ModuleID=%d, CMD=%d recv data failed: %d\n",
			module_id, opcode, ret);
	return ret;
}
EXPORT_SYMBOL_GPL(cdns_mhdp_secure_mailbox_send_recv);

/**
 * cdns_mhdp_secure_mailbox_send_recv_multi - Sends a secure message and
 * receives multiple responses.
 *
 * This function sends a secure message to a specified module and receives
 * multiple responses.
 *
 * @base: Pointer to the CDNS MHDP base structure.
 * @module_id: ID of the module to send the message to.
 * @opcode: Operation code of the message.
 * @msg_size: Size of the message data.
 * @msg: Pointer to the message data.
 * @opcode_resp: Operation code of the response.
 * @resp1_size: Size of the first response buffer.
 * @resp1: Pointer to the first response buffer.
 * @resp2_size: Size of the second response buffer.
 * @resp2: Pointer to the second response buffer.
 *
 * Returns: 0 on success, negative error code on failure.
 */
int cdns_mhdp_secure_mailbox_send_recv_multi(struct cdns_mhdp_base *base,
					     u8 module_id, u8 opcode,
					     u16 msg_size, u8 *msg,
					     u8 opcode_resp,
					     u16 resp1_size, u8 *resp1,
					     u16 resp2_size, u8 *resp2)
{
	int ret;

	guard(mutex)(&base->mailbox_mutex);

	ret = mhdp_mailbox_send(base, module_id, opcode, msg_size, msg, true);
	if (ret) {
		dev_err(base->dev, "ModuleID=%d, CMD=%d send failed: %d\n",
			module_id, opcode, ret);
		return ret;
	}

	ret = mhdp_mailbox_recv_header(base, module_id, opcode_resp,
				       resp1_size + resp2_size, true);
	if (ret) {
		dev_err(base->dev, "ModuleID=%d, Resp_CMD=%d recv header failed: %d\n",
			module_id, opcode_resp, ret);
		return ret;
	}

	ret = mhdp_mailbox_recv_data(base, resp1, resp1_size, true);
	if (ret) {
		dev_err(base->dev, "ModuleID=%d, Resp_CMD=%d recv data1 failed: %d\n",
			module_id, opcode_resp, ret);
		return ret;
	}

	/*
	 * Response data length for HDCP TX HDCP_TRAN_IS_REC_ID_VALID depend on
	 * the number of HDCP receivers in resp1[0].
	 * 1 for regular case, more can be in repeater.
	 */
	if (module_id == MB_MODULE_ID_HDCP_TX &&
	    opcode == HDCP_TRAN_IS_REC_ID_VALID)
		ret = mhdp_mailbox_recv_data(base, resp2, 5 * resp1[0], true);
	else
		ret = mhdp_mailbox_recv_data(base, resp2, resp2_size, true);
	if (ret)
		dev_err(base->dev, "ModuleID=%d, Resp_CMD=%d recv data2 failed: %d\n",
			module_id, opcode_resp, ret);
	return ret;
}
EXPORT_SYMBOL_GPL(cdns_mhdp_secure_mailbox_send_recv_multi);

/**
 * cdns_mhdp_reg_read - Reads a general register value.
 *
 * This function reads the value from a general register
 * using the mailbox.
 *
 * @base: Pointer to the CDNS MHDP base structure.
 * @addr: Address of the register to read.
 * @value: Pointer to store the read value.
 *
 * Returns: 0 on success, negative error code on failure.
 */
int cdns_mhdp_reg_read(struct cdns_mhdp_base *base, u32 addr, u32 *value)
{
	u8 msg[4], resp[8];
	int ret;

	put_unaligned_be32(addr, msg);

	ret = cdns_mhdp_mailbox_send_recv(base, MB_MODULE_ID_GENERAL,
					  GENERAL_REGISTER_READ,
					  sizeof(msg), msg, sizeof(resp), resp);
	if (ret)
		goto out;

	/* Returned address value should be the same as requested */
	if (memcmp(msg, resp, sizeof(msg))) {
		ret = -EINVAL;
		goto out;
	}

	*value = get_unaligned_be32(resp + 4);
out:
	if (ret) {
		dev_err(base->dev, "Failed to read register\n");
		*value = 0;
	}

	return ret;
}
EXPORT_SYMBOL_GPL(cdns_mhdp_reg_read);

/**
 * cdns_mhdp_reg_write - Writes a value to a general register.
 *
 * This function writes a value to a general register using the mailbox.
 *
 * @base: Pointer to the CDNS MHDP base structure.
 * @addr: Address of the register to write to.
 * @val: Value to write to the register.
 *
 * Returns: 0 on success, negative error code on failure.
 */
int cdns_mhdp_reg_write(struct cdns_mhdp_base *base, u32 addr, u32 val)
{
	u8 msg[8];

	put_unaligned_be32(addr, msg);
	put_unaligned_be32(val, msg + 4);

	return cdns_mhdp_mailbox_send(base, MB_MODULE_ID_GENERAL,
				     GENERAL_REGISTER_WRITE,
				     sizeof(msg), msg);
}
EXPORT_SYMBOL_GPL(cdns_mhdp_reg_write);

/* DPTX helper functions */
/**
 * cdns_mhdp_dp_reg_write_bit - Writes a bit field to a DP register.
 *
 * This function writes a specific bit field within a DP register
 * using the MHDP mailbox.
 *
 * @base: Pointer to the CDNS MHDP base structure.
 * @addr: Address of the DP register.
 * @start_bit: Starting bit position within the register.
 * @bits_no: Number of bits to write.
 * @val: Value to write to the bit field.
 *
 * Returns: 0 on success, negative error code on failure.
 */
int cdns_mhdp_dp_reg_write_bit(struct cdns_mhdp_base *base, u16 addr,
			       u8 start_bit, u8 bits_no, u32 val)
{
	u8 field[8];

	put_unaligned_be16(addr, field);
	field[2] = start_bit;
	field[3] = bits_no;
	put_unaligned_be32(val, field + 4);

	return cdns_mhdp_mailbox_send(base, MB_MODULE_ID_DP_TX,
				      DPTX_WRITE_FIELD, sizeof(field), field);
}
EXPORT_SYMBOL_GPL(cdns_mhdp_dp_reg_write_bit);

/**
 * cdns_mhdp_dpcd_read - Reads data from a DPCD register.
 *
 * This function reads data from a specified DPCD register
 * using the MHDP mailbox.
 *
 * @base: Pointer to the CDNS MHDP base structure.
 * @addr: Address of the DPCD register to read.
 * @data: Buffer to store the read data.
 * @len: Length of the data to read.
 *
 * Returns: 0 on success, negative error code on failure.
 */
int cdns_mhdp_dpcd_read(struct cdns_mhdp_base *base,
			u32 addr, u8 *data, u16 len)
{
	u8 msg[5], reg[5];
	int ret;

	put_unaligned_be16(len, msg);
	put_unaligned_be24(addr, msg + 2);

	ret = cdns_mhdp_mailbox_send_recv_multi(base,
						 MB_MODULE_ID_DP_TX,
						 DPTX_READ_DPCD,
						 sizeof(msg), msg,
						 DPTX_READ_DPCD,
						 sizeof(reg), reg,
						 len, data);
	if (ret) {
		dev_err(base->dev, "dpcd read failed: %d\n", ret);
		return ret;
	}

	if (addr != get_unaligned_be24(reg + 2)) {
		dev_err(base->dev,
			"Invalid response: expected address 0x%06x, got 0x%06x\n",
			addr, get_unaligned_be24(reg + 2));
		return -EINVAL;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(cdns_mhdp_dpcd_read);

/**
 * cdns_mhdp_dpcd_write - Writes data to a DPCD register.
 *
 * This function writes data to a specified DPCD register
 * using the MHDP mailbox.
 *
 * @base: Pointer to the CDNS MHDP base structure.
 * @addr: Address of the DPCD register to write to.
 * @value: Value to write to the register.
 *
 * Returns: 0 on success, negative error code on failure.
 */
int cdns_mhdp_dpcd_write(struct cdns_mhdp_base *base, u32 addr, u8 value)
{
	u8 msg[6], reg[5];
	int ret;

	put_unaligned_be16(1, msg);
	put_unaligned_be24(addr, msg + 2);
	msg[5] = value;

	ret = cdns_mhdp_mailbox_send_recv(base, MB_MODULE_ID_DP_TX,
					  DPTX_WRITE_DPCD,
					  sizeof(msg), msg, sizeof(reg), reg);
	if (ret) {
		dev_err(base->dev, "dpcd write failed: %d\n", ret);
		return ret;
	}

	if (addr != get_unaligned_be24(reg + 2)) {
		dev_err(base->dev,
			"Invalid response: expected address 0x%06x, got 0x%06x\n",
			addr, get_unaligned_be24(reg + 2));
		return -EINVAL;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(cdns_mhdp_dpcd_write);

MODULE_DESCRIPTION("Cadence MHDP Helper driver");
MODULE_AUTHOR("Sandor Yu <Sandor.yu@nxp.com>");
MODULE_LICENSE("GPL");
