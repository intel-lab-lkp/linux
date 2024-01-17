// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Programmable Qdisc with eBPF
 *
 * Copyright (C) 2022, ByteDance, Cong Wang <cong.wang@bytedance.com>
 */
#include <linux/module.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/jiffies.h>
#include <linux/string.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/skbuff.h>
#include <linux/slab.h>
#include <linux/filter.h>
#include <linux/bpf.h>
#include <linux/btf_ids.h>
#include <net/netlink.h>
#include <net/pkt_sched.h>
#include <net/pkt_cls.h>

#define ACT_BPF_NAME_LEN	256

struct sch_bpf_prog {
	struct bpf_prog *prog;
	const char *name;
};

struct sch_bpf_class {
	struct Qdisc_class_common common;
	struct Qdisc *qdisc;

	unsigned int drops;
	unsigned int overlimits;
	struct gnet_stats_basic_sync bstats;
};

struct bpf_sched_data {
	struct tcf_proto __rcu *filter_list; /* optional external classifier */
	struct tcf_block *block;
	struct Qdisc_class_hash clhash;
	struct sch_bpf_prog __rcu enqueue_prog;
	struct sch_bpf_prog __rcu dequeue_prog;
	struct sch_bpf_prog __rcu reset_prog;

	struct qdisc_watchdog watchdog;
};

static int sch_bpf_dump_prog(const struct sch_bpf_prog *prog, struct sk_buff *skb,
			     int name, int id, int tag)
{
	struct nlattr *nla;

	if (!prog->prog)
		return 0;

	if (prog->name &&
	    nla_put_string(skb, name, prog->name))
		return -EMSGSIZE;

	if (nla_put_u32(skb, id, prog->prog->aux->id))
		return -EMSGSIZE;

	nla = nla_reserve(skb, tag, sizeof(prog->prog->tag));
	if (!nla)
		return -EMSGSIZE;

	memcpy(nla_data(nla), prog->prog->tag, nla_len(nla));
	return 0;
}

static int sch_bpf_dump(struct Qdisc *sch, struct sk_buff *skb)
{
	struct bpf_sched_data *q = qdisc_priv(sch);
	struct nlattr *opts;

	opts = nla_nest_start_noflag(skb, TCA_OPTIONS);
	if (!opts)
		goto nla_put_failure;

	if (sch_bpf_dump_prog(&q->enqueue_prog, skb, TCA_SCH_BPF_ENQUEUE_PROG_NAME,
			      TCA_SCH_BPF_ENQUEUE_PROG_ID, TCA_SCH_BPF_ENQUEUE_PROG_TAG))
		goto nla_put_failure;
	if (sch_bpf_dump_prog(&q->dequeue_prog, skb, TCA_SCH_BPF_DEQUEUE_PROG_NAME,
			      TCA_SCH_BPF_DEQUEUE_PROG_ID, TCA_SCH_BPF_DEQUEUE_PROG_TAG))
		goto nla_put_failure;
	if (sch_bpf_dump_prog(&q->reset_prog, skb, TCA_SCH_BPF_RESET_PROG_NAME,
			      TCA_SCH_BPF_RESET_PROG_ID, TCA_SCH_BPF_RESET_PROG_TAG))
		goto nla_put_failure;

	return nla_nest_end(skb, opts);

nla_put_failure:
	return -1;
}

static int sch_bpf_dump_stats(struct Qdisc *sch, struct gnet_dump *d)
{
	return 0;
}

static struct sch_bpf_class *sch_bpf_find(struct Qdisc *sch, u32 classid)
{
	struct bpf_sched_data *q = qdisc_priv(sch);
	struct Qdisc_class_common *clc;

	clc = qdisc_class_find(&q->clhash, classid);
	if (!clc)
		return NULL;
	return container_of(clc, struct sch_bpf_class, common);
}

static int sch_bpf_enqueue(struct sk_buff *skb, struct Qdisc *sch,
			   struct sk_buff **to_free)
{
	struct bpf_sched_data *q = qdisc_priv(sch);
	unsigned int len = qdisc_pkt_len(skb);
	struct bpf_qdisc_ctx ctx = {};
	int res = NET_XMIT_SUCCESS;
	struct sch_bpf_class *cl;
	struct bpf_prog *enqueue;

