// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2025, Silicon Laboratories, Inc.
 */

#include <linux/bitfield.h>
#include <linux/bits.h>

#include "header.h"

#define CPC_HEADER_CONTROL_IS_CONTROL_MASK BIT(7)
#define CPC_HEADER_CONTROL_REQ_ACK_MASK BIT(6)

/**
 * cpc_header_is_control() - Identify if this is a control frame.
 * @hdr: CPC header.
 *
 * Return: True if this is a control frame, false if this a Greybus frame.
 */
bool cpc_header_is_control(const struct cpc_header *hdr)
{
	return hdr->ctrl_flags & CPC_HEADER_CONTROL_IS_CONTROL_MASK;
}

/**
 * cpc_header_get_recv_wnd() - Get the receive window.
 * @hdr: CPC header.
 *
 * Return: Receive window.
 */
u8 cpc_header_get_recv_wnd(const struct cpc_header *hdr)
{
	return hdr->recv_wnd;
}

/**
 * cpc_header_get_seq() - Get the sequence number.
 * @hdr: CPC header.
 *
 * Return: Sequence number.
 */
u8 cpc_header_get_seq(const struct cpc_header *hdr)
{
	return hdr->seq;
}

/**
 * cpc_header_get_ack() - Get the acknowledge number.
 * @hdr: CPC header.
 *
 * Return: Acknowledge number.
 */
u8 cpc_header_get_ack(const struct cpc_header *hdr)
{
	return hdr->ack;
}

/**
 * cpc_header_get_req_ack() - Get the request acknowledge frame flag.
 * @hdr: CPC header.
 *
 * Return: Request acknowledge frame flag.
 */
bool cpc_header_get_req_ack(const struct cpc_header *hdr)
{
	return FIELD_GET(CPC_HEADER_CONTROL_REQ_ACK_MASK, hdr->ctrl_flags);
}

/**
 * cpc_header_encode_ctrl_flags() - Encode parameters into the control byte.
 * @control: True if CPC control frame, false if Greybus frame.
 * @req_ack: Frame flag indicating a request to be acknowledged.
 *
 * Return: Encoded control byte.
 */
u8 cpc_header_encode_ctrl_flags(bool control, bool req_ack)
{
	return FIELD_PREP(CPC_HEADER_CONTROL_IS_CONTROL_MASK, control) |
	       FIELD_PREP(CPC_HEADER_CONTROL_REQ_ACK_MASK, req_ack);
}

/**
 * cpc_header_get_frames_acked_count() - Get frames to be acknowledged.
 * @seq: Current sequence number of the endpoint.
 * @ack: Acknowledge number of the received frame.
 *
 * Return: Frames to be acknowledged.
 */
u8 cpc_header_get_frames_acked_count(u8 seq, u8 ack)
{
	return ack - seq;
}

/**
 * cpc_header_number_in_window() - Test if a number is within a window.
 * @start: Start of the window.
 * @wnd: Window size.
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
