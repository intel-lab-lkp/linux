// SPDX-License-Identifier: GPL-2.0
/*  OpenVPN data channel offload
 *
 *  Copyright (C) 2020-2024 OpenVPN, Inc.
 *
 *  Author:	Antonio Quartulli <antonio@openvpn.net>
 */

#include "main.h"
#include "io.h"
#include "netlink.h"
#include "ovpnstruct.h"
#include "peer.h"
#include "packet.h"

#include <uapi/linux/ovpn.h>

#include <linux/netdevice.h>
#include <linux/netlink.h>
#include <linux/socket.h>
#include <linux/types.h>
#include <net/genetlink.h>
#include <uapi/linux/in.h>
#include <uapi/linux/in6.h>

/** The ovpn netlink family */
static struct genl_family ovpn_nl_family;

enum ovpn_nl_multicast_groups {
	OVPN_MCGRP_PEERS,
};

static const struct genl_multicast_group ovpn_nl_mcgrps[] = {
	[OVPN_MCGRP_PEERS] = { .name = OVPN_NL_MULTICAST_GROUP_PEERS },
};

/** KEYDIR policy. Can be used for configuring an encryption and a decryption key */
static const struct nla_policy ovpn_nl_policy_keydir[NUM_OVPN_A_KEYDIR] = {
	[OVPN_A_KEYDIR_CIPHER_KEY] = NLA_POLICY_MAX_LEN(U8_MAX),
	[OVPN_A_KEYDIR_NONCE_TAIL] = NLA_POLICY_EXACT_LEN(NONCE_TAIL_SIZE),
};

/** KEYCONF policy */
static const struct nla_policy ovpn_nl_policy_keyconf[NUM_OVPN_A_KEYCONF] = {
	[OVPN_A_KEYCONF_SLOT] = NLA_POLICY_RANGE(NLA_U8, __OVPN_KEY_SLOT_FIRST,
						 NUM_OVPN_KEY_SLOT - 1),
	[OVPN_A_KEYCONF_KEY_ID] = { .type = NLA_U8 },
	[OVPN_A_KEYCONF_CIPHER_ALG] = { .type = NLA_U16 },
	[OVPN_A_KEYCONF_ENCRYPT_DIR] = NLA_POLICY_NESTED(ovpn_nl_policy_keydir),
	[OVPN_A_KEYCONF_DECRYPT_DIR] = NLA_POLICY_NESTED(ovpn_nl_policy_keydir),
};

/** PEER policy */
static const struct nla_policy ovpn_nl_policy_peer[NUM_OVPN_A_PEER] = {
	[OVPN_A_PEER_ID] = { .type = NLA_U32 },
	[OVPN_A_PEER_SOCKADDR_REMOTE] = NLA_POLICY_MIN_LEN(sizeof(struct sockaddr)),
	[OVPN_A_PEER_SOCKET] = { .type = NLA_U32 },
	[OVPN_A_PEER_VPN_IPV4] = { .type = NLA_U32 },
	[OVPN_A_PEER_VPN_IPV6] = NLA_POLICY_EXACT_LEN(sizeof(struct in6_addr)),
	[OVPN_A_PEER_LOCAL_IP] = NLA_POLICY_MAX_LEN(sizeof(struct in6_addr)),
	[OVPN_A_PEER_LOCAL_PORT] = NLA_POLICY_MAX_LEN(sizeof(u16)),
	[OVPN_A_PEER_KEEPALIVE_INTERVAL] = { .type = NLA_U32 },
	[OVPN_A_PEER_KEEPALIVE_TIMEOUT] = { .type = NLA_U32 },
	[OVPN_A_PEER_DEL_REASON] = NLA_POLICY_RANGE(NLA_U8, __OVPN_DEL_PEER_REASON_FIRST,
						    NUM_OVPN_DEL_PEER_REASON - 1),
	[OVPN_A_PEER_KEYCONF] = NLA_POLICY_NESTED(ovpn_nl_policy_keyconf),
};

/** Generic message container policy */
static const struct nla_policy ovpn_nl_policy[NUM_OVPN_A] = {
	[OVPN_A_IFINDEX] = { .type = NLA_U32 },
	[OVPN_A_IFNAME] = NLA_POLICY_MAX_LEN(IFNAMSIZ),
	[OVPN_A_MODE] = NLA_POLICY_RANGE(NLA_U8, __OVPN_MODE_FIRST,
					 NUM_OVPN_MODE - 1),
	[OVPN_A_PEER] = NLA_POLICY_NESTED(ovpn_nl_policy_peer),
};

/**
 * ovpn_get_dev_from_attrs() - retrieve the netdevice a netlink message is targeting
 */
