/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * AMD/Xilinx TSN Endpoint Ethernet MAC: shared tagger / switch
 * private interface.
 */
#ifndef _NET_DSA_XLNX_TSN_H
#define _NET_DSA_XLNX_TSN_H

#include <linux/skbuff.h>
#include <net/dsa.h>

/**
 * struct xlnx_tsn_tagger_data - per-switch context for the tag protocol
 * @ptp_tx: callback that copies a PTP event frame into the per-port
 *	    hardware TX buffer and holds the skb until the TX IRQ
 *	    delivers the timestamp. Consumes one reference to @skb.
 *
 * Set on @dsa_switch.tagger_data by the switch driver in setup()
 * and read by the tag protocol's xmit hook.
 */
struct xlnx_tsn_tagger_data {
	void (*ptp_tx)(struct dsa_port *dp, struct sk_buff *skb);
};

#endif /* _NET_DSA_XLNX_TSN_H */
