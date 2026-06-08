// SPDX-License-Identifier: GPL-2.0
/* Copyright 2011-2014 Autronica Fire and Security AS
 *
 * Author(s):
 *	2011-2014 Arvid Brodin, arvid.brodin@alten.se
 *
 * Routines for handling Netlink messages for HSR and PRP.
 */

#include "hsr_netlink.h"
#include <linux/kernel.h>
#include <net/rtnetlink.h>
#include <net/genetlink.h>
#include <uapi/linux/if_link.h>
#include <uapi/linux/hsr_netlink.h>
#include "hsr_main.h"
#include "hsr_device.h"
#include "hsr_framereg.h"

static const struct nla_policy hsr_policy[IFLA_HSR_MAX + 1] = {
	[IFLA_HSR_SLAVE1]		= { .type = NLA_U32 },
	[IFLA_HSR_SLAVE2]		= { .type = NLA_U32 },
	[IFLA_HSR_MULTICAST_SPEC]	= { .type = NLA_U8 },
	[IFLA_HSR_VERSION]	= { .type = NLA_U8 },
	[IFLA_HSR_SUPERVISION_ADDR]	= { .len = ETH_ALEN },
	[IFLA_HSR_SEQ_NR]		= { .type = NLA_U16 },
	[IFLA_HSR_PROTOCOL]		= { .type = NLA_U8 },
	[IFLA_HSR_INTERLINK]		= { .type = NLA_U32 },
};

/* Here, it seems a netdevice has already been allocated for us, and the
 * hsr_dev_setup routine has been executed. Nice!
 */
static int hsr_newlink(struct net_device *dev,
		       struct rtnl_newlink_params *params,
		       struct netlink_ext_ack *extack)
{
	struct net *link_net = rtnl_newlink_link_net(params);
	struct net_device *link[2], *interlink = NULL;
	struct nlattr **data = params->data;
	enum hsr_version proto_version;
	unsigned char multicast_spec;
	u8 proto = HSR_PROTOCOL_HSR;

	if (!net_eq(link_net, dev_net(dev))) {
		NL_SET_ERR_MSG_MOD(extack,
				   "HSR slaves/interlink must be on the same net namespace than HSR link");
		return -EINVAL;
	}

	if (!data) {
		NL_SET_ERR_MSG_MOD(extack, "No slave devices specified");
		return -EINVAL;
	}
	if (!data[IFLA_HSR_SLAVE1]) {
		NL_SET_ERR_MSG_MOD(extack, "Slave1 device not specified");
		return -EINVAL;
	}
	link[0] = __dev_get_by_index(link_net,
				     nla_get_u32(data[IFLA_HSR_SLAVE1]));
	if (!link[0]) {
		NL_SET_ERR_MSG_MOD(extack, "Slave1 does not exist");
		return -EINVAL;
	}
	if (!data[IFLA_HSR_SLAVE2]) {
		NL_SET_ERR_MSG_MOD(extack, "Slave2 device not specified");
		return -EINVAL;
	}
	link[1] = __dev_get_by_index(link_net,
				     nla_get_u32(data[IFLA_HSR_SLAVE2]));
	if (!link[1]) {
		NL_SET_ERR_MSG_MOD(extack, "Slave2 does not exist");
		return -EINVAL;
	}

	if (link[0] == link[1]) {
		NL_SET_ERR_MSG_MOD(extack, "Slave1 and Slave2 are same");
		return -EINVAL;
	}

	if (data[IFLA_HSR_INTERLINK])
		interlink = __dev_get_by_index(link_net,
					       nla_get_u32(data[IFLA_HSR_INTERLINK]));

	if (interlink && interlink == link[0]) {
		NL_SET_ERR_MSG_MOD(extack, "Interlink and Slave1 are the same");
		return -EINVAL;
	}

	if (interlink && interlink == link[1]) {
		NL_SET_ERR_MSG_MOD(extack, "Interlink and Slave2 are the same");
		return -EINVAL;
	}

	multicast_spec = nla_get_u8_default(data[IFLA_HSR_MULTICAST_SPEC], 0);

	if (data[IFLA_HSR_PROTOCOL])
		proto = nla_get_u8(data[IFLA_HSR_PROTOCOL]);

	if (proto >= HSR_PROTOCOL_MAX) {
		NL_SET_ERR_MSG_MOD(extack, "Unsupported protocol");
		return -EINVAL;
	}

