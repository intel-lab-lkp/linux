// SPDX-License-Identifier: GPL-2.0
/*  OpenVPN data channel offload
 *
 *  Copyright (C) 2020-2024 OpenVPN, Inc.
 *
 *  Author:	James Yonan <james@openvpn.net>
 *		Antonio Quartulli <antonio@openvpn.net>
 */

#include "bind.h"
#include "io.h"
#include "main.h"
#include "netlink.h"
#include "ovpnstruct.h"
#include "peer.h"

#include <linux/ip.h>
#include <linux/ipv6.h>
#include <linux/skbuff.h>
#include <linux/list.h>
#include <linux/timer.h>
#include <linux/workqueue.h>


/* Construct a new peer */
struct ovpn_peer *ovpn_peer_new(struct ovpn_struct *ovpn, u32 id)
{
	struct ovpn_peer *peer;
	int ret;

	/* alloc and init peer object */
	peer = kzalloc(sizeof(*peer), GFP_KERNEL);
	if (!peer)
		return ERR_PTR(-ENOMEM);

	peer->id = id;
	peer->halt = false;
	peer->ovpn = ovpn;

	peer->vpn_addrs.ipv4.s_addr = htonl(INADDR_ANY);
	peer->vpn_addrs.ipv6 = in6addr_any;

	RCU_INIT_POINTER(peer->bind, NULL);
	spin_lock_init(&peer->lock);
	kref_init(&peer->refcount);

	ret = dst_cache_init(&peer->dst_cache, GFP_KERNEL);
	if (ret < 0) {
		netdev_err(ovpn->dev, "%s: cannot initialize dst cache\n", __func__);
		goto err;
	}

	ret = ptr_ring_init(&peer->tx_ring, OVPN_QUEUE_LEN, GFP_KERNEL);
	if (ret < 0) {
		netdev_err(ovpn->dev, "%s: cannot allocate TX ring\n", __func__);
		goto err_dst_cache;
	}

	ret = ptr_ring_init(&peer->rx_ring, OVPN_QUEUE_LEN, GFP_KERNEL);
	if (ret < 0) {
		netdev_err(ovpn->dev, "%s: cannot allocate RX ring\n", __func__);
		goto err_tx_ring;
	}

	ret = ptr_ring_init(&peer->netif_rx_ring, OVPN_QUEUE_LEN, GFP_KERNEL);
	if (ret < 0) {
		netdev_err(ovpn->dev, "%s: cannot allocate NETIF RX ring\n", __func__);
		goto err_rx_ring;
	}

	dev_hold(ovpn->dev);

	return peer;
err_rx_ring:
	ptr_ring_cleanup(&peer->rx_ring, NULL);
err_tx_ring:
	ptr_ring_cleanup(&peer->tx_ring, NULL);
err_dst_cache:
	dst_cache_destroy(&peer->dst_cache);
err:
	kfree(peer);
	return ERR_PTR(ret);
}

#define ovpn_peer_index(_tbl, _key, _key_len)		\
	(jhash(_key, _key_len, 0) % HASH_SIZE(_tbl))	\

static void ovpn_peer_free(struct ovpn_peer *peer)
{
	ovpn_bind_reset(peer, NULL);

	WARN_ON(!__ptr_ring_empty(&peer->tx_ring));
	ptr_ring_cleanup(&peer->tx_ring, NULL);
	WARN_ON(!__ptr_ring_empty(&peer->rx_ring));
	ptr_ring_cleanup(&peer->rx_ring, NULL);
	WARN_ON(!__ptr_ring_empty(&peer->netif_rx_ring));
	ptr_ring_cleanup(&peer->netif_rx_ring, NULL);

	dst_cache_destroy(&peer->dst_cache);

	dev_put(peer->ovpn->dev);

	kfree(peer);
}

static void ovpn_peer_release_rcu(struct rcu_head *head)
{
	struct ovpn_peer *peer = container_of(head, struct ovpn_peer, rcu);

	ovpn_peer_free(peer);
}

