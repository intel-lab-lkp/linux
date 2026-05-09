// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2025-2026 NXP
 */

#include <linux/if_vlan.h>
#include <linux/dsa/tag_hms.h>
#include <linux/packing.h>
#include <linux/slab.h>
#include "tag.h"
#include "tag_8021q.h"

#define HMS_TAG_NAME		"hms"

/*
 * HMS meta header inserted after the source MAC address.
 *
 * |     2B      |     2B      |   0 / 4B / 8B / 12B / 16B / 20B |
 * +------------ +-------------+---------------------------------+
 * |    0xDADC   |   HEADER    |               DATA              |
 * +------------ +------------ +---------------------------------+
 */

#define HMS_HEADER_LEN			4

#define HMS_HEADER_HOST_TO_SWITCH	BIT(15)

/* Binary structure of the HMS Header ETH_P_HMS_META:
 *
 * |   15      |  14  |     13    |   12  |  11   | 10 - 9 |   7 - 4   |  3 - 0  |
 * +-----------+------+-----------+-------+-------+--------+-----------+---------+
 * | TO HOST 0 | META | HOST Only |       |       |        | Switch ID | Port ID |
 * +-----------+------+-----------+-------+-------+--------+-----------+---------+
 */
#define HMS_RX_HEADER_IS_METADATA	BIT(14)
#define HMS_RX_HEADER_HOST_ONLY		BIT(13)

#define HMS_HEADER_PORT_MASK		0x0F
#define HMS_HEADER_SWITCH_MASK		0xF0
#define HMS_HEADER_SWITCH_OFFSET	4
#define HMS_RX_HEADER_PORT_ID(x)	((x) & HMS_HEADER_PORT_MASK)
#define HMS_RX_HEADER_SWITCH_ID(x)	(((x) & HMS_HEADER_SWITCH_MASK) >> HMS_HEADER_SWITCH_OFFSET)

/* TX header */

/*
 * Binary structure of the HMS Header ETH_P_HMS_META:
 *
 * |   15      |  14  |   13   |   12  |  11     | 10 - 9 |  7 - 4    |  3 - 0  |
 * +-----------+------+--------+-------+---------+--------+-----------+---------+
 * |  To SW 1  | META |        |       |         |        | SWITCH ID | PORT ID |
 * +-----------+------+--------+-------+------  -+--------+-----------+---------+
 */

#define HMS_TX_HEADER_SWITCHID(x)	(((x) << HMS_HEADER_SWITCH_OFFSET) & HMS_HEADER_SWITCH_MASK)
#define HMS_TX_HEADER_DESTPORTID(x)	((x) & HMS_HEADER_PORT_MASK)

/* Similar to is_link_local_ether_addr(hdr->h_dest) but also covers PTP */
static inline bool hms_is_link_local(const struct sk_buff *skb)
{
	const struct ethhdr *hdr = eth_hdr(skb);
	u64 dmac = ether_addr_to_u64(hdr->h_dest);

	if (ntohs(hdr->h_proto) == HMS_META_ETYPE)
		return true;

	if ((dmac & HMS_LINKLOCAL_FILTER_A_MASK) ==
	     HMS_LINKLOCAL_FILTER_A)
		return true;

	if ((dmac & HMS_LINKLOCAL_FILTER_B_MASK) ==
	     HMS_LINKLOCAL_FILTER_B)
		return true;

	return false;
}

/* Send VLAN tags with a TPID that blends in with whatever VLAN protocol a
 * bridge spanning ports of this switch might have.
 */
static u16 hms_xmit_tpid(struct dsa_port *dp)
{
	struct dsa_switch *ds = dp->ds;
	struct dsa_port *other_dp;
	u16 proto;

	if (!dsa_port_is_vlan_filtering(dp))
		return ETH_P_HMS_8021Q;

	/* Port is VLAN-aware, so there is a bridge somewhere (a single one,
	 * we're sure about that). It may not be on this port though, so we
	 * need to find it.
	 */
	dsa_switch_for_each_port(other_dp, ds) {
		struct net_device *br = dsa_port_bridge_dev_get(other_dp);

		if (!br)
			continue;

		/* Error is returned only if CONFIG_BRIDGE_VLAN_FILTERING,
		 * which seems pointless to handle, as our port cannot become
		 * VLAN-aware in that case.
		 */
		br_vlan_get_proto(br, &proto);

		return proto;
	}

	WARN_ONCE(1, "Port is VLAN-aware but cannot find associated bridge!\n");

	return ETH_P_HMS_8021Q;
}