static struct net_device *
ovpn_get_dev_from_attrs(struct net *net, struct nlattr **attrs)
{
	struct net_device *dev;
	int ifindex;

	if (!attrs[OVPN_A_IFINDEX])
		return ERR_PTR(-EINVAL);

	ifindex = nla_get_u32(attrs[OVPN_A_IFINDEX]);

	dev = dev_get_by_index(net, ifindex);
	if (!dev)
		return ERR_PTR(-ENODEV);

	if (!ovpn_dev_is_valid(dev))
		goto err_put_dev;

	return dev;

err_put_dev:
	dev_put(dev);

	return ERR_PTR(-EINVAL);
}

/**
 * ovpn_pre_doit() - Prepare ovpn genl doit request
 * @ops: requested netlink operation
 * @skb: Netlink message with request data
 * @info: receiver information
 *
 * Return: 0 on success or negative error number in case of failure
 */
static int ovpn_pre_doit(const struct genl_split_ops *ops, struct sk_buff *skb,
			 struct genl_info *info)
{
	struct net *net = genl_info_net(info);
	struct net_device *dev;

	/* the OVPN_CMD_NEW_IFACE command is different from the rest as it
	 * just expects an IFNAME, while all the others expect an IFINDEX
	 */
	if (info->genlhdr->cmd == OVPN_CMD_NEW_IFACE) {
		if (!info->attrs[OVPN_A_IFNAME]) {
			GENL_SET_ERR_MSG(info, "no interface name specified");
			return -EINVAL;
		}
		return 0;
	}

	dev = ovpn_get_dev_from_attrs(net, info->attrs);
	if (IS_ERR(dev))
		return PTR_ERR(dev);

	info->user_ptr[0] = netdev_priv(dev);

	return 0;
}

/**
 * ovpn_post_doit() - complete ovpn genl doit request
 * @ops: requested netlink operation
 * @skb: Netlink message with request data
 * @info: receiver information
 */
static void ovpn_post_doit(const struct genl_split_ops *ops, struct sk_buff *skb,
			   struct genl_info *info)
{
	struct ovpn_struct *ovpn;

	ovpn = info->user_ptr[0];
	/* in case of OVPN_CMD_NEW_IFACE, there is no pre-stored device */
	if (ovpn)
		dev_put(ovpn->dev);
}