	enqueue = rcu_dereference(q->enqueue_prog.prog);
	if (!enqueue)
		return NET_XMIT_DROP;

	ctx.skb = skb;
	ctx.classid = sch->handle;
	res = bpf_prog_run(enqueue, &ctx);
	switch (res) {
	case SCH_BPF_THROTTLE:
		qdisc_watchdog_schedule_range_ns(&q->watchdog, ctx.expire, ctx.delta_ns);
		qdisc_qstats_overlimit(sch);
		fallthrough;
	case SCH_BPF_QUEUED:
		qdisc_qstats_backlog_inc(sch, skb);
		return NET_XMIT_SUCCESS;
	case SCH_BPF_BYPASS:
		qdisc_qstats_drop(sch);
		__qdisc_drop(skb, to_free);
		return NET_XMIT_SUCCESS | __NET_XMIT_BYPASS;
	case SCH_BPF_STOLEN:
		__qdisc_drop(skb, to_free);
		return NET_XMIT_SUCCESS | __NET_XMIT_STOLEN;
	case SCH_BPF_CN:
		return NET_XMIT_CN;
	case SCH_BPF_PASS:
		break;
	default:
		return qdisc_drop(skb, sch, to_free);
	}

	cl = sch_bpf_find(sch, ctx.classid);
	if (!cl || !cl->qdisc)
		return qdisc_drop(skb, sch, to_free);

	res = qdisc_enqueue(skb, cl->qdisc, to_free);
	if (res != NET_XMIT_SUCCESS) {
		if (net_xmit_drop_count(res)) {
			qdisc_qstats_drop(sch);
			cl->drops++;
		}
		return res;
	}

	sch->qstats.backlog += len;
	sch->q.qlen++;
	return res;
}

DEFINE_PER_CPU(struct sk_buff*, bpf_skb_dequeue);

static struct sk_buff *sch_bpf_dequeue(struct Qdisc *sch)
{
	struct bpf_sched_data *q = qdisc_priv(sch);
	struct bpf_qdisc_ctx ctx = {};
	struct sk_buff *skb = NULL;
	struct bpf_prog *dequeue;
	struct sch_bpf_class *cl;
	int res;

	dequeue = rcu_dereference(q->dequeue_prog.prog);
	if (!dequeue)
		return NULL;

	__this_cpu_write(bpf_skb_dequeue, NULL);
	ctx.classid = sch->handle;
	res = bpf_prog_run(dequeue, &ctx);
	switch (res) {
	case SCH_BPF_DEQUEUED:
		skb = __this_cpu_read(bpf_skb_dequeue);
		qdisc_bstats_update(sch, skb);
		qdisc_qstats_backlog_dec(sch, skb);
		break;
	case SCH_BPF_THROTTLE:
		qdisc_watchdog_schedule_range_ns(&q->watchdog, ctx.expire, ctx.delta_ns);
		qdisc_qstats_overlimit(sch);
		cl = sch_bpf_find(sch, ctx.classid);
		if (cl)
			cl->overlimits++;
		return NULL;
	case SCH_BPF_PASS:
		cl = sch_bpf_find(sch, ctx.classid);
		if (!cl || !cl->qdisc)
			return NULL;
		skb = qdisc_dequeue_peeked(cl->qdisc);
		if (skb) {
			bstats_update(&cl->bstats, skb);
			qdisc_bstats_update(sch, skb);
			qdisc_qstats_backlog_dec(sch, skb);
			sch->q.qlen--;
		}
		break;
	}

	return skb;
}

static struct Qdisc *sch_bpf_leaf(struct Qdisc *sch, unsigned long arg)
{
	struct sch_bpf_class *cl = (struct sch_bpf_class *)arg;

	return cl->qdisc;
}

static int sch_bpf_graft(struct Qdisc *sch, unsigned long arg, struct Qdisc *new,
			 struct Qdisc **old, struct netlink_ext_ack *extack)
{
	struct sch_bpf_class *cl = (struct sch_bpf_class *)arg;

	if (new)
		*old = qdisc_replace(sch, new, &cl->qdisc);
	return 0;
}