void ovpn_peer_release(struct ovpn_peer *peer)
{
	call_rcu(&peer->rcu, ovpn_peer_release_rcu);
}

static void ovpn_peer_delete_work(struct work_struct *work)
{
	struct ovpn_peer *peer = container_of(work, struct ovpn_peer,
					      delete_work);
	ovpn_peer_release(peer);
}

/* Use with kref_put calls, when releasing refcount
 * on ovpn_peer objects.  This method should only
 * be called from process context with config_mutex held.
 */
void ovpn_peer_release_kref(struct kref *kref)
{
	struct ovpn_peer *peer = container_of(kref, struct ovpn_peer, refcount);

	INIT_WORK(&peer->delete_work, ovpn_peer_delete_work);
	queue_work(peer->ovpn->events_wq, &peer->delete_work);
}

/**
 * ovpn_peer_lookup_by_dst() - Lookup peer to send skb to
 *
 * This function takes a tunnel packet and looks up the peer to send it to
 * after encapsulation. The skb is expected to be the in-tunnel packet, without
 * any OpenVPN related header.
 *
 * Assume that the IP header is accessible in the skb data.
 *
 * @ovpn: the private data representing the current VPN session
 * @skb: the skb to extract the destination address from
 *
 * Return the peer if found or NULL otherwise.
 */
struct ovpn_peer *ovpn_peer_lookup_by_dst(struct ovpn_struct *ovpn, struct sk_buff *skb)
{
	struct ovpn_peer *tmp, *peer = NULL;

	/* in P2P mode, no matter the destination, packets are always sent to the single peer
	 * listening on the other side
	 */
	if (ovpn->mode == OVPN_MODE_P2P) {
		rcu_read_lock();
		tmp = rcu_dereference(ovpn->peer);
		if (likely(tmp && ovpn_peer_hold(tmp)))
			peer = tmp;
		rcu_read_unlock();
	}

	return peer;
}

static bool ovpn_peer_skb_to_sockaddr(struct sk_buff *skb, struct sockaddr_storage *ss)
{
	struct sockaddr_in6 *sa6;
	struct sockaddr_in *sa4;

	ss->ss_family = skb_protocol_to_family(skb);
	switch (ss->ss_family) {
	case AF_INET:
		sa4 = (struct sockaddr_in *)ss;
		sa4->sin_family = AF_INET;
		sa4->sin_addr.s_addr = ip_hdr(skb)->saddr;
		sa4->sin_port = udp_hdr(skb)->source;
		break;
	case AF_INET6:
		sa6 = (struct sockaddr_in6 *)ss;
		sa6->sin6_family = AF_INET6;
		sa6->sin6_addr = ipv6_hdr(skb)->saddr;
		sa6->sin6_port = udp_hdr(skb)->source;
		break;
	default:
		return false;
	}

	return true;
}

static bool ovpn_peer_transp_match(struct ovpn_peer *peer, struct sockaddr_storage *ss)
{
	struct ovpn_bind *bind = rcu_dereference(peer->bind);
	struct sockaddr_in6 *sa6;
	struct sockaddr_in *sa4;

	if (unlikely(!bind))
		return false;

	if (ss->ss_family != bind->sa.in4.sin_family)
		return false;

	switch (ss->ss_family) {
	case AF_INET:
		sa4 = (struct sockaddr_in *)ss;
		if (sa4->sin_addr.s_addr != bind->sa.in4.sin_addr.s_addr)
			return false;
		if (sa4->sin_port != bind->sa.in4.sin_port)
			return false;
		break;
	case AF_INET6:
		sa6 = (struct sockaddr_in6 *)ss;
		if (memcmp(&sa6->sin6_addr, &bind->sa.in6.sin6_addr, sizeof(struct in6_addr)))
			return false;
		if (sa6->sin6_port != bind->sa.in6.sin6_port)
			return false;
		break;
	default:
		return false;
	}

	return true;
}

static struct ovpn_peer *ovpn_peer_lookup_transp_addr_p2p(struct ovpn_struct *ovpn,
							  struct sockaddr_storage *ss)
{
	struct ovpn_peer *tmp, *peer = NULL;

