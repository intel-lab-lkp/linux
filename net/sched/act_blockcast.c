// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * net/sched/act_blockcast.c	Block Cast action
 * Copyright (c) 2023, Mojatatu Networks
 * Authors:     Jamal Hadi Salim <jhs@mojatatu.com>
 *              Victor Nogueira <victor@mojatatu.com>
 *              Pedro Tammela <pctammela@mojatatu.com>
 */

#include <linux/module.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/skbuff.h>
#include <linux/rtnetlink.h>
#include <net/netlink.h>
#include <net/pkt_sched.h>
#include <net/pkt_cls.h>
#include <linux/if_arp.h>
#include <net/tc_wrapper.h>

#include <linux/tc_act/tc_defact.h>

static struct tc_action_ops act_blockcast_ops;

struct tcf_blockcast_act {
	struct tc_action	common;
};

#define to_blockcast_act(a) ((struct tcf_blockcast_act *)a)

#define TCA_ID_BLOCKCAST 123
#define CAST_RECURSION_LIMIT  4

static DEFINE_PER_CPU(unsigned int, redirect_rec_level);

static int cast_one(struct sk_buff *skb, const u32 ifindex)
{
	struct sk_buff *skb2 = skb;
	int retval = TC_ACT_PIPE;
	struct net_device *dev;
	unsigned int rec_level;
	bool expects_nh;
	int mac_len;
	bool at_nh;
	int err;

	rec_level = __this_cpu_inc_return(redirect_rec_level);
	if (unlikely(rec_level > CAST_RECURSION_LIMIT)) {
		net_warn_ratelimited("blockcast: exceeded redirect recursion limit on dev %s\n",
				     netdev_name(skb->dev));
		__this_cpu_dec(redirect_rec_level);
		return TC_ACT_SHOT;
	}

	dev = dev_get_by_index_rcu(dev_net(skb->dev), ifindex);
	if (unlikely(!dev)) {
		__this_cpu_dec(redirect_rec_level);
		return TC_ACT_SHOT;
	}

	if (unlikely(!(dev->flags & IFF_UP) || !netif_carrier_ok(dev))) {
		net_notice_ratelimited("blockcast: device %s is down\n",
				       dev->name);
		__this_cpu_dec(redirect_rec_level);
		return TC_ACT_SHOT;
	}

	skb2 = skb_clone(skb, GFP_ATOMIC);
	if (!skb2) {
		__this_cpu_dec(redirect_rec_level);
		return retval;
	}

	nf_reset_ct(skb2);

	expects_nh = !dev_is_mac_header_xmit(dev);
	at_nh = skb->data == skb_network_header(skb);
	if (at_nh != expects_nh) {
		mac_len = skb_at_tc_ingress(skb) ?
				  skb->mac_len :
				  skb_network_header(skb) - skb_mac_header(skb);

		if (expects_nh) {
			/* target device/action expect data at nh */
			skb_pull_rcsum(skb2, mac_len);
		} else {
			/* target device/action expect data at mac */
			skb_push_rcsum(skb2, mac_len);
		}
	}

	skb2->skb_iif = skb->dev->ifindex;
	skb2->dev = dev;

	err = dev_queue_xmit(skb2);
	if (err)
		retval = TC_ACT_SHOT;

	__this_cpu_dec(redirect_rec_level);

	return retval;
}

TC_INDIRECT_SCOPE int tcf_blockcast_run(struct sk_buff *skb,
					const struct tc_action *a,
					struct tcf_result *res)
{
	u32 block_index = qdisc_skb_cb(skb)->block_index;
	struct tcf_blockcast_act *p = to_blockcast_act(a);
	int action = READ_ONCE(p->tcf_action);
	struct net *net = dev_net(skb->dev);
	struct tcf_block *block;
	struct net_device *dev;
	u32 exception_ifindex;
	unsigned long index;

	block = tcf_block_lookup(net, block_index);
	exception_ifindex = skb->dev->ifindex;

	tcf_action_update_bstats(&p->common, skb);
	tcf_lastuse_update(&p->tcf_tm);

	if (!block || xa_empty(&block->ports))
		goto act_done;

	/* we are already under rcu protection, so iterating block is safe*/
	xa_for_each(&block->ports, index, dev) {
		int err;

		if (index == exception_ifindex)
			continue;

		err = cast_one(skb, dev->ifindex);
		if (err != TC_ACT_PIPE)
			printk("(%d)Failed to send to dev\t%d: %s\n", err,
			       dev->ifindex, dev->name);
	}

act_done:
	if (action == TC_ACT_SHOT)
		tcf_action_inc_drop_qstats(&p->common);
	return action;
}

static const struct nla_policy blockcast_policy[TCA_DEF_MAX + 1] = {
	[TCA_DEF_PARMS]	= { .len = sizeof(struct tc_defact) },
};