static unsigned long sch_bpf_bind(struct Qdisc *sch, unsigned long parent,
				  u32 classid)
{
	return 0;
}

static void sch_bpf_unbind(struct Qdisc *q, unsigned long cl)
{
}

static unsigned long sch_bpf_search(struct Qdisc *sch, u32 handle)
{
	return (unsigned long)sch_bpf_find(sch, handle);
}

static struct tcf_block *sch_bpf_tcf_block(struct Qdisc *sch, unsigned long cl,
					   struct netlink_ext_ack *extack)
{
	struct bpf_sched_data *q = qdisc_priv(sch);

	if (cl)
		return NULL;
	return q->block;
}

static const struct nla_policy sch_bpf_policy[TCA_SCH_BPF_MAX + 1] = {
	[TCA_SCH_BPF_ENQUEUE_PROG_FD]	= { .type = NLA_U32 },
	[TCA_SCH_BPF_ENQUEUE_PROG_NAME]	= { .type = NLA_NUL_STRING,
					    .len = ACT_BPF_NAME_LEN },
	[TCA_SCH_BPF_DEQUEUE_PROG_FD]	= { .type = NLA_U32 },
	[TCA_SCH_BPF_DEQUEUE_PROG_NAME]	= { .type = NLA_NUL_STRING,
					    .len = ACT_BPF_NAME_LEN },
	[TCA_SCH_BPF_RESET_PROG_FD]	= { .type = NLA_U32 },
	[TCA_SCH_BPF_RESET_PROG_NAME]	= { .type = NLA_NUL_STRING,
					    .len = ACT_BPF_NAME_LEN },
};

static int bpf_init_prog(struct nlattr *fd, struct nlattr *name,
			 struct sch_bpf_prog *prog, bool optional)
{
	struct bpf_prog *fp, *old_fp;
	char *prog_name = NULL;
	u32 bpf_fd;

	if (!fd)
		return optional ? 0 : -EINVAL;

	bpf_fd = nla_get_u32(fd);

	fp = bpf_prog_get_type(bpf_fd, BPF_PROG_TYPE_QDISC);
	if (IS_ERR(fp))
		return PTR_ERR(fp);

	if (name) {
		prog_name = nla_memdup(name, GFP_KERNEL);
		if (!prog_name) {
			bpf_prog_put(fp);
			return -ENOMEM;
		}
	}

	prog->name = prog_name;

	/* updates to prog->prog are prevent since the caller holds
	 * sch_tree_lock
	 */
	old_fp = rcu_replace_pointer(prog->prog, fp, 1);
	if (old_fp)
		bpf_prog_put(old_fp);

	return 0;
}

static void bpf_cleanup_prog(struct sch_bpf_prog *prog)
{
	struct bpf_prog *old_fp = NULL;

	/* updates to prog->prog are prevent since the caller holds
	 * sch_tree_lock
	 */
	old_fp = rcu_replace_pointer(prog->prog, old_fp, 1);
	if (old_fp)
		bpf_prog_put(old_fp);

	kfree(prog->name);
}

static int sch_bpf_change(struct Qdisc *sch, struct nlattr *opt,
			  struct netlink_ext_ack *extack)
{
	struct bpf_sched_data *q = qdisc_priv(sch);
	struct nlattr *tb[TCA_SCH_BPF_MAX + 1];
	int err;

	if (!opt)
		return -EINVAL;

	err = nla_parse_nested_deprecated(tb, TCA_SCH_BPF_MAX, opt,
					  sch_bpf_policy, NULL);
	if (err < 0)
		return err;

	sch_tree_lock(sch);

	err = bpf_init_prog(tb[TCA_SCH_BPF_ENQUEUE_PROG_FD],
			    tb[TCA_SCH_BPF_ENQUEUE_PROG_NAME], &q->enqueue_prog, false);
	if (err)
		goto failure;
	err = bpf_init_prog(tb[TCA_SCH_BPF_DEQUEUE_PROG_FD],
			    tb[TCA_SCH_BPF_DEQUEUE_PROG_NAME], &q->dequeue_prog, false);
	if (err)
		goto failure;
	err = bpf_init_prog(tb[TCA_SCH_BPF_RESET_PROG_FD],
			    tb[TCA_SCH_BPF_RESET_PROG_NAME], &q->reset_prog, true);
failure:
	sch_tree_unlock(sch);
	return err;
}