static int ovpn_nl_set_peer(struct sk_buff *skb, struct genl_info *info)
{
	bool keepalive_set = false, new_peer = false;
	struct nlattr *attrs[NUM_OVPN_A_PEER];
	struct ovpn_struct *ovpn = info->user_ptr[0];
	struct sockaddr_storage *ss = NULL;
	u32 sockfd, id, interv, timeout;
	struct socket *sock = NULL;
	struct sockaddr_in mapped;
	struct sockaddr_in6 *in6;
	struct ovpn_peer *peer;
	size_t sa_len, ip_len;
	u8 *local_ip = NULL;
	int ret;

	if (!info->attrs[OVPN_A_PEER])
		return -EINVAL;

	ret = nla_parse_nested(attrs, NUM_OVPN_A_PEER - 1, info->attrs[OVPN_A_PEER], NULL,
			       info->extack);
	if (ret)
		return ret;

	if (!attrs[OVPN_A_PEER_ID]) {
		netdev_err(ovpn->dev, "%s: peer ID missing\n", __func__);
		return -EINVAL;
	}

	id = nla_get_u32(attrs[OVPN_A_PEER_ID]);
	/* check if the peer exists first, otherwise create a new one */
	peer = ovpn_peer_lookup_id(ovpn, id);
	if (!peer) {
		peer = ovpn_peer_new(ovpn, id);
		new_peer = true;
		if (IS_ERR(peer)) {
			netdev_err(ovpn->dev, "%s: cannot create new peer object for peer %u (sockaddr=%pIScp): %ld\n",
				   __func__, id, ss, PTR_ERR(peer));
			return PTR_ERR(peer);
		}
	}

	if (new_peer && !attrs[OVPN_A_PEER_SOCKET]) {
		netdev_err(ovpn->dev, "%s: peer socket missing\n", __func__);
		ret = -EINVAL;
		goto peer_release;
	}

	if (new_peer && ovpn->mode == OVPN_MODE_MP && !attrs[OVPN_A_PEER_VPN_IPV4] &&
	    !attrs[OVPN_A_PEER_VPN_IPV6]) {
		netdev_err(ovpn->dev, "%s: a VPN IP is required when adding a peer in MP mode\n",
			   __func__);
		ret = -EINVAL;
		goto peer_release;
	}

	if (attrs[OVPN_A_PEER_SOCKET]) {
		/* lookup the fd in the kernel table and extract the socket object */
		sockfd = nla_get_u32(attrs[OVPN_A_PEER_SOCKET]);
		/* sockfd_lookup() increases sock's refcounter */
		sock = sockfd_lookup(sockfd, &ret);
		if (!sock) {
			netdev_dbg(ovpn->dev, "%s: cannot lookup peer socket (fd=%u): %d\n",
				   __func__, sockfd, ret);
			ret = -ENOTSOCK;
			goto peer_release;
		}

		/* Only when using UDP as transport protocol the remote endpoint can be configured
		 * so that ovpn knows where to send packets to.
		 *
		 * In case of TCP, the socket is connected to the peer and ovpn will just send bytes
		 * over it, without the need to specify a destination.
		 */
		if (sock->sk->sk_protocol == IPPROTO_UDP && attrs[OVPN_A_PEER_SOCKADDR_REMOTE]) {
			ss = nla_data(attrs[OVPN_A_PEER_SOCKADDR_REMOTE]);
			sa_len = nla_len(attrs[OVPN_A_PEER_SOCKADDR_REMOTE]);
			switch (sa_len) {
			case sizeof(struct sockaddr_in):
				if (ss->ss_family == AF_INET)
					/* valid sockaddr */
					break;

				netdev_err(ovpn->dev, "%s: remote sockaddr_in has invalid family\n",
					   __func__);
				ret = -EINVAL;
				goto peer_release;
			case sizeof(struct sockaddr_in6):
				if (ss->ss_family == AF_INET6)
					/* valid sockaddr */
					break;

				netdev_err(ovpn->dev, "%s: remote sockaddr_in6 has invalid family\n",
					   __func__);
				ret = -EINVAL;
				goto peer_release;
			default:
				netdev_err(ovpn->dev, "%s: invalid size for sockaddr\n", __func__);
				ret = -EINVAL;
				goto peer_release;
			}

			if (ss->ss_family == AF_INET6) {
				in6 = (struct sockaddr_in6 *)ss;

				if (ipv6_addr_type(&in6->sin6_addr) & IPV6_ADDR_MAPPED) {
					mapped.sin_family = AF_INET;
					mapped.sin_addr.s_addr = in6->sin6_addr.s6_addr32[3];
					mapped.sin_port = in6->sin6_port;
					ss = (struct sockaddr_storage *)&mapped;
				}
			}

			/* When using UDP we may be talking over socket bound to 0.0.0.0/::.
			 * In this case, if the host has multiple IPs, we need to make sure
			 * that outgoing traffic has as source IP the same address that the
			 * peer is using to reach us.
			 *
			 * Since early control packets were all forwarded to userspace, we
			 * need the latter to tell us what IP has to be used.
			 */
			if (attrs[OVPN_A_PEER_LOCAL_IP]) {
				ip_len = nla_len(attrs[OVPN_A_PEER_LOCAL_IP]);
				local_ip = nla_data(attrs[OVPN_A_PEER_LOCAL_IP]);

				if (ip_len == sizeof(struct in_addr)) {
					if (ss->ss_family != AF_INET) {
						netdev_dbg(ovpn->dev,
							   "%s: the specified local IP is IPv4, but the peer endpoint is not\n",
							   __func__);
						ret = -EINVAL;
						goto peer_release;
					}
				} else if (ip_len == sizeof(struct in6_addr)) {
					bool is_mapped = ipv6_addr_type((struct in6_addr *)local_ip) &
						IPV6_ADDR_MAPPED;

					if (ss->ss_family != AF_INET6 && !is_mapped) {
						netdev_dbg(ovpn->dev,
							   "%s: the specified local IP is IPv6, but the peer endpoint is not\n",
							   __func__);
						ret = -EINVAL;
						goto peer_release;
					}

					if (is_mapped)
						/* this is an IPv6-mapped IPv4 address, therefore extract
						 * the actual v4 address from the last 4 bytes
						 */
						local_ip += 12;
				} else {
					netdev_dbg(ovpn->dev,
						   "%s: invalid length %zu for local IP\n", __func__,
						   ip_len);
					ret = -EINVAL;
					goto peer_release;
				}
			}

			/* set peer sockaddr */
			ret = ovpn_peer_reset_sockaddr(peer, ss, local_ip);
			if (ret < 0)
				goto peer_release;
		}

		if (peer->sock)
			ovpn_socket_put(peer->sock);

		peer->sock = ovpn_socket_new(sock, peer);
		if (IS_ERR(peer->sock)) {
			sockfd_put(sock);
			peer->sock = NULL;
			ret = -ENOTSOCK;
			goto peer_release;
		}
	}

	/* VPN IPs cannot be updated, because they are hashed */
	if (new_peer && attrs[OVPN_A_PEER_VPN_IPV4]) {
		if (nla_len(attrs[OVPN_A_PEER_VPN_IPV4]) != sizeof(struct in_addr)) {
			ret = -EINVAL;
			goto peer_release;
		}

		peer->vpn_addrs.ipv4.s_addr = nla_get_be32(attrs[OVPN_A_PEER_VPN_IPV4]);
	}

	/* VPN IPs cannot be updated, because they are hashed */
	if (new_peer && attrs[OVPN_A_PEER_VPN_IPV6]) {
		if (nla_len(attrs[OVPN_A_PEER_VPN_IPV6]) != sizeof(struct in6_addr)) {
			ret = -EINVAL;
			goto peer_release;
		}

		memcpy(&peer->vpn_addrs.ipv6, nla_data(attrs[OVPN_A_PEER_VPN_IPV6]),
		       sizeof(struct in6_addr));
	}

	/* when setting the keepalive, both parameters have to be configured */
	if (attrs[OVPN_A_PEER_KEEPALIVE_INTERVAL] &&
	    attrs[OVPN_A_PEER_KEEPALIVE_TIMEOUT]) {
		keepalive_set = true;
		interv = nla_get_u32(attrs[OVPN_A_PEER_KEEPALIVE_INTERVAL]);
		timeout = nla_get_u32(attrs[OVPN_A_PEER_KEEPALIVE_TIMEOUT]);
	}

	if (keepalive_set)
		ovpn_peer_keepalive_set(peer, interv, timeout);

	netdev_dbg(ovpn->dev,
		   "%s: adding peer with endpoint=%pIScp/%s id=%u VPN-IPv4=%pI4 VPN-IPv6=%pI6c\n",
		   __func__, ss, sock->sk->sk_prot_creator->name, peer->id,
		   &peer->vpn_addrs.ipv4.s_addr, &peer->vpn_addrs.ipv6);

	ret = ovpn_peer_add(ovpn, peer);
	if (ret < 0) {
		netdev_err(ovpn->dev, "%s: cannot add new peer (id=%u) to hashtable: %d\n",
			   __func__, peer->id, ret);
		goto peer_release;
	}

	return 0;

peer_release:
	/* release right away because peer is not really used in any context */
	ovpn_peer_release(peer);
	return ret;

}

