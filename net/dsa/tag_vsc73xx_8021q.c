// SPDX-License-Identifier: GPL-2.0 OR MIT
/* Copyright (C) 2024 Pawel Dembicki <paweldembicki@gmail.com>
 */
#include <linux/dsa/8021q.h>
#include <linux/dsa/vsc73xx.h>

#include "tag.h"
#include "tag_8021q.h"

#define VSC73XX_8021Q_NAME "vsc73xx-8021q"

struct vsc73xx_8021q_tagger_private {
	struct vsc73xx_8021q_tagger_data data; /* Must be first */
	struct kthread_worker *xmit_worker;
};

static struct sk_buff *vsc73xx_defer_xmit(struct dsa_port *dp, struct sk_buff *skb)
{
	struct vsc73xx_8021q_tagger_private *priv = dp->ds->tagger_data;
	struct vsc73xx_8021q_tagger_data *data = &priv->data;
	void (*xmit_work_fn)(struct kthread_work *work);
	struct vsc73xx_deferred_xmit_work *xmit_work;
	struct kthread_worker *xmit_worker;

	xmit_work_fn = data->xmit_work_fn;
	xmit_worker = priv->xmit_worker;

	if (!xmit_work_fn || !xmit_worker)
		return NULL;

	xmit_work = kzalloc(sizeof(*xmit_work), GFP_ATOMIC);
	if (!xmit_work)
		return NULL;

	/* Calls vsc73xx_port_deferred_xmit in vitesse-vsc73xx-core.c */
	kthread_init_work(&xmit_work->work, xmit_work_fn);
	/* Increase refcount so the kfree_skb in dsa_slave_xmit
	 * won't really free the packet.
	 */
	xmit_work->dp = dp;
	xmit_work->skb = skb_get(skb);

	kthread_queue_work(xmit_worker, &xmit_work->work);

	return NULL;
}

static struct sk_buff *
vsc73xx_xmit(struct sk_buff *skb, struct net_device *netdev)
{
	struct dsa_port *dp = dsa_user_to_port(netdev);
	u16 queue_mapping = skb_get_queue_mapping(skb);
	u16 tx_vid = dsa_tag_8021q_standalone_vid(dp);
	struct ethhdr *hdr = eth_hdr(skb);
	u8 pcp;

	if (is_link_local_ether_addr(hdr->h_dest))
		return vsc73xx_defer_xmit(dp, skb);

	if (skb->offload_fwd_mark) {
		unsigned int bridge_num = dsa_port_bridge_num_get(dp);
		struct net_device *br = dsa_port_bridge_dev_get(dp);

		if (br_vlan_enabled(br))
			return skb;

		tx_vid = dsa_tag_8021q_bridge_vid(bridge_num);
	}

	pcp = netdev_txq_to_tc(netdev, queue_mapping);

	return dsa_8021q_xmit(skb, netdev, ETH_P_8021Q,
			      ((pcp << VLAN_PRIO_SHIFT) | tx_vid));
}

static struct sk_buff *
vsc73xx_rcv(struct sk_buff *skb, struct net_device *netdev)
{
	int src_port = -1, switch_id = -1, vbid = -1, vid = -1;

	dsa_8021q_rcv(skb, &src_port, &switch_id, &vbid, &vid);

	skb->dev = dsa_tag_8021q_find_user(netdev, src_port, switch_id,
					   vid, vbid);
	if (!skb->dev) {
		dev_warn_ratelimited(&netdev->dev,
				     "Couldn't decode source port\n");
		return NULL;
	}

	dsa_default_offload_fwd_mark(skb);

	return skb;
}

static void vsc73xx_disconnect(struct dsa_switch *ds)
{
	struct vsc73xx_8021q_tagger_private *priv = ds->tagger_data;

	kthread_destroy_worker(priv->xmit_worker);
	kfree(priv);
	ds->tagger_data = NULL;
}

static int vsc73xx_connect(struct dsa_switch *ds)
{
	struct vsc73xx_8021q_tagger_private *priv;
	int err;

	priv = kzalloc(sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->xmit_worker = kthread_create_worker(0, "vsc73xx_xmit");
	if (IS_ERR(priv->xmit_worker)) {
		err = PTR_ERR(priv->xmit_worker);
		kfree(priv);
		return err;
	}

	ds->tagger_data = priv;

	return 0;
}

static const struct dsa_device_ops vsc73xx_8021q_netdev_ops = {
	.name			= VSC73XX_8021Q_NAME,
	.proto			= DSA_TAG_PROTO_VSC73XX_8021Q,
	.xmit			= vsc73xx_xmit,
	.rcv			= vsc73xx_rcv,
	.connect		= vsc73xx_connect,
	.disconnect		= vsc73xx_disconnect,
	.needed_headroom	= VLAN_HLEN,
	.promisc_on_conduit	= true,
};

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("DSA tag driver for VSC73XX family of switches, using VLAN");
MODULE_ALIAS_DSA_TAG_DRIVER(DSA_TAG_PROTO_VSC73XX_8021Q, VSC73XX_8021Q_NAME);

module_dsa_tag_driver(vsc73xx_8021q_netdev_ops);
