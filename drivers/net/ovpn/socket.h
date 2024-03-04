/* SPDX-License-Identifier: GPL-2.0-only */
/*  OpenVPN data channel offload
 *
 *  Copyright (C) 2020-2024 OpenVPN, Inc.
 *
 *  Author:	James Yonan <james@openvpn.net>
 *		Antonio Quartulli <antonio@openvpn.net>
 */

#ifndef _NET_OVPN_SOCK_H_
#define _NET_OVPN_SOCK_H_

#include <linux/net.h>
#include <linux/kref.h>
#include <linux/ptr_ring.h>
#include <net/sock.h>


struct ovpn_struct;
struct ovpn_peer;

/**
 * struct ovpn_socket - a kernel socket referenced in the ovpn code
 */
struct ovpn_socket {
	union {
		/* the VPN session object owning this socket (UDP only) */
		struct ovpn_struct *ovpn;

		/* TCP only */
		struct {
			/** @peer: the unique peer transmitting over this socket (TCP only) */
			struct ovpn_peer *peer;
			struct ptr_ring recv_ring;
		};
	};

	/* the kernel socket */
	struct socket *sock;
	/* amount of contexts currently referencing this object */
	struct kref refcount;
	/* member used to schedule RCU destructor callback */
	struct rcu_head rcu;
};

struct ovpn_struct *ovpn_from_udp_sock(struct sock *sk);

void ovpn_socket_release_kref(struct kref *kref);

static inline void ovpn_socket_put(struct ovpn_socket *sock)
{
	kref_put(&sock->refcount, ovpn_socket_release_kref);
}

struct ovpn_socket *ovpn_socket_new(struct socket *sock, struct ovpn_peer *peer);

#endif /* _NET_OVPN_SOCK_H_ */