	if (!data[IFLA_HSR_VERSION]) {
		proto_version = HSR_V0;
	} else {
		if (proto == HSR_PROTOCOL_PRP) {
			NL_SET_ERR_MSG_MOD(extack, "PRP version unsupported");
			return -EINVAL;
		}

		proto_version = nla_get_u8(data[IFLA_HSR_VERSION]);
		if (proto_version > HSR_V1) {
			NL_SET_ERR_MSG_MOD(extack,
					   "Only HSR version 0/1 supported");
			return -EINVAL;
		}
	}

	if (proto == HSR_PROTOCOL_PRP) {
		proto_version = PRP_V1;
		if (interlink) {
			NL_SET_ERR_MSG_MOD(extack,
					   "Interlink only works with HSR");
			return -EINVAL;
		}
	}

	return hsr_dev_finalize(dev, link, interlink, multicast_spec,
				proto_version, extack);
}

static void hsr_dellink(struct net_device *dev, struct list_head *head)
{
	struct hsr_priv *hsr = netdev_priv(dev);

	timer_delete_sync(&hsr->prune_timer);
	timer_delete_sync(&hsr->prune_proxy_timer);
	timer_delete_sync(&hsr->announce_timer);
	timer_delete_sync(&hsr->announce_proxy_timer);

	hsr_debugfs_term(hsr);
	hsr_del_ports(hsr);

	hsr_del_self_node(hsr);
	hsr_del_nodes(&hsr->node_db);
	hsr_del_nodes(&hsr->proxy_node_db);

	unregister_netdevice_queue(dev, head);
}

static int hsr_fill_info(struct sk_buff *skb, const struct net_device *dev)
{
	struct hsr_priv *hsr = netdev_priv(dev);
	u8 proto = HSR_PROTOCOL_HSR;
	struct hsr_port *port;

	port = hsr_port_get_hsr(hsr, HSR_PT_SLAVE_A);
	if (port) {
		if (nla_put_u32(skb, IFLA_HSR_SLAVE1, port->dev->ifindex))
			goto nla_put_failure;
	}

	port = hsr_port_get_hsr(hsr, HSR_PT_SLAVE_B);
	if (port) {
		if (nla_put_u32(skb, IFLA_HSR_SLAVE2, port->dev->ifindex))
			goto nla_put_failure;
	}

	port = hsr_port_get_hsr(hsr, HSR_PT_INTERLINK);
	if (port) {
		if (nla_put_u32(skb, IFLA_HSR_INTERLINK, port->dev->ifindex))
			goto nla_put_failure;
	}

	if (nla_put(skb, IFLA_HSR_SUPERVISION_ADDR, ETH_ALEN,
		    hsr->sup_multicast_addr) ||
	    nla_put_u16(skb, IFLA_HSR_SEQ_NR, hsr->sequence_nr))
		goto nla_put_failure;
	if (hsr->prot_version == PRP_V1)
		proto = HSR_PROTOCOL_PRP;
	else if (nla_put_u8(skb, IFLA_HSR_VERSION, hsr->prot_version))
		goto nla_put_failure;
	if (nla_put_u8(skb, IFLA_HSR_PROTOCOL, proto))
		goto nla_put_failure;

	return 0;

nla_put_failure:
	return -EMSGSIZE;
}

/*
 * Number of real HSR_XSTATS_* u64 counter attributes.
 * Real counters run from HSR_XSTATS_CNT_TX_A(1) through
 * HSR_XSTATS_CNT_OWN_RX_B(25); HSR_XSTATS_PAD is not a counter.
 */
#define HSR_XSTATS_CNT_ATTRS (HSR_XSTATS_PAD - 1)

static size_t hsr_get_linkxstats_size(const struct net_device *dev, int attr)
{
	if (attr != IFLA_STATS_LINK_XSTATS)
		return 0;

	/* Nest header (LINK_XSTATS_TYPE_HSR) + one u64 nla per counter */
	return nla_total_size(0) +
	       HSR_XSTATS_CNT_ATTRS * nla_total_size_64bit(sizeof(u64));
}

/* Put a u64 counter attribute; skip if value is ~0ULL (unsupported). */
static int hsr_put_stat(struct sk_buff *skb, int attr_id, u64 val)
{
	if (val == ~0ULL)
		return 0;
	return nla_put_u64_64bit(skb, attr_id, val, HSR_XSTATS_PAD);
}

