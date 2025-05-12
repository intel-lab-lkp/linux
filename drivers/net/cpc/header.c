// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2025, Silicon Laboratories, Inc.
 */

#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/string.h>

#include "header.h"

#define CPC_CONTROL_TYPE_MASK 0xC0
#define CPC_CONTROL_ACK_MASK BIT(2)

/**
 * cpc_header_get_type() - Get the frame type.
 * @hdr_raw: Raw header.
 * @type: Reference to a frame type.
 *
 * Return: True if the type has been successfully decoded, otherwise false.
 *         On success, the output parameter type is assigned.
 */
bool cpc_header_get_type(const u8 hdr_raw[CPC_HEADER_SIZE], enum cpc_frame_type *type)
{
	const struct cpc_header *hdr = (struct cpc_header *)hdr_raw;

	switch (FIELD_GET(CPC_CONTROL_TYPE_MASK, hdr->ctrl)) {
	case CPC_FRAME_TYPE_DATA:
		*type = CPC_FRAME_TYPE_DATA;
		break;
	case CPC_FRAME_TYPE_SYN:
		*type = CPC_FRAME_TYPE_SYN;
		break;
	case CPC_FRAME_TYPE_RST:
		*type = CPC_FRAME_TYPE_RST;
		break;
	default:
		return false;
	}

	return true;
}

/**
 * cpc_header_get_ep_id() - Get the endpoint id.
 * @hdr_raw: Raw header.
 *
 * Return: Endpoint id.
 */
u8 cpc_header_get_ep_id(const u8 hdr_raw[CPC_HEADER_SIZE])
{
	const struct cpc_header *hdr = (struct cpc_header *)hdr_raw;

	return hdr->ep_id;
}

/**
 * cpc_header_get_recv_wnd() - Get the receive window.
 * @hdr_raw: Raw header.
 *
 * Return: Receive window.
 */
u8 cpc_header_get_recv_wnd(const u8 hdr_raw[CPC_HEADER_SIZE])
{
	const struct cpc_header *hdr = (struct cpc_header *)hdr_raw;

	return hdr->recv_wnd;
}

/**
 * cpc_header_get_seq() - Get the sequence number.
 * @hdr_raw: Raw header.
 *
 * Return: Sequence number.
 */
u8 cpc_header_get_seq(const u8 hdr_raw[CPC_HEADER_SIZE])
{
	const struct cpc_header *hdr = (struct cpc_header *)hdr_raw;

	return hdr->seq;
}

/**
 * cpc_header_get_ack() - Get the acknowledge number.
 * @hdr_raw: Raw header.
 *
 * Return: Acknowledge number.
 */
u8 cpc_header_get_ack(const u8 hdr_raw[CPC_HEADER_SIZE])
{
	const struct cpc_header *hdr = (struct cpc_header *)hdr_raw;

	return hdr->ack;
}

/**
 * cpc_header_get_req_ack() - Get the request acknowledge frame flag.
 * @hdr_raw: Raw header.
 *
 * Return: Request acknowledge frame flag.
 */
bool cpc_header_get_req_ack(const u8 hdr_raw[CPC_HEADER_SIZE])
{
	const struct cpc_header *hdr = (struct cpc_header *)hdr_raw;

	return FIELD_GET(CPC_CONTROL_ACK_MASK, hdr->ctrl);
}

/**
 * cpc_header_get_mtu() - Get the maximum transmission unit.
 * @hdr_raw: Raw header.
 *
 * Return: Maximum transmission unit.
 *
 * Must only be used over a SYN frame.
 */
u16 cpc_header_get_mtu(const u8 hdr_raw[CPC_HEADER_SIZE])
{
	const struct cpc_header *hdr = (struct cpc_header *)hdr_raw;

	return le16_to_cpu(hdr->syn.mtu);
}

/**
 * cpc_header_get_payload_len() - Get the payload length.
 * @hdr_raw: Raw header.
 *
 * Return: Payload length.
 *
 * Must only be used over a DATA frame.
 */
u16 cpc_header_get_payload_len(const u8 hdr_raw[CPC_HEADER_SIZE])
{
	const struct cpc_header *hdr = (struct cpc_header *)hdr_raw;

	return le16_to_cpu(hdr->dat.payload_len);
}

/**
 * cpc_header_get_ctrl() - Encode parameters into a control byte.
 * @type: Frame type.
 * @req_ack: Frame flag indicating a request to be acknowledged.
 *
 * Return: Encoded control byte.
 */
u8 cpc_header_get_ctrl(enum cpc_frame_type type, bool req_ack)
{
	return FIELD_PREP(CPC_CONTROL_TYPE_MASK, type) |
	       FIELD_PREP(CPC_CONTROL_ACK_MASK, req_ack);
}

/**
 * cpc_header_get_frames_acked_count() - Get frames to be acknowledged.
 * @seq: Current sequence number of the endpoint.
 * @ack: Acknowledge number of the received frame.
 * @ack_pending_count: Amount of frames pending on an acknowledge.
 *
 * Return: Frames to be acknowledged.
 */
u8 cpc_header_get_frames_acked_count(u8 seq, u8 ack, u8 ack_pending_count)
{
	u8 frames_acked_count;
	u8 ack_range_min;
	u8 ack_range_max;

	ack_range_min = seq + 1;
	ack_range_max = seq + ack_pending_count;

	if (!cpc_header_number_in_range(ack_range_min, ack_range_max, ack))
		return 0;

	/* Find number of frames acknowledged with ACK number. */
	if (ack > seq) {
		frames_acked_count = ack - seq;
	} else {
		frames_acked_count = 256 - seq;
		frames_acked_count += ack;
	}

	return frames_acked_count;
}

/**
 * cpc_header_is_syn_ack_valid() - Check if the provided SYN-ACK valid or not.
 * @seq: Current sequence number of the endpoint.
 * @ack: Acknowledge number of the received SYN.
 *
 * Return: True if valid, otherwise false.
 */
bool cpc_header_is_syn_ack_valid(u8 seq, u8 ack)
{
	return !!cpc_header_get_frames_acked_count(seq, ack, 1);
}

/**
 * cpc_header_number_in_window() - Test if a number is within a window.
 * @start: Start of the window.
 * @end: Window size.
 * @n: Number to be tested.
 *
 * Given the start of the window and its size, test if the number is
 * in the range [start; start + wnd).
 *
 * @return True if start <= n <= start + wnd - 1 (modulo 256), otherwise false.
 */
bool cpc_header_number_in_window(u8 start, u8 wnd, u8 n)
{
	u8 end;

	if (wnd == 0)
		return false;

	end = start + wnd - 1;

	return cpc_header_number_in_range(start, end, n);
}

/**
 * cpc_header_number_in_range() - Test if a number is between start and end (included).
 * @start: Lowest limit.
 * @end: Highest limit inclusively.
 * @n: Number to be tested.
 *
 * @return True if start <= n <= end (modulo 256), otherwise false.
 */
bool cpc_header_number_in_range(u8 start, u8 end, u8 n)
{
	if (end >= start) {
		if (n < start || n > end)
			return false;
	} else {
		if (n > end && n < start)
			return false;
	}

	return true;
}