static int sch_bpf_init(struct Qdisc *sch, struct nlattr *opt,
			struct netlink_ext_ack *extack)
{
	struct bpf_sched_data *q = qdisc_priv(sch);
	int err;

	qdisc_watchdog_init(&q->watchdog, sch);
	if (opt) {
		err = sch_bpf_change(sch, opt, extack);
		if (err)
			return err;
	}

	err = tcf_block_get(&q->block, &q->filter_list, sch, extack);
	if (err)
		return err;

	return qdisc_class_hash_init(&q->clhash);
}

static void sch_bpf_reset(struct Qdisc *sch)
{
	struct bpf_sched_data *q = qdisc_priv(sch);
	struct bpf_qdisc_ctx ctx = {};
	struct sch_bpf_class *cl;
	struct bpf_prog *reset;
	unsigned int i;

	for (i = 0; i < q->clhash.hashsize; i++) {
		hlist_for_each_entry(cl, &q->clhash.hash[i], common.hnode) {
			if (cl->qdisc)
				qdisc_reset(cl->qdisc);
		}
	}

	qdisc_watchdog_cancel(&q->watchdog);
	reset = rcu_dereference(q->reset_prog.prog);
	if (reset)
		bpf_prog_run(reset, &ctx);
}

static void sch_bpf_destroy_class(struct Qdisc *sch, struct sch_bpf_class *cl)
{
	qdisc_put(cl->qdisc);
	kfree(cl);
}

static void sch_bpf_destroy(struct Qdisc *sch)
{
	struct bpf_sched_data *q = qdisc_priv(sch);
	struct sch_bpf_class *cl;
	unsigned int i;

	qdisc_watchdog_cancel(&q->watchdog);
	tcf_block_put(q->block);
	for (i = 0; i < q->clhash.hashsize; i++) {
		hlist_for_each_entry(cl, &q->clhash.hash[i], common.hnode) {
			sch_bpf_destroy_class(sch, cl);
		}
	}

	qdisc_class_hash_destroy(&q->clhash);

	sch_tree_lock(sch);
	bpf_cleanup_prog(&q->enqueue_prog);
	bpf_cleanup_prog(&q->dequeue_prog);
	bpf_cleanup_prog(&q->reset_prog);
	sch_tree_unlock(sch);
}

static int sch_bpf_change_class(struct Qdisc *sch, u32 classid,
				u32 parentid, struct nlattr **tca,
				unsigned long *arg,
				struct netlink_ext_ack *extack)
{
	struct sch_bpf_class *cl = (struct sch_bpf_class *)*arg;
	struct bpf_sched_data *q = qdisc_priv(sch);

	if (!cl) {
		if (classid == 0 || TC_H_MAJ(classid ^ sch->handle) != 0 ||
		    sch_bpf_find(sch, classid))
			return -EINVAL;

		cl = kzalloc(sizeof(*cl), GFP_KERNEL);
		if (!cl)
			return -ENOBUFS;

		cl->common.classid = classid;
		gnet_stats_basic_sync_init(&cl->bstats);
		qdisc_class_hash_insert(&q->clhash, &cl->common);
	}

	qdisc_class_hash_grow(sch, &q->clhash);
	*arg = (unsigned long)cl;
	return 0;
}

static int sch_bpf_delete(struct Qdisc *sch, unsigned long arg,
			  struct netlink_ext_ack *extack)
{
	struct sch_bpf_class *cl = (struct sch_bpf_class *)arg;
	struct bpf_sched_data *q = qdisc_priv(sch);

	qdisc_class_hash_remove(&q->clhash, &cl->common);
	if (cl->qdisc)
		qdisc_put(cl->qdisc);
	return 0;
}

static int sch_bpf_dump_class(struct Qdisc *sch, unsigned long arg,
			      struct sk_buff *skb, struct tcmsg *tcm)
{
	return 0;
}

