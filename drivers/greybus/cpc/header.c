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
