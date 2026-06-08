/* SPDX-License-Identifier: GPL-2.0+ WITH Linux-syscall-note */
/*
 * Copyright 2011-2013 Autronica Fire and Security AS
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * Author(s):
 *	2011-2013 Arvid Brodin, arvid.brodin@xdin.com
 */

#ifndef __UAPI_HSR_NETLINK_H
#define __UAPI_HSR_NETLINK_H

/* Generic Netlink HSR family definition
 */

/* attributes for HSR or PRP node */
enum {
	HSR_A_UNSPEC,
	HSR_A_NODE_ADDR,
	HSR_A_IFINDEX,
	HSR_A_IF1_AGE,
	HSR_A_IF2_AGE,
	HSR_A_NODE_ADDR_B,
	HSR_A_IF1_SEQ,
	HSR_A_IF2_SEQ,
	HSR_A_IF1_IFINDEX,
	HSR_A_IF2_IFINDEX,
	HSR_A_ADDR_B_IFINDEX,
	__HSR_A_MAX,
};
#define HSR_A_MAX (__HSR_A_MAX - 1)


/* commands */
enum {
	HSR_C_UNSPEC,
	HSR_C_RING_ERROR,
	HSR_C_NODE_DOWN,
	HSR_C_GET_NODE_STATUS,
	HSR_C_SET_NODE_STATUS,
	HSR_C_GET_NODE_LIST,
	HSR_C_SET_NODE_LIST,
	__HSR_C_MAX,
};
#define HSR_C_MAX (__HSR_C_MAX - 1)

/* HSR/PRP LRE extended statistics attributes.
 * Reported inside LINK_XSTATS_TYPE_HSR (RTM_GETSTATS / ip stats show).
 * Counter definitions follow IEC-62439-3 MIB naming.
 *
 * All counters are __u64.  Unsupported counters are omitted from the
 * netlink reply; user-space must treat an absent attribute as "not available".
 *
 * Per-port suffix: _A = port A (slave 1), _B = port B (slave 2),
 *                  _C = interlink / application interface.
 */
enum {
	/* Sent HSR/PRP tagged frames per port */
	HSR_XSTATS_CNT_TX_A = 1,
	HSR_XSTATS_CNT_TX_B,
	HSR_XSTATS_CNT_TX_C,

	/* Received HSR/PRP tagged frames per port */
	HSR_XSTATS_CNT_RX_A,
	HSR_XSTATS_CNT_RX_B,
	HSR_XSTATS_CNT_RX_C,

	/* Received frames with wrong LAN ID (PRP only) per port */
	HSR_XSTATS_CNT_ERR_WRONG_LAN_A,
	HSR_XSTATS_CNT_ERR_WRONG_LAN_B,
	HSR_XSTATS_CNT_ERR_WRONG_LAN_C,

	/* Received frames with errors per port */
	HSR_XSTATS_CNT_ERRORS_A,
	HSR_XSTATS_CNT_ERRORS_B,
	HSR_XSTATS_CNT_ERRORS_C,

	/* Frames received with no duplicate per port */
	HSR_XSTATS_CNT_UNIQUE_A,
	HSR_XSTATS_CNT_UNIQUE_B,
	HSR_XSTATS_CNT_UNIQUE_C,

	/* Frames received with exactly one duplicate per port */
	HSR_XSTATS_CNT_DUPLICATE_A,
	HSR_XSTATS_CNT_DUPLICATE_B,
	HSR_XSTATS_CNT_DUPLICATE_C,

	/* Frames received with more than one duplicate per port */
	HSR_XSTATS_CNT_MULTI_A,
	HSR_XSTATS_CNT_MULTI_B,
	HSR_XSTATS_CNT_MULTI_C,

	/* Frames received matching this node's own address (HSR only) */
	HSR_XSTATS_CNT_OWN_RX_A,
	HSR_XSTATS_CNT_OWN_RX_B,

	HSR_XSTATS_PAD,
	__HSR_XSTATS_MAX,
};

#define HSR_XSTATS_MAX (__HSR_XSTATS_MAX - 1)

#endif /* __UAPI_HSR_NETLINK_H */
