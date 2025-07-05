/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2025, Silicon Laboratories, Inc.
 */

#ifndef __CPC_HEADER_H
#define __CPC_HEADER_H

#include <linux/compiler_attributes.h>
#include <linux/types.h>

#define CPC_HEADER_MAX_RX_WINDOW	U8_MAX
#define CPC_HEADER_SIZE			8

/**
 * enum cpc_frame_type - Describes all possible frame types that can
 * be received or sent.
 * @CPC_FRAME_TYPE_DATA: Used to send and control application DATA frames.
 * @CPC_FRAME_TYPE_SYN: Used to initiate an endpoint connection.
 * @CPC_FRAME_TYPE_RST: Used to reset the endpoint connection and indicate
 *                      that the endpoint is unavailable.
 */
enum cpc_frame_type {
	CPC_FRAME_TYPE_DATA,
	CPC_FRAME_TYPE_SYN,
	CPC_FRAME_TYPE_RST,
};

/**
 * struct cpc_header - Representation of the CPC header.
 * @ep_id: Address of the endpoint the frame is destined to.
 * @ctrl: Indicates the frame type [7..6] and frame flags [5..0].
 *        Currently only the request acknowledge flag is supported.
 *        This flag indicates if the frame should be acknowledged by
 *        the remote on reception.
 * @recv_wnd: Indicates to the remote how many reception buffers are
 *            available so it can determine how many frames it can send.
 * @seq: Identifies the frame with a number.
 * @ack: Indicate the sequence number of the next expected frame from
 *       the remote. When paired with a fast re-transmit flag, it indicates
 *       the sequence number of the frame in error that should be
 *       re-transmitted.
 * @syn.mtu: On a SYN frame, this represents the maximum transmission unit.
 * @dat.payload_len: On a DATA frame, this indicates the payload length.
 */
struct cpc_header {
	u16 ep_id;
	u8 ctrl;
	u8 recv_wnd;
	u8 seq;
	u8 ack;
	union {
		u8 extension[2];
		struct __packed {
			__le16 mtu;
		} syn;
		struct __packed {
			__le16 payload_len;
		} dat;
		struct __packed {
			u8 reserved[2];
		} rst;
	};
} __packed;

bool cpc_header_get_type(const struct cpc_header *hdr, enum cpc_frame_type *type);
u8 cpc_header_get_ep_id(const struct cpc_header *hdr);
u8 cpc_header_get_recv_wnd(const struct cpc_header *hdr);
u8 cpc_header_get_seq(const struct cpc_header *hdr);
u8 cpc_header_get_ack(const struct cpc_header *hdr);
bool cpc_header_get_req_ack(const struct cpc_header *hdr);
u16 cpc_header_get_mtu(const struct cpc_header *hdr);
u16 cpc_header_get_payload_len(const struct cpc_header *hdr);
u8 cpc_header_get_ctrl(enum cpc_frame_type type, bool req_ack);

u8 cpc_header_get_frames_acked_count(u8 seq, u8 ack);
bool cpc_header_is_syn_ack_valid(u8 seq, u8 ack);
bool cpc_header_number_in_window(u8 start, u8 wnd, u8 n);
bool cpc_header_number_in_range(u8 start, u8 end, u8 n);

#endif
