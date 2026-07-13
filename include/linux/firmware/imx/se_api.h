/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Copyright 2025 NXP
 */

#ifndef __SE_API_H__
#define __SE_API_H__

#include <linux/types.h>

#define SOC_ID_OF_IMX8ULP		0x084d
#define SOC_ID_OF_IMX93			0x9300

/**
 * struct se_msg_hdr - Header of the messages exchanged with the secure enclave.
 * @ver: API version the message conforms to (base or firmware API version).
 * @size: Message size in 32-bit words, including the header.
 * @command: Command identifier.
 * @tag: Message tag identifying it as a command or a response.
 */
struct se_msg_hdr {
	u8 ver;
	u8 size;
	u8 command;
	u8 tag;
}  __packed;

/**
 * struct se_api_msg - A message exchanged with the secure enclave.
 * @header: Message header describing the command and its length.
 * @data: Command or response payload, sized per @header.size.
 */
struct se_api_msg {
	struct se_msg_hdr header;
	u32 data[];
};

/* Opaque handle to a secure-enclave interface instance. */
struct se_if_priv;

/**
 * imx_se_fill_cmd_msg_hdr() - Populate the header of a command message.
 * @priv: Secure-enclave interface instance the command targets.
 * @hdr: Message header to be filled in.
 * @cmd: Command identifier to place in the header.
 * @len: Total message length in bytes, including the header.
 * @is_base_api: %true to tag the message with the base API version, %false to
 *               use the firmware API version.
 *
 * Fill in the tag, version, command and size fields of @hdr so that the message
 * can be sent to the secure enclave.
 *
 * Return: 0 on success.
 */
int imx_se_fill_cmd_msg_hdr(struct se_if_priv *priv, struct se_msg_hdr *hdr,
			    u8 cmd, u32 len, bool is_base_api);

/**
 * imx_se_msg_send_rcv() - Send a command to the secure enclave and wait for the
 *                         response.
 * @priv: Secure-enclave interface instance to communicate with.
 * @tx_msg: Buffer holding the command message to send.
 * @tx_msg_sz: Size of the command message in bytes.
 * @rx_msg: Buffer receiving the response message.
 * @exp_rx_msg_sz: Expected size of the response message in bytes.
 *
 * Blocking send/receive helper for external drivers. The transaction is
 * serialized internally and the misc device context is resolved from @priv.
 *
 * Return: number of bytes received on success, or a negative error code.
 */
int imx_se_msg_send_rcv(struct se_if_priv *priv, void *tx_msg, int tx_msg_sz,
			void *rx_msg, int exp_rx_msg_sz);

/**
 * imx_se_val_rsp_hdr_n_status() - Validate a response header and status code.
 * @priv: Secure-enclave interface instance the response came from.
 * @msg: Response message to validate.
 * @msg_id: Command identifier the response is expected to match.
 * @sz: Expected response size in bytes.
 * @is_base_api: %true if the command used the base API version, %false if it
 *               used the firmware API version.
 *
 * Check that the response tag, command identifier, size and API version match
 * the expectations, and that the enclave reported a successful status.
 *
 * Return: 0 if the response is valid and successful, or a negative error code.
 */
int imx_se_val_rsp_hdr_n_status(struct se_if_priv *priv, struct se_api_msg *msg,
				u8 msg_id, u8 sz, bool is_base_api);

#endif /* __SE_API_H__ */