static int ovpn_nl_send_peer(struct sk_buff *skb, const struct ovpn_peer *peer, u32 portid,
			     u32 seq, int flags)
{
	const struct ovpn_bind *bind;
	struct nlattr *attr;
	void *hdr;

	hdr = genlmsg_put(skb, portid, seq, &ovpn_nl_family, flags, OVPN_CMD_SET_PEER);
	if (!hdr) {
		netdev_dbg(peer->ovpn->dev, "%s: cannot create message header\n", __func__);
		return -EMSGSIZE;
	}

	attr = nla_nest_start(skb, OVPN_A_PEER);
	if (!attr) {
		netdev_dbg(peer->ovpn->dev, "%s: cannot create submessage\n", __func__);
		goto err;
	}

	if (nla_put_u32(skb, OVPN_A_PEER_ID, peer->id))
		goto err;

	if (peer->vpn_addrs.ipv4.s_addr != htonl(INADDR_ANY))
		if (nla_put(skb, OVPN_A_PEER_VPN_IPV4, sizeof(peer->vpn_addrs.ipv4),
			    &peer->vpn_addrs.ipv4))
			goto err;

	if (memcmp(&peer->vpn_addrs.ipv6, &in6addr_any, sizeof(peer->vpn_addrs.ipv6)))
		if (nla_put(skb, OVPN_A_PEER_VPN_IPV6, sizeof(peer->vpn_addrs.ipv6),
			    &peer->vpn_addrs.ipv6))
			goto err;

	if (nla_put_u32(skb, OVPN_A_PEER_KEEPALIVE_INTERVAL,
			peer->keepalive_interval) ||
	    nla_put_u32(skb, OVPN_A_PEER_KEEPALIVE_TIMEOUT,
			peer->keepalive_timeout))
		goto err;

	rcu_read_lock();
	bind = rcu_dereference(peer->bind);
	if (bind) {
		if (bind->sa.in4.sin_family == AF_INET) {
			if (nla_put(skb, OVPN_A_PEER_SOCKADDR_REMOTE,
				    sizeof(bind->sa.in4), &bind->sa.in4) ||
			    nla_put(skb, OVPN_A_PEER_LOCAL_IP,
				    sizeof(bind->local.ipv4), &bind->local.ipv4))
				goto err_unlock;
		} else if (bind->sa.in4.sin_family == AF_INET6) {
			if (nla_put(skb, OVPN_A_PEER_SOCKADDR_REMOTE,
				    sizeof(bind->sa.in6), &bind->sa.in6) ||
			    nla_put(skb, OVPN_A_PEER_LOCAL_IP,
				    sizeof(bind->local.ipv6), &bind->local.ipv6))
				goto err_unlock;
		}
	}
	rcu_read_unlock();

	if (nla_put_net16(skb, OVPN_A_PEER_LOCAL_PORT,
			  inet_sk(peer->sock->sock->sk)->inet_sport) ||
	    /* VPN RX stats */
	    nla_put_u64_64bit(skb, OVPN_A_PEER_VPN_RX_BYTES,
			      atomic64_read(&peer->vpn_stats.rx.bytes),
			      OVPN_A_PEER_UNSPEC) ||
	    nla_put_u32(skb, OVPN_A_PEER_VPN_RX_PACKETS,
			atomic_read(&peer->vpn_stats.rx.packets)) ||
	    /* VPN TX stats */
	    nla_put_u64_64bit(skb, OVPN_A_PEER_VPN_TX_BYTES,
			      atomic64_read(&peer->vpn_stats.tx.bytes),
			      OVPN_A_PEER_UNSPEC) ||
	    nla_put_u32(skb, OVPN_A_PEER_VPN_TX_PACKETS,
			atomic_read(&peer->vpn_stats.tx.packets)) ||
	    /* link RX stats */
	    nla_put_u64_64bit(skb, OVPN_A_PEER_LINK_RX_BYTES,
			      atomic64_read(&peer->link_stats.rx.bytes),
			      OVPN_A_PEER_UNSPEC) ||
	    nla_put_u32(skb, OVPN_A_PEER_LINK_RX_PACKETS,
			atomic_read(&peer->link_stats.rx.packets)) ||
	    /* link TX stats */
	    nla_put_u64_64bit(skb, OVPN_A_PEER_LINK_TX_BYTES,
			      atomic64_read(&peer->link_stats.tx.bytes),
			      OVPN_A_PEER_UNSPEC) ||
	    nla_put_u32(skb, OVPN_A_PEER_LINK_TX_PACKETS,
			atomic_read(&peer->link_stats.tx.packets)))
		goto err;

	nla_nest_end(skb, attr);
	genlmsg_end(skb, hdr);

	return 0;
err_unlock:
	rcu_read_unlock();
err:
	genlmsg_cancel(skb, hdr);
	return -EMSGSIZE;
}

