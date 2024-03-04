// SPDX-License-Identifier: GPL-2.0
/*  OpenVPN data channel offload
 *
 *  Copyright (C) 2019-2024 OpenVPN, Inc.
 *
 *  Author:	James Yonan <james@openvpn.net>
 *		Antonio Quartulli <antonio@openvpn.net>
 */

#include "io.h"
#include "ovpnstruct.h"
#include "netlink.h"
#include "peer.h"
#include "udp.h"

#include <linux/netdevice.h>
#include <linux/skbuff.h>
#include <net/gso.h>


int ovpn_struct_init(struct net_device *dev)
{
	struct ovpn_struct *ovpn = netdev_priv(dev);
	int err;

	memset(ovpn, 0, sizeof(*ovpn));

	ovpn->dev = dev;

	err = ovpn_nl_init(ovpn);
	if (err < 0)
		return err;

	spin_lock_init(&ovpn->lock);

	ovpn->crypto_wq = alloc_workqueue("ovpn-crypto-wq-%s",
					  WQ_CPU_INTENSIVE | WQ_MEM_RECLAIM, 0, dev->name);
	if (!ovpn->crypto_wq)
		return -ENOMEM;

	ovpn->events_wq = alloc_workqueue("ovpn-events-wq-%s", WQ_MEM_RECLAIM, 0, dev->name);
	if (!ovpn->events_wq)
		return -ENOMEM;

	dev->tstats = netdev_alloc_pcpu_stats(struct pcpu_sw_netstats);
	if (!dev->tstats)
		return -ENOMEM;

	err = security_tun_dev_alloc_security(&ovpn->security);
	if (err < 0)
		return err;

	return 0;
}

static bool ovpn_encrypt_one(struct ovpn_peer *peer, struct sk_buff *skb)
{
	return true;
}

/* Process packets in TX queue in a transport-specific way.
 *
 * UDP transport - encrypt and send across the tunnel.
 */
void ovpn_encrypt_work(struct work_struct *work)
{
	struct sk_buff *skb, *curr, *next;
	struct ovpn_peer *peer;

	peer = container_of(work, struct ovpn_peer, encrypt_work);
	while ((skb = ptr_ring_consume_bh(&peer->tx_ring))) {
		/* this might be a GSO-segmented skb list: process each skb
		 * independently
		 */
		skb_list_walk_safe(skb, curr, next) {
			/* if one segment fails encryption, we drop the entire
			 * packet, because it does not really make sense to send
			 * only part of it at this point
			 */
			if (unlikely(!ovpn_encrypt_one(peer, curr))) {
				kfree_skb_list(skb);
				skb = NULL;
				break;
			}
		}

		/* successful encryption */
		if (likely(skb)) {
			skb_list_walk_safe(skb, curr, next) {
				skb_mark_not_on_list(curr);

				switch (peer->sock->sock->sk->sk_protocol) {
				case IPPROTO_UDP:
					ovpn_udp_send_skb(peer->ovpn, peer, curr);
					break;
				default:
					/* no transport configured yet */
					consume_skb(skb);
					break;
				}
			}
		}

		/* give a chance to be rescheduled if needed */
		cond_resched();
	}
	ovpn_peer_put(peer);
}

/* send skb to connected peer, if any */
static void ovpn_queue_skb(struct ovpn_struct *ovpn, struct sk_buff *skb, struct ovpn_peer *peer)
{
	int ret;

	if (likely(!peer))
		/* retrieve peer serving the destination IP of this packet */
		peer = ovpn_peer_lookup_by_dst(ovpn, skb);
	if (unlikely(!peer)) {
		net_dbg_ratelimited("%s: no peer to send data to\n", ovpn->dev->name);
		goto drop;
	}

	ret = ptr_ring_produce_bh(&peer->tx_ring, skb);
	if (unlikely(ret < 0)) {
		net_err_ratelimited("%s: cannot queue packet to TX ring\n", peer->ovpn->dev->name);
		goto drop;
	}

	if (!queue_work(ovpn->crypto_wq, &peer->encrypt_work))
		ovpn_peer_put(peer);

	return;
drop:
	if (peer)
		ovpn_peer_put(peer);
	kfree_skb_list(skb);
}

/* Return IP protocol version from skb header.
 * Return 0 if protocol is not IPv4/IPv6 or cannot be read.
 */
static __be16 ovpn_ip_check_protocol(struct sk_buff *skb)
{
	__be16 proto = 0;

	/* skb could be non-linear, make sure IP header is in non-fragmented part */
	if (!pskb_network_may_pull(skb, sizeof(struct iphdr)))
		return 0;

	if (ip_hdr(skb)->version == 4)
		proto = htons(ETH_P_IP);
	else if (ip_hdr(skb)->version == 6)
		proto = htons(ETH_P_IPV6);

	return proto;
}

/* Send user data to the network
 */
netdev_tx_t ovpn_net_xmit(struct sk_buff *skb, struct net_device *dev)
{
	struct ovpn_struct *ovpn = netdev_priv(dev);
	struct sk_buff *segments, *tmp, *curr, *next;
	struct sk_buff_head skb_list;
	__be16 proto;
	int ret;

	/* reset netfilter state */
	nf_reset_ct(skb);

	/* verify IP header size in network packet */
	proto = ovpn_ip_check_protocol(skb);
	if (unlikely(!proto || skb->protocol != proto)) {
		net_err_ratelimited("%s: dropping malformed payload packet\n",
				    dev->name);
		goto drop;
	}

	if (skb_is_gso(skb)) {
		segments = skb_gso_segment(skb, 0);
		if (IS_ERR(segments)) {
			ret = PTR_ERR(segments);
			net_err_ratelimited("%s: cannot segment packet: %d\n", dev->name, ret);
			goto drop;
		}

		consume_skb(skb);
		skb = segments;
	}

	/* from this moment on, "skb" might be a list */

	__skb_queue_head_init(&skb_list);
	skb_list_walk_safe(skb, curr, next) {
		skb_mark_not_on_list(curr);

		tmp = skb_share_check(curr, GFP_ATOMIC);
		if (unlikely(!tmp)) {
			kfree_skb_list(next);
			net_err_ratelimited("%s: skb_share_check failed\n", dev->name);
			goto drop_list;
		}

		__skb_queue_tail(&skb_list, tmp);
	}
	skb_list.prev->next = NULL;

	ovpn_queue_skb(ovpn, skb_list.next, NULL);

	return NETDEV_TX_OK;

drop_list:
	skb_queue_walk_safe(&skb_list, curr, next)
		kfree_skb(curr);
drop:
	skb_tx_error(skb);
	kfree_skb_list(skb);
	return NET_XMIT_DROP;
}
