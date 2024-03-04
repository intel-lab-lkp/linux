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

static const struct genl_small_ops ovpn_nl_ops[] = {
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