static int ovpn_nl_get_peer(struct sk_buff *skb, struct genl_info *info)
{
	struct nlattr *attrs[NUM_OVPN_A_PEER];
	struct ovpn_struct *ovpn = info->user_ptr[0];
	struct ovpn_peer *peer;
	struct sk_buff *msg;
	u32 peer_id;
	int ret;

	if (!info->attrs[OVPN_A_PEER])
		return -EINVAL;

	ret = nla_parse_nested(attrs, NUM_OVPN_A_PEER - 1, info->attrs[OVPN_A_PEER], NULL,
			       info->extack);
	if (ret)
		return ret;

	if (!attrs[OVPN_A_PEER_ID])
		return -EINVAL;

	peer_id = nla_get_u32(attrs[OVPN_A_PEER_ID]);
	peer = ovpn_peer_lookup_id(ovpn, peer_id);
	if (!peer)
		return -ENOENT;

	msg = nlmsg_new(NLMSG_DEFAULT_SIZE, GFP_KERNEL);
	if (!msg)
		return -ENOMEM;

	ret = ovpn_nl_send_peer(msg, peer, info->snd_portid, info->snd_seq, 0);
	if (ret < 0) {
		nlmsg_free(msg);
		goto err;
	}

	ret = genlmsg_reply(msg, info);
err:
	ovpn_peer_put(peer);
	return ret;
}

static int ovpn_nl_dump_peers(struct sk_buff *skb, struct netlink_callback *cb)
{
	struct net *netns = sock_net(cb->skb->sk);
	struct nlattr **attrbuf;
	struct ovpn_struct *ovpn;
	struct net_device *dev;
	int ret, bkt, last_idx = cb->args[1], dumped = 0;
	struct ovpn_peer *peer;

	attrbuf = kcalloc(NUM_OVPN_A, sizeof(*attrbuf), GFP_KERNEL);
	if (!attrbuf)
		return -ENOMEM;

	ret = nlmsg_parse_deprecated(cb->nlh, GENL_HDRLEN, attrbuf, NUM_OVPN_A,
				     ovpn_nl_policy, NULL);
	if (ret < 0) {
		pr_err("ovpn: cannot parse incoming request in %s: %d\n", __func__, ret);
		goto err;
	}

	dev = ovpn_get_dev_from_attrs(netns, attrbuf);
	if (IS_ERR(dev)) {
		ret = PTR_ERR(dev);
		pr_err("ovpn: cannot retrieve device in %s: %d\n", __func__, ret);
		goto err;
	}

	ovpn = netdev_priv(dev);

	rcu_read_lock();
	hash_for_each_rcu(ovpn->peers.by_id, bkt, peer, hash_entry_id) {
		/* skip already dumped peers that were dumped by previous invocations */
		if (last_idx > 0) {
			last_idx--;
			continue;
		}

		if (ovpn_nl_send_peer(skb, peer, NETLINK_CB(cb->skb).portid,
				      cb->nlh->nlmsg_seq, NLM_F_MULTI) < 0)
			break;

		/* count peers being dumped during this invocation */
		dumped++;
	}
	rcu_read_unlock();

	dev_put(dev);

	/* sum up peers dumped in this message, so that at the next invocation
	 * we can continue from where we left
	 */
	cb->args[1] += dumped;
	ret = skb->len;
err:
	kfree(attrbuf);
	return ret;
}

