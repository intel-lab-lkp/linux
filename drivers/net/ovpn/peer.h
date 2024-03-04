/* SPDX-License-Identifier: GPL-2.0-only */
/* OpenVPN data channel offload
 *
 *  Copyright (C) 2020-2024 OpenVPN, Inc.
 *
 *  Author:	James Yonan <james@openvpn.net>
 *		Antonio Quartulli <antonio@openvpn.net>
 */

#ifndef _NET_OVPN_OVPNPEER_H_
#define _NET_OVPN_OVPNPEER_H_

#include "bind.h"
#include "crypto.h"
#include "socket.h"
#include "stats.h"

#include <linux/ptr_ring.h>
#include <net/dst_cache.h>
#include <uapi/linux/ovpn.h>


struct ovpn_peer {
	struct ovpn_struct *ovpn;

	u32 id;

	struct {
		struct in_addr ipv4;
		struct in6_addr ipv6;
	} vpn_addrs;

	/* work objects to handle encryption/decryption of packets.
	 * these works are queued on the ovpn->crypt_wq workqueue.
	 */
	struct work_struct encrypt_work;
	struct work_struct decrypt_work;

	struct ptr_ring tx_ring;
	struct ptr_ring rx_ring;
	struct ptr_ring netif_rx_ring;

	struct napi_struct napi;

	struct ovpn_socket *sock;

	/* state of the TCP reading. Needed to keep track of how much of a single packet has already
	 * been read from the stream and how much is missing
	 */
	struct {
		struct ptr_ring tx_ring;
		struct work_struct tx_work;
		struct work_struct rx_work;

		u8 raw_len[sizeof(u16)];
		struct sk_buff *skb;
		u16 offset;
		u16 data_len;
		struct {
			void (*sk_state_change)(struct sock *sk);
			void (*sk_data_ready)(struct sock *sk);
			void (*sk_write_space)(struct sock *sk);
			struct proto *prot;
		} sk_cb;
	} tcp;


	struct ovpn_crypto_state crypto;

	struct dst_cache dst_cache;

	/* our binding to peer, protected by spinlock */
	struct ovpn_bind __rcu *bind;

	/* true if ovpn_peer_mark_delete was called */
	bool halt;

	/* per-peer in-VPN rx/tx stats */
	struct ovpn_peer_stats vpn_stats;

	/* per-peer link/transport rx/tx stats */
	struct ovpn_peer_stats link_stats;

	/* why peer was deleted - keepalive timeout, module removed etc */
	enum ovpn_del_peer_reason delete_reason;

	/* protects binding to peer (bind) and timers
	 * (keepalive_xmit, keepalive_expire)
	 */
	spinlock_t lock;

	/* needed because crypto methods can go async */
	struct kref refcount;

	/* needed to free a peer in an RCU safe way */
	struct rcu_head rcu;

	/* needed to notify userspace about deletion */
	struct work_struct delete_work;
};

void ovpn_peer_release_kref(struct kref *kref);
void ovpn_peer_release(struct ovpn_peer *peer);

static inline bool ovpn_peer_hold(struct ovpn_peer *peer)
{
	return kref_get_unless_zero(&peer->refcount);
}

static inline void ovpn_peer_put(struct ovpn_peer *peer)
{
	kref_put(&peer->refcount, ovpn_peer_release_kref);
}

struct ovpn_peer *ovpn_peer_new(struct ovpn_struct *ovpn, u32 id);

int ovpn_peer_add(struct ovpn_struct *ovpn, struct ovpn_peer *peer);
int ovpn_peer_del(struct ovpn_peer *peer, enum ovpn_del_peer_reason reason);
struct ovpn_peer *ovpn_peer_find(struct ovpn_struct *ovpn, u32 peer_id);
void ovpn_peer_release_p2p(struct ovpn_struct *ovpn);

struct ovpn_peer *ovpn_peer_lookup_transp_addr(struct ovpn_struct *ovpn, struct sk_buff *skb);
struct ovpn_peer *ovpn_peer_lookup_by_dst(struct ovpn_struct *ovpn, struct sk_buff *skb);
struct ovpn_peer *ovpn_peer_lookup_by_src(struct ovpn_struct *ovpn, struct sk_buff *skb);
struct ovpn_peer *ovpn_peer_lookup_id(struct ovpn_struct *ovpn, u32 peer_id);

int ovpn_peer_reset_sockaddr(struct ovpn_peer *peer, const struct sockaddr_storage *ss,
			     const u8 *local_ip);

#endif /* _NET_OVPN_OVPNPEER_H_ */
