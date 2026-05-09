/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright 2025-2026 NXP
 */

#ifndef _NET_DSA_TAG_HMS_H
#define _NET_DSA_TAG_HMS_H

#include <linux/dsa/8021q.h>
#include <net/dsa.h>

#define ETH_P_HMS_8021Q	ETH_P_8021Q /* 0x8100 */

#define HMS_META_ETYPE	0xDADC /* HMS internal meta frame EtherType */

/* IEEE 802.3 Annex 57A: Slow Protocols PDUs (01:80:C2:xx:xx:xx) */
#define HMS_LINKLOCAL_FILTER_A		0x0180C2000000ull
#define HMS_LINKLOCAL_FILTER_A_MASK	0xFFFFFF000000ull
/* IEEE 1588 Annex F: Transport of PTP over Ethernet (01:1B:19:xx:xx:xx) */
#define HMS_LINKLOCAL_FILTER_B		0x011B19000000ull
#define HMS_LINKLOCAL_FILTER_B_MASK	0xFFFFFF000000ull

struct hms_tagger_data {
	void (*meta_cmd_handler)(struct dsa_switch *ds, int port,
				 void *buf, size_t len);
};

#endif /* _NET_DSA_TAG_HMS_H */