	rcu_read_lock();
	tmp = rcu_dereference(ovpn->peer);
	if (likely(tmp && ovpn_peer_transp_match(tmp, ss) && ovpn_peer_hold(tmp)))
		peer = tmp;
	rcu_read_unlock();

	return peer;
}

struct ovpn_peer *ovpn_peer_lookup_transp_addr(struct ovpn_struct *ovpn, struct sk_buff *skb)
{
	struct ovpn_peer *peer = NULL;
	struct sockaddr_storage ss = { 0 };

	if (unlikely(!ovpn_peer_skb_to_sockaddr(skb, &ss)))
		return NULL;

	if (ovpn->mode == OVPN_MODE_P2P)
		peer = ovpn_peer_lookup_transp_addr_p2p(ovpn, &ss);

	return peer;
}

static struct ovpn_peer *ovpn_peer_lookup_id_p2p(struct ovpn_struct *ovpn, u32 peer_id)
{
	struct ovpn_peer *tmp, *peer = NULL;

	rcu_read_lock();
	tmp = rcu_dereference(ovpn->peer);
	if (likely(tmp && tmp->id == peer_id && ovpn_peer_hold(tmp)))
		peer = tmp;
	rcu_read_unlock();

	return peer;
}

struct ovpn_peer *ovpn_peer_lookup_id(struct ovpn_struct *ovpn, u32 peer_id)
{
	struct ovpn_peer *peer = NULL;

	if (ovpn->mode == OVPN_MODE_P2P)
		peer = ovpn_peer_lookup_id_p2p(ovpn, peer_id);

	return peer;
}

static int ovpn_peer_add_p2p(struct ovpn_struct *ovpn, struct ovpn_peer *peer)
{
	struct ovpn_peer *tmp;

	spin_lock_bh(&ovpn->lock);
	/* in p2p mode it is possible to have a single peer only, therefore the
	 * old one is released and substituted by the new one
	 */
	tmp = rcu_dereference(ovpn->peer);
	if (tmp) {
		tmp->delete_reason = OVPN_DEL_PEER_REASON_TEARDOWN;
		ovpn_peer_put(tmp);
	}

	rcu_assign_pointer(ovpn->peer, peer);
	spin_unlock_bh(&ovpn->lock);

	return 0;
}

/* assume refcounter was increased by caller */
int ovpn_peer_add(struct ovpn_struct *ovpn, struct ovpn_peer *peer)
{
	switch (ovpn->mode) {
	case OVPN_MODE_P2P:
		return ovpn_peer_add_p2p(ovpn, peer);
	default:
		return -EOPNOTSUPP;
	}
}

static int ovpn_peer_del_p2p(struct ovpn_peer *peer, enum ovpn_del_peer_reason reason)
{
	struct ovpn_peer *tmp;
	int ret = -ENOENT;

	spin_lock_bh(&peer->ovpn->lock);
	tmp = rcu_dereference(peer->ovpn->peer);
	if (tmp != peer)
		goto unlock;

	ovpn_peer_put(tmp);
	tmp->delete_reason = reason;
	RCU_INIT_POINTER(peer->ovpn->peer, NULL);
	ret = 0;

unlock:
	spin_unlock_bh(&peer->ovpn->lock);

	return ret;
}

void ovpn_peer_release_p2p(struct ovpn_struct *ovpn)
{
	struct ovpn_peer *tmp;

	rcu_read_lock();
	tmp = rcu_dereference(ovpn->peer);
	if (!tmp)
		goto unlock;

	ovpn_peer_del_p2p(tmp, OVPN_DEL_PEER_REASON_TEARDOWN);
unlock:
	rcu_read_unlock();
}

int ovpn_peer_del(struct ovpn_peer *peer, enum ovpn_del_peer_reason reason)
{
	switch (peer->ovpn->mode) {
	case OVPN_MODE_P2P:
		return ovpn_peer_del_p2p(peer, reason);
	default:
		return -EOPNOTSUPP;
	}
}
