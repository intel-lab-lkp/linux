// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2006 Patrick McHardy <kaber@trash.net>
 *
 * Based on ipt_random and ipt_nth by Fabrice MARIE <fabrice@netfilter.org>.
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/init.h>
#include <linux/spinlock.h>
#include <linux/skbuff.h>
#include <linux/net.h>
#include <linux/slab.h>

#include <linux/netfilter/xt_statistic.h>
#include <linux/netfilter/x_tables.h>
#include <linux/module.h>

struct xt_statistic_priv {
	atomic_t count;
	u32 __percpu *cnt_pcpu;
} ____cacheline_aligned_in_smp;

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Patrick McHardy <kaber@trash.net>");
MODULE_DESCRIPTION("Xtables: statistics-based matching (\"Nth\", random)");
MODULE_ALIAS("ipt_statistic");
MODULE_ALIAS("ip6t_statistic");

enum gso_type {
	SKB_GSO_FRAGS_LIST,
	SKB_GSO_FRAGS_ARRAY
};

static int gso_pkt_cnt(const struct sk_buff *skb, enum gso_type *type)
{
	int pkt_cnt = 1;

	if (!skb_is_gso(skb))
		return pkt_cnt;

	/* GSO packets contain many smaller packets. This makes the probability
	 * incorrect, when wanting the probability to be per packet based.
	 */
	if (skb_has_frag_list(skb)) {
		struct sk_buff *iter;

		*type = SKB_GSO_FRAGS_LIST;
		skb_walk_frags(skb, iter)
			pkt_cnt++;
	} else {
		*type = SKB_GSO_FRAGS_ARRAY;
		pkt_cnt += skb_shinfo(skb)->nr_frags;
	}

	return pkt_cnt;
}

static bool
statistic_mt(const struct sk_buff *skb, struct xt_action_param *par)
{
	const struct xt_statistic_info *info = par->matchinfo;
	struct xt_statistic_priv *priv = info->master;
	bool ret = info->flags & XT_STATISTIC_INVERT;
	enum gso_type gso_type;
	bool match = false;
	u32 nval, oval;
	int pkt_cnt;

	switch (info->mode) {
	case XT_STATISTIC_MODE_RANDOM:
		if ((prandom_u32() & 0x7FFFFFFF) < info->u.random.probability)
			ret = !ret;
		break;
	case XT_STATISTIC_MODE_NTH:
		pkt_cnt = gso_pkt_cnt(skb, &gso_type);
		do {
			match = false;
			oval = this_cpu_read(*priv->cnt_pcpu);
			nval = oval + pkt_cnt;
			if (nval > info->u.nth.every) {
				match = true;
				nval = nval - info->u.nth.every - 1;
				nval = min(nval, info->u.nth.every);
			}
		} while (this_cpu_cmpxchg(*priv->cnt_pcpu, oval, nval) != oval);
		if (match)
			ret = !ret;
		break;
	case XT_STATISTIC_MODE_NTH_ATOMIC:
		pkt_cnt = gso_pkt_cnt(skb, &gso_type);
		do {
			match = false;
			oval = atomic_read(&priv->count);
			nval = oval + pkt_cnt;
			if (nval > info->u.nth.every) {
				match = true;
				nval = nval - info->u.nth.every - 1;
				nval = min(nval, info->u.nth.every);
			}
		} while (atomic_cmpxchg(&priv->count, oval, nval) != oval);
		if (match)
			ret = !ret;
		break;
	}

	if (match)
		pr_info("debug XXX: SKB is GRO type:%d contains %d packets\n",
			gso_type, pkt_cnt);

	return ret;
}

static int statistic_mt_check(const struct xt_mtchk_param *par)
{
	struct xt_statistic_info *info = par->matchinfo;
	struct xt_statistic_priv *priv;
	u32 *this_cpu;
	u32 nth_count;
	int cpu;

	if (info->mode > XT_STATISTIC_MODE_MAX ||
	    info->flags & ~XT_STATISTIC_MASK)
		return -EINVAL;

	info->master = kzalloc(sizeof(*info->master), GFP_KERNEL);
	if (info->master == NULL)
		return -ENOMEM;
	priv = info->master;

	priv->cnt_pcpu = alloc_percpu(u32);
	if (!priv->cnt_pcpu) {
		kfree(priv);
		return -ENOMEM;
	}

	/* Userspace specifies start nth.count value */
	nth_count = info->u.nth.count;
	for_each_possible_cpu(cpu) {
		this_cpu = per_cpu_ptr(priv->cnt_pcpu, cpu);
		(*this_cpu) = nth_count;
	}
	atomic_set(&priv->count, nth_count);

	return 0;
}

static void statistic_mt_destroy(const struct xt_mtdtor_param *par)
{
	const struct xt_statistic_info *info = par->matchinfo;

	free_percpu(info->master->cnt_pcpu);
	kfree(info->master);
}

static struct xt_match xt_statistic_mt_reg __read_mostly = {
	.name       = "statistic",
	.revision   = 0,
	.family     = NFPROTO_UNSPEC,
	.match      = statistic_mt,
	.checkentry = statistic_mt_check,
	.destroy    = statistic_mt_destroy,
	.matchsize  = sizeof(struct xt_statistic_info),
	.usersize   = offsetof(struct xt_statistic_info, master),
	.me         = THIS_MODULE,
};

static int __init statistic_mt_init(void)
{
	pr_info("module init\n");
	return xt_register_match(&xt_statistic_mt_reg);
}

static void __exit statistic_mt_exit(void)
{
	pr_info("module exit\n");
	xt_unregister_match(&xt_statistic_mt_reg);
}

module_init(statistic_mt_init);
module_exit(statistic_mt_exit);