static int
sch_bpf_dump_class_stats(struct Qdisc *sch, unsigned long arg, struct gnet_dump *d)
{
	struct sch_bpf_class *cl = (struct sch_bpf_class *)arg;
	struct gnet_stats_queue qs = {
		.drops = cl->drops,
		.overlimits = cl->overlimits,
	};
	__u32 qlen = 0;

	if (cl->qdisc)
		qdisc_qstats_qlen_backlog(cl->qdisc, &qlen, &qs.backlog);
	else
		qlen = 0;

	if (gnet_stats_copy_basic(d, NULL, &cl->bstats, true) < 0 ||
	    gnet_stats_copy_queue(d, NULL, &qs, qlen) < 0)
		return -1;
	return 0;
}

static void sch_bpf_walk(struct Qdisc *sch, struct qdisc_walker *arg)
{
	struct bpf_sched_data *q = qdisc_priv(sch);
	struct sch_bpf_class *cl;
	unsigned int i;

	if (arg->stop)
		return;

	for (i = 0; i < q->clhash.hashsize; i++) {
		hlist_for_each_entry(cl, &q->clhash.hash[i], common.hnode) {
			if (arg->count < arg->skip) {
				arg->count++;
				continue;
			}
			if (arg->fn(sch, (unsigned long)cl, arg) < 0) {
				arg->stop = 1;
				return;
			}
			arg->count++;
		}
	}
}

static const struct Qdisc_class_ops sch_bpf_class_ops = {
	.graft		=	sch_bpf_graft,
	.leaf		=	sch_bpf_leaf,
	.find		=	sch_bpf_search,
	.change		=	sch_bpf_change_class,
	.delete		=	sch_bpf_delete,
	.tcf_block	=	sch_bpf_tcf_block,
	.bind_tcf	=	sch_bpf_bind,
	.unbind_tcf	=	sch_bpf_unbind,
	.dump		=	sch_bpf_dump_class,
	.dump_stats	=	sch_bpf_dump_class_stats,
	.walk		=	sch_bpf_walk,
};

static struct Qdisc_ops sch_bpf_qdisc_ops __read_mostly = {
	.cl_ops		=	&sch_bpf_class_ops,
	.id		=	"bpf",
	.priv_size	=	sizeof(struct bpf_sched_data),
	.enqueue	=	sch_bpf_enqueue,
	.dequeue	=	sch_bpf_dequeue,
	.peek		=	qdisc_peek_dequeued,
	.init		=	sch_bpf_init,
	.reset		=	sch_bpf_reset,
	.destroy	=	sch_bpf_destroy,
	.change		=	sch_bpf_change,
	.dump		=	sch_bpf_dump,
	.dump_stats	=	sch_bpf_dump_stats,
	.owner		=	THIS_MODULE,
};

__diag_push();
__diag_ignore_all("-Wmissing-prototypes",
		  "Global functions as their definitions will be in vmlinux BTF");

/* bpf_skb_acquire - Acquire a reference to an skb. An skb acquired by this
 * kfunc which is not stored in a map as a kptr, must be released by calling
 * bpf_skb_release().
 * @skb: The skb on which a reference is being acquired.
 */
__bpf_kfunc struct sk_buff *bpf_skb_acquire(struct sk_buff *skb)
{
	return skb_get(skb);
}

/* bpf_skb_release - Release the reference acquired on an skb.
 * @skb: The skb on which a reference is being released.
 */
__bpf_kfunc void bpf_skb_release(struct sk_buff *skb)
{
	skb_unref(skb);
}

/* bpf_skb_destroy - Release an skb reference acquired and exchanged into
 * an allocated object or a map.
 * @skb: The skb on which a reference is being released.
 */
__bpf_kfunc void bpf_skb_destroy(struct sk_buff *skb)
{
	skb_unref(skb);
	consume_skb(skb);
}

/* bpf_skb_get_hash - Get the flow hash of an skb.
 * @skb: The skb to get the flow hash from.
 */
__bpf_kfunc u32 bpf_skb_get_hash(struct sk_buff *skb)
{
	return skb_get_hash(skb);
}

/* bpf_qdisc_set_skb_dequeue - Set the skb to be dequeued. This will also
 * release the reference to the skb.
 * @skb: The skb to be dequeued by the qdisc.
 */
