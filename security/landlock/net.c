// SPDX-License-Identifier: GPL-2.0-only
/*
 * Landlock LSM - Network management and hooks
 *
 * Copyright © 2022-2023 Huawei Tech. Co., Ltd.
 * Copyright © 2022-2023 Microsoft Corporation
 */

#include <linux/in.h>
#include <linux/net.h>
#include <linux/socket.h>
#include <net/ipv6.h>

#include "common.h"
#include "cred.h"
#include "limits.h"
#include "net.h"
#include "ruleset.h"

int landlock_append_net_rule(struct landlock_ruleset *const ruleset,
			     const u16 port, access_mask_t access_rights)
{
	int err;
	const struct landlock_id id = {
		.key.data = (__force uintptr_t)htons(port),
		.type = LANDLOCK_KEY_NET_PORT,
	};

	BUILD_BUG_ON(sizeof(port) > sizeof(id.key.data));

	/* Transforms relative access rights to absolute ones. */
	access_rights |= LANDLOCK_MASK_ACCESS_NET &
			 ~landlock_get_net_access_mask(ruleset, 0);

	mutex_lock(&ruleset->lock);
	err = landlock_insert_rule(ruleset, id, access_rights);
	mutex_unlock(&ruleset->lock);

	return err;
}

static const struct landlock_ruleset *get_current_net_domain(void)
{
	const union access_masks any_net = {
		.net = ~0,
	};

	return landlock_match_ruleset(landlock_get_current_domain(), any_net);
}

static int check_access_port(const struct landlock_ruleset *const dom,
			     __be16 port, access_mask_t access_request)
{
	layer_mask_t layer_masks[LANDLOCK_NUM_ACCESS_NET] = {};
	const struct landlock_rule *rule;
	struct landlock_id id = {
		.type = LANDLOCK_KEY_NET_PORT,
	};

	id.key.data = (__force uintptr_t)port;
	BUILD_BUG_ON(sizeof(port) > sizeof(id.key.data));

	rule = landlock_find_rule(dom, id);
	access_request = landlock_init_layer_masks(
		dom, access_request, &layer_masks, LANDLOCK_KEY_NET_PORT);
	if (landlock_unmask_layers(rule, access_request, &layer_masks,
				   ARRAY_SIZE(layer_masks)))
		return 0;

	return -EACCES;
}

/*
 * Checks that TCP @sock and @address attributes are correct for bind(2).
 *
 * On success, extracts port from @address in @port and returns 0.
 *
 * This validation is consistent with network stack and returns the error
 * in the order corresponding to the order of errors from the network stack.
 * It's required to not wrongfully return -EACCES instead of meaningful network
 * stack level errors. Consistency is tested with kselftest.
 *
 * This helper does not provide consistency of error codes for BPF filter
 * (if any).
 */
static int
check_tcp_bind_consistency_and_get_port(struct socket *const sock,
					struct sockaddr *const address,
					const int addrlen, __be16 *port)
{
	/* IPV6_ADDRFORM can change sk->sk_family under us. */
	switch (READ_ONCE(sock->sk->sk_family)) {
	case AF_INET:
		const struct sockaddr_in *const addr =
			(struct sockaddr_in *)address;

		/* Cf. inet_bind_sk(). */
		if (addrlen < sizeof(struct sockaddr_in))
			return -EINVAL;
		/*
		 * For compatibility reason, accept AF_UNSPEC for bind
		 * accesses (mapped to AF_INET) only if the address is
		 * INADDR_ANY (cf. __inet_bind).
		 */
		if (addr->sin_family != AF_INET) {
			if (addr->sin_family != AF_UNSPEC ||
			    addr->sin_addr.s_addr != htonl(INADDR_ANY))
				return -EAFNOSUPPORT;
		}
		*port = ((struct sockaddr_in *)address)->sin_port;
		break;
#if IS_ENABLED(CONFIG_IPV6)
	case AF_INET6:
		/* Cf. inet6_bind_sk(). */
		if (addrlen < SIN6_LEN_RFC2133)
			return -EINVAL;
		/* Cf. __inet6_bind(). */
		if (address->sa_family != AF_INET6)
			return -EAFNOSUPPORT;
		*port = ((struct sockaddr_in6 *)address)->sin6_port;
		break;
#endif /* IS_ENABLED(CONFIG_IPV6) */
	default:
		WARN_ON_ONCE(0);
		return -EACCES;
	}
	return 0;
}

/*
 * Checks that TCP @sock and @address attributes are correct for connect(2).
 *
 * On success, extracts port from @address in @port and returns 0.
 *
 * This validation is consistent with network stack and returns the error
 * in the order corresponding to the order of errors from the network stack.
 * It's required to not wrongfully return -EACCES instead of meaningful network
 * stack level error. Consistency is partially tested with kselftest.
 *
 * This helper does not provide consistency of error codes for BPF filter
 * (if any).
 *
 * The function holds socket lock while checking the socket state.
 */
