// SPDX-License-Identifier: GPL-2.0
/*  OpenVPN data channel offload
 *
 *  Copyright (C) 2020-2024 OpenVPN, Inc.
 *
 *  Author:	James Yonan <james@openvpn.net>
 *		Antonio Quartulli <antonio@openvpn.net>
 */

#include <linux/net.h>
#include <linux/netdevice.h>
#include <linux/udp.h>

#include "ovpnstruct.h"
#include "main.h"
#include "io.h"
#include "peer.h"
#include "socket.h"
#include "udp.h"

/**
 * ovpn_socket_release_kref - kref_put callback
 * @kref: the kref object
 *
 * This function releases the lock_sock which was previously
 * acquired by kref_put_sock() in ovpn_socket_put()
 */
static void ovpn_socket_release_kref(struct kref *kref)
	__releases(sock->sock->sk)
{
	struct ovpn_socket *sock = container_of(kref, struct ovpn_socket,
						refcount);

	/* UDP sockets are detached in this kref callback because
	 * we now know for sure that all concurrent users have
	 * finally gone (refcounter dropped to 0).
	 *
	 * Moreover, detachment is performed under lock to prevent
	 * a concurrent ovpn_socket_new() call with the same socket
	 * to find the socket still attached but with refcounter 0.
	 */
	if (sock->sock->sk->sk_protocol == IPPROTO_UDP)
		ovpn_udp_socket_detach(sock->sock);

	bh_unlock_sock(sock->sock->sk);
	sockfd_put(sock->sock);
	kfree_rcu(sock, rcu);
}

/**
 * ovpn_socket_put - decrease reference counter
 * @sock: the socket whose reference counter should be decreased
 *
 * Decreases the refcounter and, if it reached zero, acquires the
 * socket lock and calls the release callback.
 * This particular behaviour allows the callback to operate under
 * lock while atomically getting the refconter to 0.
 *
 * This function is only used internally. Users willing to release
 * references to the ovpn_socket should use ovpn_socket_release()
 */
static void ovpn_socket_put(struct ovpn_socket *sock)
{
	kref_put_sock(&sock->refcount, ovpn_socket_release_kref,
		      sock->sock->sk);
}

/**
 * ovpn_socket_release - release resources owned by socket user
 * @sock: the socket to process
 *
 * This function should be invoked when the user is shutting
 * down and wants to drop its link to the socket.
 *
 * In case of UDP, the socket is owned by multiple users concurrently,
 * therefore the release consists only in decreasing the refcounter.
 * Once the refcounter reaches 0, the socket can be fully detached and
 * released. In turn, the detach routine will drop a reference to the
 * ovpn netdev, pointed by the ovpn_socket.
 */
void ovpn_socket_release(struct ovpn_socket *sock)
{
	if (sock->sock->sk->sk_protocol == IPPROTO_UDP)
		ovpn_socket_put(sock);
}

static bool ovpn_socket_hold(struct ovpn_socket *sock)
{
	return kref_get_unless_zero(&sock->refcount);
}

static struct ovpn_socket *ovpn_socket_get(struct socket *sock)
{
	struct ovpn_socket *ovpn_sock;

	ovpn_sock = rcu_dereference_sk_user_data(sock->sk);
	if (WARN_ON(!ovpn_socket_hold(ovpn_sock)))
		ovpn_sock = NULL;

	return ovpn_sock;
}

static int ovpn_socket_attach(struct socket *sock, struct ovpn_peer *peer)
{
	int ret = -EOPNOTSUPP;

	if (!sock || !peer)
		return -EINVAL;

	if (sock->sk->sk_protocol == IPPROTO_UDP)
		ret = ovpn_udp_socket_attach(sock, peer->ovpn);

	return ret;
}

/* Retrieve the corresponding ovpn object from a UDP socket
 * rcu_read_lock must be held on entry
 */
struct ovpn_priv *ovpn_from_udp_sock(struct sock *sk)
{
	struct ovpn_socket *ovpn_sock;

	if (unlikely(READ_ONCE(udp_sk(sk)->encap_type) != UDP_ENCAP_OVPNINUDP))
		return NULL;

	ovpn_sock = rcu_dereference_sk_user_data(sk);
	if (unlikely(!ovpn_sock))
		return NULL;

	/* make sure that sk matches our stored transport socket */
	if (unlikely(!ovpn_sock->sock || sk != ovpn_sock->sock->sk))
		return NULL;

	return ovpn_sock->ovpn;
}

/**
 * ovpn_socket_new - create a new socket and initialize it
 * @sock: the kernel socket to embed
 * @peer: the peer reachable via this socket
 *
 * Return: an openvpn socket on success or a negative error code otherwise
 */
struct ovpn_socket *ovpn_socket_new(struct socket *sock, struct ovpn_peer *peer)
{
	struct ovpn_socket *ovpn_sock;
	int ret;

	lock_sock(sock->sk);

	ret = ovpn_socket_attach(sock, peer);
	if (ret < 0 && ret != -EALREADY)
		goto err_release;

	/* if this socket is already owned by this interface, just increase the
	 * refcounter and use it as expected.
	 *
	 * Since UDP sockets can be used to talk to multiple remote endpoints,
	 * openvpn normally instantiates only one socket and shares it among all
	 * its peers. For this reason, when we find out that a socket is already
	 * used for some other peer in *this* instance, we can happily increase
	 * its refcounter and use it normally.
	 */
	if (ret == -EALREADY) {
		/* caller is expected to increase the sock refcounter before
		 * passing it to this function. For this reason we drop it if
		 * not needed, like when this socket is already owned.
		 */
		ovpn_sock = ovpn_socket_get(sock);
		release_sock(sock->sk);
		sockfd_put(sock);
		return ovpn_sock;
	}

	ovpn_sock = kzalloc(sizeof(*ovpn_sock), GFP_KERNEL);
	if (!ovpn_sock) {
		ret = -ENOMEM;
		goto err_detach;
	}

	ovpn_sock->ovpn = peer->ovpn;
	ovpn_sock->sock = sock;
	kref_init(&ovpn_sock->refcount);

	rcu_assign_sk_user_data(sock->sk, ovpn_sock);
	release_sock(sock->sk);

	return ovpn_sock;
err_detach:
	if (sock->sk->sk_protocol == IPPROTO_UDP)
		ovpn_udp_socket_detach(sock);
err_release:
	release_sock(sock->sk);
	return ERR_PTR(ret);
}