static int tcf_blockcast_init(struct net *net, struct nlattr *nla,
			      struct nlattr *est, struct tc_action **a,
			      struct tcf_proto *tp, u32 flags,
			      struct netlink_ext_ack *extack)
{
	struct tc_action_net *tn = net_generic(net, act_blockcast_ops.net_id);
	struct tcf_blockcast_act *p = to_blockcast_act(a);
	bool bind = flags & TCA_ACT_FLAGS_BIND;
	struct nlattr *tb[TCA_DEF_MAX + 1];
	struct tcf_chain *goto_ch = NULL;
	struct tc_defact *parm;
	bool exists = false;
	int ret = 0, err;
	u32 index;

	if (!nla)
		return -EINVAL;

	err = nla_parse_nested_deprecated(tb, TCA_DEF_MAX, nla,
					  blockcast_policy, NULL);
	if (err < 0)
		return err;

	if (!tb[TCA_DEF_PARMS])
		return -EINVAL;

	parm = nla_data(tb[TCA_DEF_PARMS]);
	index = parm->index;

	err = tcf_idr_check_alloc(tn, &index, a, bind);
	if (err < 0)
		return err;

	exists = err;
	if (exists && bind)
		return 0;

	if (!exists) {
		ret = tcf_idr_create_from_flags(tn, index, est, a,
						&act_blockcast_ops, bind, flags);
		if (ret) {
			tcf_idr_cleanup(tn, index);
			return ret;
		}

		ret = ACT_P_CREATED;
	} else {
		if (!(flags & TCA_ACT_FLAGS_REPLACE)) {
			err = -EEXIST;
			goto release_idr;
		}
	}

	err = tcf_action_check_ctrlact(parm->action, tp, &goto_ch, extack);
	if (err < 0)
		goto release_idr;

	if (exists)
		spin_lock_bh(&p->tcf_lock);
	goto_ch = tcf_action_set_ctrlact(*a, parm->action, goto_ch);
	if (exists)
		spin_unlock_bh(&p->tcf_lock);

	if (goto_ch)
		tcf_chain_put_by_act(goto_ch);

	return ret;
release_idr:
	tcf_idr_release(*a, bind);
	return err;
}

static int tcf_blockcast_dump(struct sk_buff *skb, struct tc_action *a,
			      int bind, int ref)
{
	unsigned char *b = skb_tail_pointer(skb);
	struct tcf_blockcast_act *p = to_blockcast_act(a);
	struct tc_defact opt = {
		.index   = p->tcf_index,
		.refcnt  = refcount_read(&p->tcf_refcnt) - ref,
		.bindcnt = atomic_read(&p->tcf_bindcnt) - bind,
	};
	struct tcf_t t;

	spin_lock_bh(&p->tcf_lock);
	opt.action = p->tcf_action;
	if (nla_put(skb, TCA_DEF_PARMS, sizeof(opt), &opt))
		goto nla_put_failure;

	tcf_tm_dump(&t, &p->tcf_tm);
	if (nla_put_64bit(skb, TCA_DEF_TM, sizeof(t), &t, TCA_DEF_PAD))
		goto nla_put_failure;
	spin_unlock_bh(&p->tcf_lock);

	return skb->len;

nla_put_failure:
	spin_unlock_bh(&p->tcf_lock);
	nlmsg_trim(skb, b);
	return -1;
}

static struct tc_action_ops act_blockcast_ops = {
	.kind		=	"blockcast",
	.id		=	TCA_ID_BLOCKCAST,
	.owner		=	THIS_MODULE,
	.act		=	tcf_blockcast_run,
	.dump		=	tcf_blockcast_dump,
	.init		=	tcf_blockcast_init,
	.size		=	sizeof(struct tcf_blockcast_act),
};

static __net_init int blockcast_init_net(struct net *net)
{
	struct tc_action_net *tn = net_generic(net, act_blockcast_ops.net_id);

	return tc_action_net_init(net, tn, &act_blockcast_ops);
}

static void __net_exit blockcast_exit_net(struct list_head *net_list)
{
	tc_action_net_exit(net_list, act_blockcast_ops.net_id);
}

static struct pernet_operations blockcast_net_ops = {
	.init = blockcast_init_net,
	.exit_batch = blockcast_exit_net,
	.id   = &act_blockcast_ops.net_id,
	.size = sizeof(struct tc_action_net),
};

MODULE_AUTHOR("Mojatatu Networks, Inc");
MODULE_LICENSE("GPL");

static int __init blockcast_init_module(void)
{
	int ret = tcf_register_action(&act_blockcast_ops, &blockcast_net_ops);

	if (!ret)
		pr_info("blockcast TC action Loaded\n");
	return ret;
}

static void __exit blockcast_cleanup_module(void)
{
	tcf_unregister_action(&act_blockcast_ops, &blockcast_net_ops);
}

module_init(blockcast_init_module);
module_exit(blockcast_cleanup_module);
