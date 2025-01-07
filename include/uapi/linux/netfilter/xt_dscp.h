/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/* x_tables module for matching/modifying the IPv4/IPv6 DSCP field
 *
 * (C) 2002 Harald Welte <laforge@gnumonks.org>
 * based on ipt_FTOS.c (C) 2000 by Matthew G. Marsh <mgm@paktronix.com>
 * This software is distributed under GNU GPL v2, 1991
 *
 * See RFC2474 for a description of the DSCP field within the IP Header.
 *
 * xt_DSCP.h,v 1.7 2002/03/14 12:03:13 laforge Exp
 * xt_dscp.h,v 1.3 2002/08/05 19:00:21 laforge Exp
*/
#ifndef _UAPI_XT_DSCP_H
#define _UAPI_XT_DSCP_H

#include <linux/types.h>

#define XT_DSCP_MASK	(0xfc)	/* 11111100 */
#define XT_DSCP_SHIFT	(2)
#define XT_DSCP_MAX		(0x3f)	/* 00111111 */

/* match info */
struct xt_dscp_info {
	__u8 dscp;
	__u8 invert;
};

struct xt_tos_match_info {
	__u8 tos_mask;
	__u8 tos_value;
	__u8 invert;
};

/* target info */
struct xt_DSCP_info {
	__u8 dscp;
};

struct xt_tos_target_info {
	__u8 tos_value;
	__u8 tos_mask;
};

#endif /* _UAPI_XT_DSCP_H */