static int ovpn_nl_del_peer(struct sk_buff *skb, struct genl_info *info)
{
	struct nlattr *attrs[NUM_OVPN_A_PEER];
	struct ovpn_struct *ovpn = info->user_ptr[0];
	struct ovpn_peer *peer;
	u32 peer_id;
	int ret;

	if (!info->attrs[OVPN_A_PEER])
		return -EINVAL;

	ret = nla_parse_nested(attrs, NUM_OVPN_A_PEER - 1, info->attrs[OVPN_A_PEER], NULL,
			       info->extack);
	if (ret)
		return ret;

	if (!attrs[OVPN_A_PEER_ID])
		return -EINVAL;

	peer_id = nla_get_u32(attrs[OVPN_A_PEER_ID]);

	peer = ovpn_peer_lookup_id(ovpn, peer_id);
	if (!peer)
		return -ENOENT;

	netdev_dbg(ovpn->dev, "%s: peer id=%u\n", __func__, peer->id);
	ret = ovpn_peer_del(peer, OVPN_DEL_PEER_REASON_USERSPACE);
	ovpn_peer_put(peer);

	return ret;
}

static int ovpn_nl_get_key_dir(struct genl_info *info, struct nlattr *key,
				    enum ovpn_cipher_alg cipher,
				    struct ovpn_key_direction *dir)
{
	struct nlattr *attr, *attrs[NUM_OVPN_A_KEYDIR];
	int ret;

	ret = nla_parse_nested(attrs, NUM_OVPN_A_KEYDIR - 1, key, NULL, info->extack);
	if (ret)
		return ret;