static struct sk_buff *hms_imprecise_xmit(struct sk_buff *skb,
					  struct net_device *netdev)
{
	struct dsa_port *dp = dsa_user_to_port(netdev);
	unsigned int bridge_num = dsa_port_bridge_num_get(dp);
	struct net_device *br = dsa_port_bridge_dev_get(dp);
	u16 tx_vid;

	/* If the port is under a VLAN-aware bridge, just slide the
	 * VLAN-tagged packet into the FDB and hope for the best.
	 * This works because we support a single VLAN-aware bridge
	 * across the entire dst, and its VLANs cannot be shared with
	 * any standalone port.
	 */
	if (br_vlan_enabled(br))
		return skb;

	/* If the port is under a VLAN-unaware bridge, use an imprecise
	 * TX VLAN that targets the bridge's entire broadcast domain,
	 * instead of just the specific port.
	 */
	tx_vid = dsa_tag_8021q_bridge_vid(bridge_num);

	if (unlikely(skb_vlan_tag_present(skb))) {
		skb = __vlan_hwaccel_push_inside(skb);
		if (!skb) {
			WARN_ONCE(1, "Failed to push VLAN tag to payload!\n");
			return NULL;
		}
	}

	return dsa_8021q_xmit(skb, netdev, hms_xmit_tpid(dp), tx_vid);
}

static struct sk_buff *hms_meta_xmit(struct sk_buff *skb,
				     struct net_device *netdev)
{
	struct dsa_port *dp = dsa_user_to_port(netdev);
	int len = HMS_HEADER_LEN;
	__be16 *tx_header;

	skb_push(skb, len);

	dsa_alloc_etype_header(skb, len);

	tx_header = dsa_etype_header_pos_tx(skb);

	tx_header[0] = htons(HMS_META_ETYPE);
	tx_header[1] = htons(HMS_HEADER_HOST_TO_SWITCH |
			     HMS_TX_HEADER_SWITCHID(dp->ds->index) |
			     HMS_TX_HEADER_DESTPORTID(dp->index));

	return skb;
}

static struct sk_buff *hms_8021q_xmit(struct sk_buff *skb,
				      struct net_device *netdev)
{
	struct dsa_port *dp = dsa_user_to_port(netdev);
	u16 queue_mapping = skb_get_queue_mapping(skb);
	u8 pcp = netdev_txq_to_tc(netdev, queue_mapping);
	u16 tx_vid = dsa_tag_8021q_standalone_vid(dp);

	return dsa_8021q_xmit(skb, netdev, hms_xmit_tpid(dp),
			      ((pcp << VLAN_PRIO_SHIFT) | tx_vid));
}

static struct sk_buff *hms_xmit(struct sk_buff *skb,
				struct net_device *netdev)
{
	if (skb->offload_fwd_mark)
		return hms_imprecise_xmit(skb, netdev);

	if (unlikely(hms_is_link_local(skb)))
		return hms_meta_xmit(skb, netdev);

	return hms_8021q_xmit(skb, netdev);
}

static bool hms_skb_has_tag_8021q(const struct sk_buff *skb)
{
	u16 tpid = ntohs(eth_hdr(skb)->h_proto);

	return tpid == ETH_P_8021AD || tpid == ETH_P_8021Q ||
	       skb_vlan_tag_present(skb);
}

static bool hms_skb_has_inband_control_extension(const struct sk_buff *skb)
{
	return ntohs(eth_hdr(skb)->h_proto) == HMS_META_ETYPE;
}

static struct sk_buff *hms_rcv_meta_cmd(struct sk_buff *skb, u16 rx_header)
{
	u8 *buf = dsa_etype_header_pos_rx(skb) + HMS_HEADER_LEN;
	int switch_id = HMS_RX_HEADER_SWITCH_ID(rx_header);
	int source_port = HMS_RX_HEADER_PORT_ID(rx_header);
	struct hms_tagger_data *tagger_data;
	struct net_device *master = skb->dev;
	struct dsa_port *cpu_dp;
	struct dsa_switch *ds;

	cpu_dp = master->dsa_ptr;
	ds = dsa_switch_find(cpu_dp->dst->index, switch_id);
	if (!ds) {
		net_err_ratelimited("%s: cannot find switch id %d\n",
				    master->name, switch_id);
		return NULL;
	}

	tagger_data = ds->tagger_data;
	if (!tagger_data->meta_cmd_handler)
		return NULL;

