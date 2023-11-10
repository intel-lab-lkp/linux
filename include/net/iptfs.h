/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _NET_IPTFS_H
#define _NET_IPTFS_H

#include <linux/types.h>
#include <linux/ip.h>

#define IPTFS_SUBTYPE_BASIC 0
#define IPTFS_SUBTYPE_CC 1
#define IPTFS_SUBTYPE_LAST IPTFS_SUBTYPE_CC

#define IPTFS_CC_FLAGS_ECN_CE 0x1
#define IPTFS_CC_FLAGS_PLMTUD 0x2

extern void xfrm_iptfs_get_rtt_and_delays(struct ip_iptfs_cc_hdr *cch, u32 *rtt,
					  u32 *actual_delay, u32 *xmit_delay);

#endif /* _NET_IPTFS_H */