	switch (cipher) {
	case OVPN_CIPHER_ALG_AES_GCM:
	case OVPN_CIPHER_ALG_CHACHA20_POLY1305:
		attr = attrs[OVPN_A_KEYDIR_CIPHER_KEY];
		if (!attr)
			return -EINVAL;

		dir->cipher_key = nla_data(attr);
		dir->cipher_key_size = nla_len(attr);

		attr = attrs[OVPN_A_KEYDIR_NONCE_TAIL];
		/* These algorithms require a 96bit nonce,
		 * Construct it by combining 4-bytes packet id and
		 * 8-bytes nonce-tail from userspace
		 */
		if (!attr)
			return -EINVAL;

		dir->nonce_tail = nla_data(attr);
		dir->nonce_tail_size = nla_len(attr);
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int ovpn_nl_set_key(struct sk_buff *skb, struct genl_info *info)
{
	struct nlattr *p_attrs[NUM_OVPN_A_PEER];
	struct nlattr *attrs[NUM_OVPN_A_KEYCONF];
	struct ovpn_struct *ovpn = info->user_ptr[0];
	struct ovpn_peer_key_reset pkr;
	struct ovpn_peer *peer;
	u32 peer_id;
	int ret;

	if (!info->attrs[OVPN_A_PEER])
		return -EINVAL;

	ret = nla_parse_nested(p_attrs, NUM_OVPN_A_PEER - 1, info->attrs[OVPN_A_PEER],
			       NULL, info->extack);
	if (ret)
		return ret;

	if (!p_attrs[OVPN_A_PEER_ID] || !p_attrs[OVPN_A_PEER_KEYCONF])
		return -EINVAL;

	ret = nla_parse_nested(attrs, NUM_OVPN_A_KEYCONF - 1, p_attrs[OVPN_A_PEER_KEYCONF],
			       NULL, info->extack);
	if (ret)
		return ret;

	if (!attrs[OVPN_A_KEYCONF_SLOT] ||
	    !attrs[OVPN_A_KEYCONF_KEY_ID] ||
	    !attrs[OVPN_A_KEYCONF_CIPHER_ALG] ||
	    !attrs[OVPN_A_KEYCONF_ENCRYPT_DIR] ||
	    !attrs[OVPN_A_KEYCONF_DECRYPT_DIR])
		return -EINVAL;

	peer_id = nla_get_u32(p_attrs[OVPN_A_PEER_ID]);
	pkr.slot = nla_get_u8(attrs[OVPN_A_KEYCONF_SLOT]);
	pkr.key.key_id = nla_get_u16(attrs[OVPN_A_KEYCONF_KEY_ID]);
	pkr.key.cipher_alg = nla_get_u16(attrs[OVPN_A_KEYCONF_CIPHER_ALG]);

	ret = ovpn_nl_get_key_dir(info, attrs[OVPN_A_KEYCONF_ENCRYPT_DIR],
				  pkr.key.cipher_alg, &pkr.key.encrypt);
	if (ret < 0)
		return ret;

	ret = ovpn_nl_get_key_dir(info, attrs[OVPN_A_KEYCONF_DECRYPT_DIR],
				  pkr.key.cipher_alg, &pkr.key.decrypt);
	if (ret < 0)
		return ret;

	peer = ovpn_peer_lookup_id(ovpn, peer_id);
	if (!peer) {
		netdev_dbg(ovpn->dev, "%s: no peer with id %u to set key for\n", __func__, peer_id);
		return -ENOENT;
	}

	mutex_lock(&peer->crypto.mutex);
	ret = ovpn_crypto_state_reset(&peer->crypto, &pkr);
	if (ret < 0) {
		netdev_dbg(ovpn->dev, "%s: cannot install new key for peer %u\n", __func__,
			   peer_id);
		goto unlock;
	}

	netdev_dbg(ovpn->dev, "%s: new key installed (id=%u) for peer %u\n", __func__,
		   pkr.key.key_id, peer_id);
unlock:
	mutex_unlock(&peer->crypto.mutex);
	ovpn_peer_put(peer);
	return ret;
}

static int ovpn_nl_del_key(struct sk_buff *skb, struct genl_info *info)
{
	struct nlattr *p_attrs[NUM_OVPN_A_PEER];
	struct nlattr *attrs[NUM_OVPN_A_KEYCONF];
	struct ovpn_struct *ovpn = info->user_ptr[0];
	enum ovpn_key_slot slot;
	struct ovpn_peer *peer;
	u32 peer_id;
	int ret;

	if (!info->attrs[OVPN_A_PEER])
		return -EINVAL;

	ret = nla_parse_nested(p_attrs, NUM_OVPN_A_PEER - 1, info->attrs[OVPN_A_PEER],
			       NULL, info->extack);

	if (!p_attrs[OVPN_A_PEER_ID] || !p_attrs[OVPN_A_PEER_KEYCONF])
		return -EINVAL;

	ret = nla_parse_nested(attrs, NUM_OVPN_A_KEYCONF - 1, p_attrs[OVPN_A_PEER_KEYCONF],
			       NULL, info->extack);
	if (ret)
		return ret;

	if (!attrs[OVPN_A_KEYCONF_SLOT])
		return -EINVAL;

	peer_id = nla_get_u32(p_attrs[OVPN_A_PEER_ID]);
	slot = nla_get_u8(attrs[OVPN_A_KEYCONF_SLOT]);

	peer = ovpn_peer_lookup_id(ovpn, peer_id);
	if (!peer)
		return -ENOENT;

	ovpn_crypto_key_slot_delete(&peer->crypto, slot);
	ovpn_peer_put(peer);

	return 0;
}

static int ovpn_nl_swap_keys(struct sk_buff *skb, struct genl_info *info)
{
	struct nlattr *attrs[NUM_OVPN_A_PEER];
	struct ovpn_struct *ovpn = info->user_ptr[0];
	struct ovpn_peer *peer;
	u32 peer_id;
	int ret;

	if (!info->attrs[OVPN_A_PEER])
		return -EINVAL;

	ret = nla_parse_nested(attrs, NUM_OVPN_A_PEER - 1, info->attrs[OVPN_A_PEER],
			       NULL, info->extack);
	if (ret)
		return ret;

	if (!attrs[OVPN_A_PEER_ID])
		return -EINVAL;

	peer_id = nla_get_u32(attrs[OVPN_A_PEER_ID]);

	peer = ovpn_peer_lookup_id(ovpn, peer_id);
	if (!peer)
		return -ENOENT;

	ovpn_crypto_key_slots_swap(&peer->crypto);
	ovpn_peer_put(peer);

	return 0;
}

static int ovpn_nl_new_iface(struct sk_buff *skb, struct genl_info *info)
{
	enum ovpn_mode mode = OVPN_MODE_P2P;
	struct net_device *dev;
	char *ifname;
	int ret;

	if (!info->attrs[OVPN_A_IFNAME])
		return -EINVAL;

	ifname = nla_data(info->attrs[OVPN_A_IFNAME]);

	if (info->attrs[OVPN_A_MODE]) {
		mode = nla_get_u8(info->attrs[OVPN_A_MODE]);
		netdev_dbg(dev, "%s: setting device (%s) mode: %u\n", __func__, ifname,
			   mode);
	}

	ret = ovpn_iface_create(ifname, mode, genl_info_net(info));
	if (ret < 0)
		netdev_dbg(dev, "error while creating interface %s: %d\n", ifname, ret);

	return ret;
}

static int ovpn_nl_del_iface(struct sk_buff *skb, struct genl_info *info)
{
	struct ovpn_struct *ovpn = info->user_ptr[0];

	rtnl_lock();
	ovpn_iface_destruct(ovpn, true);
	dev_put(ovpn->dev);
	rtnl_unlock();

	/* we set the user_ptr to NULL to prevent post_doit from releasing it again */
	info->user_ptr[0] = NULL;

	return 0;
}

static const struct genl_small_ops ovpn_nl_ops[] = {
	{
		.cmd = OVPN_CMD_NEW_IFACE,
		.flags = GENL_ADMIN_PERM | GENL_CMD_CAP_DO,
		.doit = ovpn_nl_new_iface,
	},
	{
		.cmd = OVPN_CMD_DEL_IFACE,
		.flags = GENL_ADMIN_PERM | GENL_CMD_CAP_DO,
		.doit = ovpn_nl_del_iface,
	},
	{
		.cmd = OVPN_CMD_SET_PEER,
		.flags = GENL_ADMIN_PERM | GENL_CMD_CAP_DO,
		.doit = ovpn_nl_set_peer,
	},
	{
		.cmd = OVPN_CMD_DEL_PEER,
		.flags = GENL_ADMIN_PERM | GENL_CMD_CAP_DO,
		.doit = ovpn_nl_del_peer,
	},
	{
		.cmd = OVPN_CMD_GET_PEER,
		.flags = GENL_ADMIN_PERM | GENL_CMD_CAP_DO | GENL_CMD_CAP_DUMP,
		.doit = ovpn_nl_get_peer,
		.dumpit = ovpn_nl_dump_peers,
	},
	{
		.cmd = OVPN_CMD_SET_KEY,
		.flags = GENL_ADMIN_PERM | GENL_CMD_CAP_DO,
		.doit = ovpn_nl_set_key,
	},
	{
		.cmd = OVPN_CMD_DEL_KEY,
		.flags = GENL_ADMIN_PERM | GENL_CMD_CAP_DO,
		.doit = ovpn_nl_del_key,
	},
	{
		.cmd = OVPN_CMD_SWAP_KEYS,
		.flags = GENL_ADMIN_PERM | GENL_CMD_CAP_DO,
		.doit = ovpn_nl_swap_keys,
	},
};

static struct genl_family ovpn_nl_family __ro_after_init = {
	.hdrsize = 0,
	.name = OVPN_NL_NAME,
	.version = 1,
	.maxattr = NUM_OVPN_A + 1,
	.policy = ovpn_nl_policy,
	.netnsok = true,
	.pre_doit = ovpn_pre_doit,
	.post_doit = ovpn_post_doit,
	.module = THIS_MODULE,
	.small_ops = ovpn_nl_ops,
	.n_small_ops = ARRAY_SIZE(ovpn_nl_ops),
	.mcgrps = ovpn_nl_mcgrps,
	.n_mcgrps = ARRAY_SIZE(ovpn_nl_mcgrps),
};

int ovpn_nl_notify_swap_keys(struct ovpn_peer *peer)
{
	struct sk_buff *msg;
	void *hdr;
	int ret;

	netdev_info(peer->ovpn->dev, "peer with id %u must rekey - primary key unusable.\n",
		    peer->id);

	msg = nlmsg_new(100, GFP_KERNEL);
	if (!msg)
		return -ENOMEM;

	hdr = genlmsg_put(msg, 0, 0, &ovpn_nl_family, 0,
			  OVPN_CMD_SWAP_KEYS);
	if (!hdr) {
		ret = -ENOBUFS;
		goto err_free_msg;
	}

	if (nla_put_u32(msg, OVPN_A_IFINDEX, peer->ovpn->dev->ifindex)) {
		ret = -EMSGSIZE;
		goto err_free_msg;
	}

	if (nla_put_u32(msg, OVPN_A_PEER_ID, peer->id)) {
		ret = -EMSGSIZE;
		goto err_free_msg;
	}

	genlmsg_end(msg, hdr);

	genlmsg_multicast_netns(&ovpn_nl_family, dev_net(peer->ovpn->dev),
				msg, 0, OVPN_MCGRP_PEERS, GFP_KERNEL);

	return 0;

err_free_msg:
	nlmsg_free(msg);
	return ret;
}

/**
 * ovpn_nl_notify() - react to openvpn userspace process exit
 */
static int ovpn_nl_notify(struct notifier_block *nb, unsigned long state,
			  void *_notify)
{
	return NOTIFY_DONE;
}

static struct notifier_block ovpn_nl_notifier = {
	.notifier_call = ovpn_nl_notify,
};

/**
 * ovpn_nl_init() - perform any ovpn specific netlink initialization
 */
int ovpn_nl_init(struct ovpn_struct *ovpn)
{
	return 0;
}

/**
 * ovpn_nl_register() - register the ovpn genl nl family
 */
int __init ovpn_nl_register(void)
{
	int ret;

	ret = genl_register_family(&ovpn_nl_family);
	if (ret) {
		pr_err("ovpn: genl_register_family() failed: %d\n", ret);
		return ret;
	}

	ret = netlink_register_notifier(&ovpn_nl_notifier);
	if (ret) {
		pr_err("ovpn: netlink_register_notifier() failed: %d\n", ret);
		goto err;
	}

	return 0;
err:
	genl_unregister_family(&ovpn_nl_family);
	return ret;
}

/**
 * ovpn_nl_unregister() - unregister the ovpn genl netlink family
 */
void ovpn_nl_unregister(void)
{
	netlink_unregister_notifier(&ovpn_nl_notifier);
	genl_unregister_family(&ovpn_nl_family);
}