	if (skb_is_nonlinear(skb))
		if (skb_linearize(skb))
			return NULL;

	tagger_data->meta_cmd_handler(ds, source_port, buf,
				skb->len - HMS_HEADER_LEN - 2 * ETH_ALEN);

	/* Discard the meta frame */
	return NULL;
}

static struct sk_buff *hms_rcv_inband_control_extension(struct sk_buff *skb,
							int *source_port,
							int *switch_id,
							bool *host_only)
{
	u16 rx_header;
	int len = 0;

	if (unlikely(!pskb_may_pull(skb, HMS_HEADER_LEN)))
		return NULL;

	rx_header = ntohs(*(__be16 *)skb->data);
	if (rx_header & HMS_RX_HEADER_HOST_ONLY)
		*host_only = true;

	if (rx_header & HMS_RX_HEADER_IS_METADATA)
		return hms_rcv_meta_cmd(skb, rx_header);

	*source_port = HMS_RX_HEADER_PORT_ID(rx_header);
	*switch_id = HMS_RX_HEADER_SWITCH_ID(rx_header);

	len += HMS_HEADER_LEN;

	/* Advance skb->data past the DSA header */
	skb_pull_rcsum(skb, len);

	dsa_strip_etype_header(skb, len);

	/* With skb->data in its final place, update the MAC header
	 * so that eth_hdr() continues to works properly.
	 */
	skb_set_mac_header(skb, -ETH_HLEN);

	return skb;
}

/* If the VLAN in the packet is a tag_8021q one, set @source_port and
 * @switch_id and strip the header. Otherwise set @vid and keep it in the
 * packet.
 */
static void hms_vlan_rcv(struct sk_buff *skb, int *source_port,
			 int *switch_id, int *vbid, int *vid)
{
	dsa_8021q_rcv(skb, source_port, switch_id, vbid, vid);
}

static struct sk_buff *hms_rcv(struct sk_buff *skb,
			       struct net_device *netdev)
{
	int src_port = -1, switch_id = -1, vbid = -1, vid = -1;
	bool host_only = false;

	if (hms_skb_has_inband_control_extension(skb)) {
		skb = hms_rcv_inband_control_extension(skb, &src_port,
						       &switch_id,
						       &host_only);
		if (!skb)
			return NULL;
	}

	/* Packets with in-band control extensions might still have RX VLANs */
	if (likely(hms_skb_has_tag_8021q(skb)))
		hms_vlan_rcv(skb, &src_port, &switch_id, &vbid, &vid);

	if (src_port == -1) /* Need to check it - bridge mode */
		return NULL;

	skb->dev = dsa_tag_8021q_find_user(netdev, src_port, switch_id,
					   vid, vbid);
	if (!skb->dev) {
		/* netdev_warn(netdev, "Couldn't decode source port\n"); */
		return NULL;
	}

	if (!host_only)
		dsa_default_offload_fwd_mark(skb);

	return skb;
}

static void hms_disconnect(struct dsa_switch *ds)
{
	struct hms_tagger_data *tagger_data = ds->tagger_data;

	kfree(tagger_data);
	ds->tagger_data = NULL;
}

static int hms_connect(struct dsa_switch *ds)
{
	struct hms_tagger_data *data;

	data = kzalloc_obj(*data, GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	ds->tagger_data = data;

	return 0;
}

static void hms_flow_dissect(const struct sk_buff *skb, __be16 *proto,
			     int *offset)
{
	/* No tag added for management frames, all ok */
	if (unlikely(hms_is_link_local(skb)))
		return;

	dsa_tag_generic_flow_dissect(skb, proto, offset);
}

static const struct dsa_device_ops hms_netdev_ops = {
	.name			= HMS_TAG_NAME,
	.proto			= DSA_TAG_PROTO_HMS,
	.xmit			= hms_xmit,
	.rcv			= hms_rcv,
	.connect		= hms_connect,
	.disconnect		= hms_disconnect,
	.needed_headroom	= VLAN_HLEN,
	.flow_dissect		= hms_flow_dissect,
	.promisc_on_conduit	= true,
};

MODULE_DESCRIPTION("DSA tag driver for Heterogeneous Multi-SoC switches");
MODULE_LICENSE("GPL");
MODULE_ALIAS_DSA_TAG_DRIVER(DSA_TAG_PROTO_HMS, HMS_TAG_NAME);

module_dsa_tag_driver(hms_netdev_ops);
