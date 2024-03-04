// SPDX-License-Identifier: GPL-2.0
/*  OpenVPN data channel offload
 *
 *  Copyright (C) 2019-2024 OpenVPN, Inc.
 *
 *  Author:	Antonio Quartulli <antonio@openvpn.net>
 */

#include "main.h"
#include "ovpnstruct.h"
#include "socket.h"
#include "udp.h"

#include <linux/socket.h>


/* Set UDP encapsulation callbacks */
int ovpn_udp_socket_attach(struct socket *sock, struct ovpn_struct *ovpn)
{
	struct ovpn_socket *old_data;

	/* sanity check */
	if (sock->sk->sk_protocol != IPPROTO_UDP) {
		netdev_err(ovpn->dev, "%s: expected UDP socket\n", __func__);
		return -EINVAL;
	}

	/* make sure no pre-existing encapsulation handler exists */
	rcu_read_lock();
	old_data = rcu_dereference_sk_user_data(sock->sk);
	rcu_read_unlock();
	if (old_data) {
		if (old_data->ovpn == ovpn) {
			netdev_dbg(ovpn->dev,
				   "%s: provided socket already owned by this interface\n",
				   __func__);
			return -EALREADY;
		}

		netdev_err(ovpn->dev, "%s: provided socket already taken by other user\n",
			   __func__);
		return -EBUSY;
	}

	return 0;
}