__bpf_kfunc void bpf_qdisc_set_skb_dequeue(struct sk_buff *skb)
{
	consume_skb(skb);
	__this_cpu_write(bpf_skb_dequeue, skb);
}

/* bpf_skb_tc_classify - Classify an skb using an existing filter referred
 * to by the specified handle on the net device of index ifindex.
 * @skb: The skb to be classified.
 * @handle: The handle of the filter to be referenced.
 * @ifindex: The ifindex of the net device where the filter is attached.
 *
 * Returns a 64-bit integer containing the tc action verdict and the classid,
 * created as classid << 32 | action.
 */
__bpf_kfunc u64 bpf_skb_tc_classify(struct sk_buff *skb, int ifindex,
				    u32 handle)
{
	struct net *net = dev_net(skb->dev);
	const struct Qdisc_class_ops *cops;
	struct tcf_result res = {};
	struct tcf_block *block;
	struct tcf_chain *chain;
	struct net_device *dev;
	int result = TC_ACT_OK;
	unsigned long cl = 0;
	struct Qdisc *q;

	rcu_read_lock();
	dev = dev_get_by_index_rcu(net, ifindex);
	if (!dev)
		goto out;
	q = qdisc_lookup_rcu(dev, handle);
	if (!q)
		goto out;

	cops = q->ops->cl_ops;
	if (!cops)
		goto out;
	if (!cops->tcf_block)
		goto out;
	if (TC_H_MIN(handle)) {
		cl = cops->find(q, handle);
		if (cl == 0)
			goto out;
	}
	block = cops->tcf_block(q, cl, NULL);
	if (!block)
		goto out;

	for (chain = tcf_get_next_chain(block, NULL);
	     chain;
	     chain = tcf_get_next_chain(block, chain)) {
		struct tcf_proto *tp;

		result = tcf_classify(skb, NULL, tp, &res, false);
		if (result >= 0) {
			switch (result) {
			case TC_ACT_QUEUED:
			case TC_ACT_STOLEN:
			case TC_ACT_TRAP:
				fallthrough;
			case TC_ACT_SHOT:
				rcu_read_unlock();
				return result;
			}
		}
	}
out:
	rcu_read_unlock();
	return (res.class << 32 | result);
}

__diag_pop();

BTF_SET8_START(skb_kfunc_btf_ids)
BTF_ID_FLAGS(func, bpf_skb_acquire, KF_ACQUIRE)
BTF_ID_FLAGS(func, bpf_skb_release, KF_RELEASE)
BTF_ID_FLAGS(func, bpf_skb_get_hash)
BTF_ID_FLAGS(func, bpf_qdisc_set_skb_dequeue, KF_RELEASE)
BTF_ID_FLAGS(func, bpf_skb_tc_classify)
BTF_SET8_END(skb_kfunc_btf_ids)

static const struct btf_kfunc_id_set skb_kfunc_set = {
	.owner = THIS_MODULE,
	.set   = &skb_kfunc_btf_ids,
};

BTF_ID_LIST(skb_kfunc_dtor_ids)
BTF_ID(struct, sk_buff)
BTF_ID_FLAGS(func, bpf_skb_destroy, KF_RELEASE)

static int __init sch_bpf_mod_init(void)
{
	int ret;
	const struct btf_id_dtor_kfunc skb_kfunc_dtors[] = {
		{
			.btf_id       = skb_kfunc_dtor_ids[0],
			.kfunc_btf_id = skb_kfunc_dtor_ids[1]
		},
	};

	ret = register_btf_kfunc_id_set(BPF_PROG_TYPE_QDISC, &skb_kfunc_set);
	ret = ret ?: register_btf_id_dtor_kfuncs(skb_kfunc_dtors,
						 ARRAY_SIZE(skb_kfunc_dtors),
						 THIS_MODULE);
	return ret ?: register_qdisc(&sch_bpf_qdisc_ops);
}

static void __exit sch_bpf_mod_exit(void)
{
	unregister_qdisc(&sch_bpf_qdisc_ops);
}

module_init(sch_bpf_mod_init)
module_exit(sch_bpf_mod_exit)
MODULE_AUTHOR("Cong Wang");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("eBPF queue discipline");
