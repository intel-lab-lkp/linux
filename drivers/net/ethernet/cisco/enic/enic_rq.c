// SPDX-License-Identifier: GPL-2.0-only
// Copyright 2024 Cisco Systems, Inc.  All rights reserved.

#include <linux/skbuff.h>
#include <linux/if_vlan.h>
#include "enic.h"
#include "enic_rq.h"
#include "vnic_rq.h"
#include "cq_enet_desc.h"

#define ENIC_LARGE_PKT_THRESHOLD		1000

void enic_intr_update_pkt_size(struct vnic_rx_bytes_counter *pkt_size,
			       u32 pkt_len)
{
	if (pkt_len >= ENIC_LARGE_PKT_THRESHOLD)
		pkt_size->large_pkt_bytes_cnt += pkt_len;
	else
		pkt_size->small_pkt_bytes_cnt += pkt_len;
}

void enic_rq_set_skb_flags(struct vnic_rq *vrq, u8 type, u32 rss_hash, u8 rss_type, u8 fcoe,
			   u8 fcoe_fc_crc_ok, u8 vlan_stripped, u8 csum_not_calc,
			   u8 tcp_udp_csum_ok, u8 ipv6, u8 ipv4_csum_ok, u16 vlan_tci,
			   struct sk_buff *skb)
{
	struct enic *enic = vnic_dev_priv(vrq->vdev);
	struct net_device *netdev = enic->netdev;
	struct enic_rq_stats *rqstats =  &enic->rq[vrq->index].stats;
	bool outer_csum_ok = true, encap = false;

	if ((netdev->features & NETIF_F_RXHASH) && rss_hash && type == 3) {
		switch (rss_type) {
		case CQ_ENET_RQ_DESC_RSS_TYPE_TCP_IPv4:
		case CQ_ENET_RQ_DESC_RSS_TYPE_TCP_IPv6:
		case CQ_ENET_RQ_DESC_RSS_TYPE_TCP_IPv6_EX:
			skb_set_hash(skb, rss_hash, PKT_HASH_TYPE_L4);
			rqstats->l4_rss_hash++;
			break;
		case CQ_ENET_RQ_DESC_RSS_TYPE_IPv4:
		case CQ_ENET_RQ_DESC_RSS_TYPE_IPv6:
		case CQ_ENET_RQ_DESC_RSS_TYPE_IPv6_EX:
			skb_set_hash(skb, rss_hash, PKT_HASH_TYPE_L3);
			rqstats->l3_rss_hash++;
			break;
		}
	}
	if (enic->vxlan.vxlan_udp_port_number) {
		switch (enic->vxlan.patch_level) {
		case 0:
			if (fcoe) {
				encap = true;
				outer_csum_ok = fcoe_fc_crc_ok;
			}
			break;
		case 2:
			if (type == 7 && (rss_hash & BIT(0))) {
				encap = true;
				outer_csum_ok = (rss_hash & BIT(1)) &&
						(rss_hash & BIT(2));
			}
			break;
		}
	}

	/* Hardware does not provide whole packet checksum. It only
	 * provides pseudo checksum. Since hw validates the packet
	 * checksum but not provide us the checksum value. use
	 * CHECSUM_UNNECESSARY.
	 *
	 * In case of encap pkt tcp_udp_csum_ok/tcp_udp_csum_ok is
	 * inner csum_ok. outer_csum_ok is set by hw when outer udp
	 * csum is correct or is zero.
	 */
	if ((netdev->features & NETIF_F_RXCSUM) && !csum_not_calc &&
	    tcp_udp_csum_ok && outer_csum_ok && (ipv4_csum_ok || ipv6)) {
		skb->ip_summed = CHECKSUM_UNNECESSARY;
		skb->csum_level = encap;
		if (encap)
			rqstats->csum_unnecessary_encap++;
		else
			rqstats->csum_unnecessary++;
	}

	if (vlan_stripped) {
		__vlan_hwaccel_put_tag(skb, htons(ETH_P_8021Q), vlan_tci);
		rqstats->vlan_stripped++;
	}
}

int enic_rq_pkt_error(struct vnic_rq *vrq, u8 packet_error, u8 fcs_ok, u16 bytes_written)
{
	struct enic *enic = vnic_dev_priv(vrq->vdev);
	struct enic_rq_stats *rqstats = &enic->rq[vrq->index].stats;
	int ret = 0;

	if (packet_error) {
		if (!fcs_ok) {
			if (bytes_written > 0) {
				rqstats->bad_fcs++;
				ret = 1;
			} else if (bytes_written == 0) {
				rqstats->pkt_truncated++;
				ret = 2;
			}
		}
	}
	return ret;
}

int enic_rq_service(struct vnic_dev *vdev, struct cq_desc *cq_desc,
		    u8 type, u16 q_number, u16 completed_index, void *opaque)
{
	struct enic *enic = vnic_dev_priv(vdev);

	vnic_rq_service(&enic->rq[q_number].vrq, cq_desc, completed_index,
			VNIC_RQ_RETURN_DESC, enic_rq_indicate_buf, opaque);

	return 0;
}