static int
check_tcp_connect_consistency_and_get_port(struct socket *const sock,
					   struct sockaddr *const address,
					   const int addrlen, __be16 *port)
{
	int err = 0;
	struct sock *const sk = sock->sk;

	/* Cf. __inet_stream_connect(). */
	lock_sock(sk);
	switch (sock->state) {
	default:
		err = -EINVAL;
		break;
	case SS_CONNECTED:
		err = -EISCONN;
		break;
	case SS_CONNECTING:
		/*
		 * Calling connect(2) on nonblocking socket with SYN_SENT or SYN_RECV
		 * state immediately returns -EISCONN and -EALREADY (Cf. __inet_stream_connect()).
		 *
		 * This check is not tested with kselftests.
		 */
		if ((sock->file->f_flags & O_NONBLOCK) &&
		    ((1 << sk->sk_state) & (TCPF_SYN_SENT | TCPF_SYN_RECV))) {
			if (inet_test_bit(DEFER_CONNECT, sk))
				err = -EISCONN;
			else
				err = -EALREADY;
			break;
		}

		/*
		 * Current state is possible in two cases:
		 * 1. connect(2) is called upon nonblocking socket and previous
		 *    connection attempt was closed by RST packet (therefore socket is
		 *    in TCP_CLOSE state). In this case connect(2) calls
		 *    sk_prot->disconnect(), changes socket state and increases number
		 *    of disconnects.
		 * 2. connect(2) is called twice upon socket with TCP_FASTOPEN_CONNECT
		 *    option set. If socket state is TCP_CLOSE connect(2) does the
		 *    same logic as in point 1 case. Otherwise connect(2) may freeze
		 *    after inet_wait_for_connect() call since SYN was never sent.
		 *
		 * For both this cases Landlock cannot provide error consistency since
		 * 1. Both cases involve executing some network stack logic and changing
		 *    the socket state.
		 * 2. It cannot omit access check and allow network stack handle error
		 *    consistency since socket can change its state to SS_UNCONNECTED
		 *    before it will be locked again in inet_stream_connect().
		 *
		 * Therefore it is only possible to return 0 and check access right with
		 * check_access_port() helper.
		 */
		release_sock(sk);
		return 0;
	case SS_UNCONNECTED:
		if (sk->sk_state != TCP_CLOSE)
			err = -EISCONN;
		break;
	}
	release_sock(sk);

	if (err)
		return err;

	/* IPV6_ADDRFORM can change sk->sk_family under us. */
	switch (READ_ONCE(sk->sk_family)) {
	case AF_INET:
		/* Cf. tcp_v4_connect(). */
		if (addrlen < sizeof(struct sockaddr_in))
			return -EINVAL;
		if (address->sa_family != AF_INET)
			return -EAFNOSUPPORT;

		*port = ((struct sockaddr_in *)address)->sin_port;
		break;
#if IS_ENABLED(CONFIG_IPV6)
	case AF_INET6:
		/* Cf. tcp_v6_connect(). */
		if (addrlen < SIN6_LEN_RFC2133)
			return -EINVAL;
		if (address->sa_family != AF_INET6)
			return -EAFNOSUPPORT;

		*port = ((struct sockaddr_in6 *)address)->sin6_port;
		break;
#endif /* IS_ENABLED(CONFIG_IPV6) */
	default:
		WARN_ON_ONCE(0);
		return -EACCES;
	}

	return 0;
}

static int hook_socket_bind(struct socket *const sock,
			    struct sockaddr *const address, const int addrlen)
{
	int err;
	__be16 port;
	const struct landlock_ruleset *const dom = get_current_net_domain();

	if (!dom)
		return 0;
	if (WARN_ON_ONCE(dom->num_layers < 1))
		return -EACCES;

	if (sk_is_tcp(sock->sk)) {
		err = check_tcp_bind_consistency_and_get_port(sock, address,
							      addrlen, &port);
		if (err)
			return err;
		return check_access_port(dom, port,
					 LANDLOCK_ACCESS_NET_BIND_TCP);
	}
	return 0;
}

static int hook_socket_connect(struct socket *const sock,
			       struct sockaddr *const address,
			       const int addrlen)
{
	int err;
	__be16 port;
	const struct landlock_ruleset *const dom = get_current_net_domain();

	if (!dom)
		return 0;
	if (WARN_ON_ONCE(dom->num_layers < 1))
		return -EACCES;

	if (sk_is_tcp(sock->sk)) {
		/* Checks for minimal header length to safely read sa_family. */
		if (addrlen < sizeof(address->sa_family))
			return -EINVAL;
		/*
		 * Connecting to an address with AF_UNSPEC dissolves the TCP
		 * association, which have the same effect as closing the
		 * connection while retaining the socket object (i.e., the file
		 * descriptor).  As for dropping privileges, closing
		 * connections is always allowed.
		 *
		 * For a TCP access control system, this request is legitimate.
		 * Let the network stack handle potential inconsistencies and
		 * return -EINVAL if needed.
		 */
		if (address->sa_family == AF_UNSPEC)
			return 0;

		err = check_tcp_connect_consistency_and_get_port(
			sock, address, addrlen, &port);
		if (err)
			return err;
		return check_access_port(dom, port,
					 LANDLOCK_ACCESS_NET_CONNECT_TCP);
	}
	return 0;
}

static struct security_hook_list landlock_hooks[] __ro_after_init = {
	LSM_HOOK_INIT(socket_bind, hook_socket_bind),
	LSM_HOOK_INIT(socket_connect, hook_socket_connect),
};

__init void landlock_add_net_hooks(void)
{
	security_add_hooks(landlock_hooks, ARRAY_SIZE(landlock_hooks),
			   &landlock_lsmid);
}
