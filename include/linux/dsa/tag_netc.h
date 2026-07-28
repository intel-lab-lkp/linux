/* SPDX-License-Identifier: GPL-2.0
 *
 * Copyright 2025-2026 NXP
 */

#ifndef __NET_DSA_TAG_NETC_H
#define __NET_DSA_TAG_NETC_H

#include <linux/skbuff.h>
#include <net/dsa.h>

#define NETC_TAG_MAX_LEN			14
#define NETC_TAG_TS_REQ_ID			GENMASK(3, 0)
#define NETC_PTP_FLAG_ONESTEP			BIT(0)
#define NETC_PTP_FLAG_TWOSTEP			BIT(1)

struct netc_skb_cb {
	struct sk_buff *clone;
	unsigned long ptp_tx_time;
	u64 tstamp;
	u8 ptp_flag;
	u8 ts_req_id;
	/* One-step Sync parsing results, computed in netc_port_txtstamp()
	 * and reused in the tagger xmit path and the deferred work, to avoid
	 * re-parsing the PTP header. Valid only while
	 * ptp_flag == NETC_PTP_FLAG_ONESTEP.
	 */
	u16 correction_offset;
	u16 timestamp_offset;
	bool is_udp;
};

#define NETC_SKB_CB(skb)	((struct netc_skb_cb *)((skb)->cb))

/**
 * struct netc_tagger_data - NETC tagger/switch-driver shared operations
 * @twostep_tstamp_handler: Called by the tagger when a two-step transmit
 *	timestamp response is received, to deliver the timestamp to the
 *	switch driver.
 * @onestep_sync_handler: Called from the tagger xmit path for a one-step Sync
 *	frame. Returns the skb if it can be transmitted immediately (the
 *	in-flight slot has been claimed and PM_SINGLE_STEP programmed), or NULL
 *	if the frame has been queued for deferred transmission or dropped.
 * @onestep_sync_xmit: Called by the switch driver to transmit a deferred
 *	one-step Sync frame directly to the conduit, bypassing dsa_user_xmit().
 */
struct netc_tagger_data {
	void (*twostep_tstamp_handler)(struct dsa_switch *ds, int port,
				       u8 ts_req_id, u64 ts);
	struct sk_buff *(*onestep_sync_handler)(struct dsa_switch *ds,
						int port, struct sk_buff *skb);
	netdev_tx_t (*onestep_sync_xmit)(struct sk_buff *skb,
					 struct net_device *ndev);
};

#endif