static int hsr_fill_linkxstats(struct sk_buff *skb,
			       const struct net_device *dev,
			       int *prividx, int attr)
{
	struct hsr_lre_stats stats;
	struct hsr_port *port;
	struct hsr_priv *hsr = netdev_priv(dev);
	struct nlattr *nest;
	int s_prividx = *prividx;
	int err;

	if (attr != IFLA_STATS_LINK_XSTATS)
		return 0;

	*prividx = 0;

	nest = nla_nest_start_noflag(skb, LINK_XSTATS_TYPE_HSR);
	if (!nest)
		return -EMSGSIZE;

	/* Initialise all counters to ~0ULL ("unsupported") */
	memset(&stats, 0xff, sizeof(stats));

	/* Ask the offload driver (if any) via ndo_get_offload_stats on slave A.
	 * Guard with ndo_has_offload_stats so we only call drivers that
	 * explicitly declare support for IFLA_STATS_LINK_XSTATS, avoiding
	 * spurious -EINVAL from drivers that implement the NDO for a different
	 * attr_id (e.g. IFLA_OFFLOAD_XSTATS_CPU_HIT).
	 */
	port = hsr_port_get_hsr(hsr, HSR_PT_SLAVE_A);
	if (port) {
		const struct net_device_ops *ops = port->dev->netdev_ops;

		if (ops->ndo_has_offload_stats &&
		    ops->ndo_has_offload_stats(port->dev,
					       IFLA_STATS_LINK_XSTATS) &&
		    ops->ndo_get_offload_stats) {
			err = ops->ndo_get_offload_stats(IFLA_STATS_LINK_XSTATS,
							 port->dev, &stats);
			if (err && err != -EOPNOTSUPP) {
				nla_nest_cancel(skb, nest);
				return err;
			}
		}
	}

#define PUT_STAT(attr, field) \
	do { \
		if (HSR_XSTATS_##attr < s_prividx) \
			break; \
		if (hsr_put_stat(skb, HSR_XSTATS_##attr, stats.field)) { \
			*prividx = HSR_XSTATS_##attr; \
			nla_nest_end(skb, nest); \
			return -EMSGSIZE; \
		} \
	} while (0)

	PUT_STAT(CNT_TX_A,		cnt_tx_a);
	PUT_STAT(CNT_TX_B,		cnt_tx_b);
	PUT_STAT(CNT_TX_C,		cnt_tx_c);
	PUT_STAT(CNT_RX_A,		cnt_rx_a);
	PUT_STAT(CNT_RX_B,		cnt_rx_b);
	PUT_STAT(CNT_RX_C,		cnt_rx_c);
	PUT_STAT(CNT_ERR_WRONG_LAN_A,	cnt_err_wrong_lan_a);
	PUT_STAT(CNT_ERR_WRONG_LAN_B,	cnt_err_wrong_lan_b);
	PUT_STAT(CNT_ERR_WRONG_LAN_C,	cnt_err_wrong_lan_c);
	PUT_STAT(CNT_ERRORS_A,		cnt_errors_a);
	PUT_STAT(CNT_ERRORS_B,		cnt_errors_b);
	PUT_STAT(CNT_ERRORS_C,		cnt_errors_c);
	PUT_STAT(CNT_UNIQUE_A,		cnt_unique_a);
	PUT_STAT(CNT_UNIQUE_B,		cnt_unique_b);
	PUT_STAT(CNT_UNIQUE_C,		cnt_unique_c);
	PUT_STAT(CNT_DUPLICATE_A,	cnt_duplicate_a);
	PUT_STAT(CNT_DUPLICATE_B,	cnt_duplicate_b);
	PUT_STAT(CNT_DUPLICATE_C,	cnt_duplicate_c);
	PUT_STAT(CNT_MULTI_A,		cnt_multi_a);
	PUT_STAT(CNT_MULTI_B,		cnt_multi_b);
	PUT_STAT(CNT_MULTI_C,		cnt_multi_c);
	PUT_STAT(CNT_OWN_RX_A,		cnt_own_rx_a);
	PUT_STAT(CNT_OWN_RX_B,		cnt_own_rx_b);

#undef PUT_STAT

	nla_nest_end(skb, nest);
	return 0;
}

static struct rtnl_link_ops hsr_link_ops __read_mostly = {
	.kind			= "hsr",
	.maxtype		= IFLA_HSR_MAX,
	.policy			= hsr_policy,
	.priv_size		= sizeof(struct hsr_priv),
	.setup			= hsr_dev_setup,
	.newlink		= hsr_newlink,
	.dellink		= hsr_dellink,
	.fill_info		= hsr_fill_info,
	.get_linkxstats_size	= hsr_get_linkxstats_size,
	.fill_linkxstats	= hsr_fill_linkxstats,
};

/* attribute policy */
static const struct nla_policy hsr_genl_policy[HSR_A_MAX + 1] = {
	[HSR_A_NODE_ADDR] = { .len = ETH_ALEN },
	[HSR_A_NODE_ADDR_B] = { .len = ETH_ALEN },
	[HSR_A_IFINDEX] = { .type = NLA_U32 },
	[HSR_A_IF1_AGE] = { .type = NLA_U32 },
	[HSR_A_IF2_AGE] = { .type = NLA_U32 },
	[HSR_A_IF1_SEQ] = { .type = NLA_U16 },
	[HSR_A_IF2_SEQ] = { .type = NLA_U16 },
};

static struct genl_family hsr_genl_family;

static const struct genl_multicast_group hsr_mcgrps[] = {
	{ .name = "hsr-network", },
};

/* This is called if for some node with MAC address addr, we only get frames
 * over one of the slave interfaces. This would indicate an open network ring
 * (i.e. a link has failed somewhere).
 */
void hsr_nl_ringerror(struct hsr_priv *hsr, unsigned char addr[ETH_ALEN],
		      struct hsr_port *port)
{
	struct sk_buff *skb;
	void *msg_head;
	struct hsr_port *master;
	int res;

	skb = genlmsg_new(NLMSG_GOODSIZE, GFP_ATOMIC);
	if (!skb)
		goto fail;

	msg_head = genlmsg_put(skb, 0, 0, &hsr_genl_family, 0,
			       HSR_C_RING_ERROR);
	if (!msg_head)
		goto nla_put_failure;

	res = nla_put(skb, HSR_A_NODE_ADDR, ETH_ALEN, addr);
	if (res < 0)
		goto nla_put_failure;

	res = nla_put_u32(skb, HSR_A_IFINDEX, port->dev->ifindex);
	if (res < 0)
		goto nla_put_failure;

	genlmsg_end(skb, msg_head);
	genlmsg_multicast(&hsr_genl_family, skb, 0, 0, GFP_ATOMIC);

	return;

nla_put_failure:
	kfree_skb(skb);

fail:
	rcu_read_lock();
	master = hsr_port_get_hsr(hsr, HSR_PT_MASTER);
	netdev_warn(master->dev, "Could not send HSR ring error message\n");
	rcu_read_unlock();
}

/* This is called when we haven't heard from the node with MAC address addr for
 * some time (just before the node is removed from the node table/list).
 */
void hsr_nl_nodedown(struct hsr_priv *hsr, unsigned char addr[ETH_ALEN])
{
	struct sk_buff *skb;
	void *msg_head;
	struct hsr_port *master;
	int res;

	skb = genlmsg_new(NLMSG_GOODSIZE, GFP_ATOMIC);
	if (!skb)
		goto fail;

	msg_head = genlmsg_put(skb, 0, 0, &hsr_genl_family, 0, HSR_C_NODE_DOWN);
	if (!msg_head)
		goto nla_put_failure;

	res = nla_put(skb, HSR_A_NODE_ADDR, ETH_ALEN, addr);
	if (res < 0)
		goto nla_put_failure;

	genlmsg_end(skb, msg_head);
	genlmsg_multicast(&hsr_genl_family, skb, 0, 0, GFP_ATOMIC);

	return;

nla_put_failure:
	kfree_skb(skb);

fail:
	rcu_read_lock();
	master = hsr_port_get_hsr(hsr, HSR_PT_MASTER);
	netdev_warn(master->dev, "Could not send HSR node down\n");
	rcu_read_unlock();
}

/* HSR_C_GET_NODE_STATUS lets userspace query the internal HSR node table
 * about the status of a specific node in the network, defined by its MAC
 * address.
 *
 * Input: hsr ifindex, node mac address
 * Output: hsr ifindex, node mac address (copied from request),
 *	   age of latest frame from node over slave 1, slave 2 [ms]
 */
static int hsr_get_node_status(struct sk_buff *skb_in, struct genl_info *info)
{
	/* For receiving */
	struct nlattr *na;
	struct net_device *hsr_dev;

	/* For sending */
	struct sk_buff *skb_out;
	void *msg_head;
	struct hsr_priv *hsr;
	struct hsr_port *port;
	unsigned char hsr_node_addr_b[ETH_ALEN];
	int hsr_node_if1_age;
	u16 hsr_node_if1_seq;
	int hsr_node_if2_age;
	u16 hsr_node_if2_seq;
	int addr_b_ifindex;
	int res;

	if (!info)
		goto invalid;

	na = info->attrs[HSR_A_IFINDEX];
	if (!na)
		goto invalid;
	na = info->attrs[HSR_A_NODE_ADDR];
	if (!na)
		goto invalid;

	rcu_read_lock();
	hsr_dev = dev_get_by_index_rcu(genl_info_net(info),
				       nla_get_u32(info->attrs[HSR_A_IFINDEX]));
	if (!hsr_dev)
		goto rcu_unlock;
	if (!is_hsr_master(hsr_dev))
		goto rcu_unlock;

	/* Send reply */
	skb_out = genlmsg_new(NLMSG_GOODSIZE, GFP_ATOMIC);
	if (!skb_out) {
		res = -ENOMEM;
		goto fail;
	}

	msg_head = genlmsg_put(skb_out, NETLINK_CB(skb_in).portid,
			       info->snd_seq, &hsr_genl_family, 0,
			       HSR_C_SET_NODE_STATUS);
	if (!msg_head) {
		res = -ENOMEM;
		goto nla_put_failure;
	}

	res = nla_put_u32(skb_out, HSR_A_IFINDEX, hsr_dev->ifindex);
	if (res < 0)
		goto nla_put_failure;

	hsr = netdev_priv(hsr_dev);
	res = hsr_get_node_data(hsr,
				(unsigned char *)
				nla_data(info->attrs[HSR_A_NODE_ADDR]),
					 hsr_node_addr_b,
					 &addr_b_ifindex,
					 &hsr_node_if1_age,
					 &hsr_node_if1_seq,
					 &hsr_node_if2_age,
					 &hsr_node_if2_seq);
	if (res < 0)
		goto nla_put_failure;

	res = nla_put(skb_out, HSR_A_NODE_ADDR, ETH_ALEN,
		      nla_data(info->attrs[HSR_A_NODE_ADDR]));
	if (res < 0)
		goto nla_put_failure;

	if (addr_b_ifindex > -1) {
		res = nla_put(skb_out, HSR_A_NODE_ADDR_B, ETH_ALEN,
			      hsr_node_addr_b);
		if (res < 0)
			goto nla_put_failure;

		res = nla_put_u32(skb_out, HSR_A_ADDR_B_IFINDEX,
				  addr_b_ifindex);
		if (res < 0)
			goto nla_put_failure;
	}

	res = nla_put_u32(skb_out, HSR_A_IF1_AGE, hsr_node_if1_age);
	if (res < 0)
		goto nla_put_failure;
	res = nla_put_u16(skb_out, HSR_A_IF1_SEQ, hsr_node_if1_seq);
	if (res < 0)
		goto nla_put_failure;
	port = hsr_port_get_hsr(hsr, HSR_PT_SLAVE_A);
	if (port)
		res = nla_put_u32(skb_out, HSR_A_IF1_IFINDEX,
				  port->dev->ifindex);
	if (res < 0)
		goto nla_put_failure;

	res = nla_put_u32(skb_out, HSR_A_IF2_AGE, hsr_node_if2_age);
	if (res < 0)
		goto nla_put_failure;
	res = nla_put_u16(skb_out, HSR_A_IF2_SEQ, hsr_node_if2_seq);
	if (res < 0)
		goto nla_put_failure;
	port = hsr_port_get_hsr(hsr, HSR_PT_SLAVE_B);
	if (port)
		res = nla_put_u32(skb_out, HSR_A_IF2_IFINDEX,
				  port->dev->ifindex);
	if (res < 0)
		goto nla_put_failure;

	rcu_read_unlock();

	genlmsg_end(skb_out, msg_head);
	genlmsg_unicast(genl_info_net(info), skb_out, info->snd_portid);

	return 0;

rcu_unlock:
	rcu_read_unlock();
invalid:
	netlink_ack(skb_in, nlmsg_hdr(skb_in), -EINVAL, NULL);
	return 0;

nla_put_failure:
	kfree_skb(skb_out);
	/* Fall through */

fail:
	rcu_read_unlock();
	return res;
}

/* Get a list of MacAddressA of all nodes known to this node (including self).
 */
static int hsr_get_node_list(struct sk_buff *skb_in, struct genl_info *info)
{
	unsigned char addr[ETH_ALEN];
	struct net_device *hsr_dev;
	struct sk_buff *skb_out;
	struct hsr_priv *hsr;
	bool restart = false;
	struct nlattr *na;
	void *pos = NULL;
	void *msg_head;
	int res;

	if (!info)
		goto invalid;

	na = info->attrs[HSR_A_IFINDEX];
	if (!na)
		goto invalid;

	rcu_read_lock();
	hsr_dev = dev_get_by_index_rcu(genl_info_net(info),
				       nla_get_u32(info->attrs[HSR_A_IFINDEX]));
	if (!hsr_dev)
		goto rcu_unlock;
	if (!is_hsr_master(hsr_dev))
		goto rcu_unlock;

restart:
	/* Send reply */
	skb_out = genlmsg_new(GENLMSG_DEFAULT_SIZE, GFP_ATOMIC);
	if (!skb_out) {
		res = -ENOMEM;
		goto fail;
	}

	msg_head = genlmsg_put(skb_out, NETLINK_CB(skb_in).portid,
			       info->snd_seq, &hsr_genl_family, 0,
			       HSR_C_SET_NODE_LIST);
	if (!msg_head) {
		res = -ENOMEM;
		goto nla_put_failure;
	}

	if (!restart) {
		res = nla_put_u32(skb_out, HSR_A_IFINDEX, hsr_dev->ifindex);
		if (res < 0)
			goto nla_put_failure;
	}

	hsr = netdev_priv(hsr_dev);

	if (!pos)
		pos = hsr_get_next_node(hsr, NULL, addr);
	while (pos) {
		res = nla_put(skb_out, HSR_A_NODE_ADDR, ETH_ALEN, addr);
		if (res < 0) {
			if (res == -EMSGSIZE) {
				genlmsg_end(skb_out, msg_head);
				genlmsg_unicast(genl_info_net(info), skb_out,
						info->snd_portid);
				restart = true;
				goto restart;
			}
			goto nla_put_failure;
		}
		pos = hsr_get_next_node(hsr, pos, addr);
	}
	rcu_read_unlock();

	genlmsg_end(skb_out, msg_head);
	genlmsg_unicast(genl_info_net(info), skb_out, info->snd_portid);

	return 0;

rcu_unlock:
	rcu_read_unlock();
invalid:
	netlink_ack(skb_in, nlmsg_hdr(skb_in), -EINVAL, NULL);
	return 0;

nla_put_failure:
	nlmsg_free(skb_out);
	/* Fall through */

fail:
	rcu_read_unlock();
	return res;
}

static const struct genl_small_ops hsr_ops[] = {
	{
		.cmd = HSR_C_GET_NODE_STATUS,
		.validate = GENL_DONT_VALIDATE_STRICT | GENL_DONT_VALIDATE_DUMP,
		.flags = 0,
		.doit = hsr_get_node_status,
		.dumpit = NULL,
	},
	{
		.cmd = HSR_C_GET_NODE_LIST,
		.validate = GENL_DONT_VALIDATE_STRICT | GENL_DONT_VALIDATE_DUMP,
		.flags = 0,
		.doit = hsr_get_node_list,
		.dumpit = NULL,
	},
};

static struct genl_family hsr_genl_family __ro_after_init = {
	.hdrsize = 0,
	.name = "HSR",
	.version = 1,
	.maxattr = HSR_A_MAX,
	.policy = hsr_genl_policy,
	.netnsok = true,
	.module = THIS_MODULE,
	.small_ops = hsr_ops,
	.n_small_ops = ARRAY_SIZE(hsr_ops),
	.resv_start_op = HSR_C_SET_NODE_LIST + 1,
	.mcgrps = hsr_mcgrps,
	.n_mcgrps = ARRAY_SIZE(hsr_mcgrps),
};

int __init hsr_netlink_init(void)
{
	int rc;

	rc = rtnl_link_register(&hsr_link_ops);
	if (rc)
		goto fail_rtnl_link_register;

	rc = genl_register_family(&hsr_genl_family);
	if (rc)
		goto fail_genl_register_family;

	hsr_debugfs_create_root();
	return 0;

fail_genl_register_family:
	rtnl_link_unregister(&hsr_link_ops);
fail_rtnl_link_register:

	return rc;
}

void __exit hsr_netlink_exit(void)
{
	genl_unregister_family(&hsr_genl_family);
	rtnl_link_unregister(&hsr_link_ops);
}

MODULE_ALIAS_RTNL_LINK("hsr");
