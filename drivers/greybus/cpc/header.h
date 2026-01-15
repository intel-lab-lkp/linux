/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2025, Silicon Laboratories, Inc.
 */

#ifndef __CPC_HEADER_H
#define __CPC_HEADER_H

#include <linux/greybus.h>
#include <linux/types.h>

#define CPC_HEADER_MAX_RX_WINDOW U8_MAX

/**
 * struct cpc header - Representation of CPC header.
 * @ctrl_flags: contains the type of frame and other control flags.
 * @recv_wnd: number of buffers that the cport can receive without blocking.
 * @seq: sequence number.
 * @ack: acknowledge number, indicate to the remote the next sequence number
 *	 this peer expects to see.
 *
 * Each peer can confirm reception of frames by setting the acknowledgment number to the next frame
 * it expects to see, i.e. setting the ack number to X effectively acknowledges frames with sequence
 * number up to X-1.
 *
 * CPC is designed around the concept that each cport has its pool of reception buffers. The number
 * of buffers in a pool is advertised to the remote via the @recv_wnd attribute. This acts as
 * software flow-control, and a peer shall not send frames to a remote if the @recv_wnd is zero.
 *
 * The eighth-bit (0x80) of the control byte indicates if the frame targets CPC or Greybus. If the
 * bit is set, the frame should be interpreted as a CPC control frame. For simplicity, control
 * frames have the same encoding as Greybus frames.
 */
struct cpc_header {
	__u8 ctrl_flags;
	__u8 recv_wnd;
	__u8 seq;
	__u8 ack;
} __packed;

#define CPC_HEADER_SIZE (sizeof(struct cpc_header))
#define GREYBUS_HEADER_SIZE (sizeof(struct gb_operation_msg_hdr))

u8 cpc_header_get_seq(const struct cpc_header *hdr);

#endif
