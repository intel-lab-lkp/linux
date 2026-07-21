// SPDX-License-Identifier: GPL-2.0
/* Marvell RVU switch driver
 *
 * Copyright (C) 2026 Marvell.
 *
 */
#include <linux/kernel.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/mutex.h>
#include <linux/refcount.h>
#include <net/switchdev.h>
#include <net/netevent.h>
#include <net/arp.h>
#include <net/nexthop.h>
#include <net/netfilter/nf_flow_table.h>

#include "../otx2_reg.h"
#include "../otx2_common.h"
#include "../otx2_struct.h"
#include "../cn10k.h"
#include "sw_nb.h"
#include "sw_trace.h"
#include "sw_fl.h"

#if !IS_ENABLED(CONFIG_OCTEONTX_SWITCH)
int sw_fl_setup_ft_block_ingress_cb(enum tc_setup_type type,
				    void *type_data, void *cb_priv)
{
	return -EOPNOTSUPP;
}

#else

static DEFINE_SPINLOCK(sw_fl_lock);
static LIST_HEAD(sw_fl_lh);

struct sw_fl_list_entry {
	struct list_head list;
	u64 flags;
	unsigned long cookie;
	struct otx2_nic *pf;
	netdevice_tracker dev_tracker;
	struct fl_tuple tuple;
};

static struct workqueue_struct *sw_fl_wq;
static void sw_fl_wq_handler(struct work_struct *work);
static DECLARE_DELAYED_WORK(sw_fl_work, sw_fl_wq_handler);

struct sw_fl_ct_cb {
	struct list_head list;
	struct nf_flowtable *ft;
	struct otx2_nic *nic;
	refcount_t ref;
};

struct sw_fl_ct_cookie {
	struct list_head list;
	unsigned long cookie;
	struct nf_flowtable *ft;
	struct otx2_nic *nic;
};

struct sw_fl_ct_drain {
	struct list_head list;
	struct nf_flowtable *ft;
	struct otx2_nic *nic;
};

static LIST_HEAD(sw_fl_ct_cb_list);
static LIST_HEAD(sw_fl_ct_drain_list);
static DEFINE_MUTEX(sw_fl_ct_cb_lock);
static LIST_HEAD(sw_fl_ct_cookie_list);
static DEFINE_MUTEX(sw_fl_ct_cookie_lock);

static struct sw_fl_ct_cb *sw_fl_ct_cb_find(struct nf_flowtable *ft,
					    struct otx2_nic *nic)
{
	struct sw_fl_ct_cb *entry;

	list_for_each_entry(entry, &sw_fl_ct_cb_list, list) {
		if (entry->ft == ft && entry->nic == nic)
			return entry;
	}

	return NULL;
}

static bool sw_fl_ct_draining(struct nf_flowtable *ft, struct otx2_nic *nic)
{
	struct sw_fl_ct_drain *drain;

	list_for_each_entry(drain, &sw_fl_ct_drain_list, list) {
		if (drain->ft == ft && drain->nic == nic)
			return true;
	}

	return false;
}

static int sw_fl_ct_cb_get(struct nf_flowtable *ft, struct otx2_nic *nic)
{
	struct sw_fl_ct_cb *entry;
	int err;

	mutex_lock(&sw_fl_ct_cb_lock);
	if (sw_fl_ct_draining(ft, nic)) {
		mutex_unlock(&sw_fl_ct_cb_lock);
		return -EBUSY;
	}
	entry = sw_fl_ct_cb_find(ft, nic);
	if (entry) {
		refcount_inc(&entry->ref);
		mutex_unlock(&sw_fl_ct_cb_lock);
		return 0;
	}
	mutex_unlock(&sw_fl_ct_cb_lock);

	err = nf_flow_table_offload_add_cb(ft, sw_fl_setup_ft_block_ingress_cb, nic);
	if (err && err != -EEXIST)
		return err;

	mutex_lock(&sw_fl_ct_cb_lock);
	if (sw_fl_ct_draining(ft, nic)) {
		mutex_unlock(&sw_fl_ct_cb_lock);
		if (!err)
			nf_flow_table_offload_del_cb(ft,
						     sw_fl_setup_ft_block_ingress_cb,
						     nic);
		return -EBUSY;
	}
	entry = sw_fl_ct_cb_find(ft, nic);
	if (entry) {
		refcount_inc(&entry->ref);
		mutex_unlock(&sw_fl_ct_cb_lock);
		if (!err)
			nf_flow_table_offload_del_cb(ft,
						     sw_fl_setup_ft_block_ingress_cb,
						     nic);
		return 0;
	}

	entry = kzalloc_obj(*entry, GFP_KERNEL);
	if (!entry) {
		mutex_unlock(&sw_fl_ct_cb_lock);
		if (!err)
			nf_flow_table_offload_del_cb(ft,
						     sw_fl_setup_ft_block_ingress_cb,
						     nic);
		return -ENOMEM;
	}

	entry->ft = ft;
	entry->nic = nic;
	refcount_set(&entry->ref, 1);
	list_add_tail(&entry->list, &sw_fl_ct_cb_list);
	mutex_unlock(&sw_fl_ct_cb_lock);

	return 0;
}

static void sw_fl_ct_cb_put(struct nf_flowtable *ft, struct otx2_nic *nic)
{
	struct sw_fl_ct_cb *entry;
	struct sw_fl_ct_drain drain;

	mutex_lock(&sw_fl_ct_cb_lock);
	entry = sw_fl_ct_cb_find(ft, nic);
	if (!entry || !refcount_dec_and_test(&entry->ref)) {
		mutex_unlock(&sw_fl_ct_cb_lock);
		return;
	}

	drain.ft = ft;
	drain.nic = nic;
	INIT_LIST_HEAD(&drain.list);
	list_add_tail(&drain.list, &sw_fl_ct_drain_list);

	list_del(&entry->list);
	mutex_unlock(&sw_fl_ct_cb_lock);

	nf_flow_table_offload_del_cb(ft, sw_fl_setup_ft_block_ingress_cb, nic);

	mutex_lock(&sw_fl_ct_cb_lock);
	list_del(&drain.list);
	mutex_unlock(&sw_fl_ct_cb_lock);

	kfree(entry);
}

static struct sw_fl_ct_cookie *sw_fl_ct_cookie_find(unsigned long cookie)
{
	struct sw_fl_ct_cookie *entry;

	list_for_each_entry(entry, &sw_fl_ct_cookie_list, list) {
		if (entry->cookie == cookie)
			return entry;
	}

	return NULL;
}

static int sw_fl_ct_cookie_add(unsigned long cookie, struct nf_flowtable *ft,
			       struct otx2_nic *nic)
{
	struct sw_fl_ct_cookie *entry, *existing;

	entry = kzalloc_obj(*entry, GFP_KERNEL);
	if (!entry)
		return -ENOMEM;

	entry->cookie = cookie;
	entry->ft = ft;
	entry->nic = nic;

	mutex_lock(&sw_fl_ct_cookie_lock);
	existing = sw_fl_ct_cookie_find(cookie);
	if (existing) {
		if (existing->ft == ft && existing->nic == nic) {
			mutex_unlock(&sw_fl_ct_cookie_lock);
			kfree(entry);
			sw_fl_ct_cb_put(ft, nic);
			return 0;
		}

		list_del(&existing->list);
		mutex_unlock(&sw_fl_ct_cookie_lock);
		sw_fl_ct_cb_put(existing->ft, existing->nic);
		kfree(existing);

		mutex_lock(&sw_fl_ct_cookie_lock);
	}

	list_add_tail(&entry->list, &sw_fl_ct_cookie_list);
	mutex_unlock(&sw_fl_ct_cookie_lock);

	return 0;
}

static void sw_fl_ct_cookie_put(unsigned long cookie)
{
	struct sw_fl_ct_cookie *entry, *tmp;

	mutex_lock(&sw_fl_ct_cookie_lock);
	list_for_each_entry_safe(entry, tmp, &sw_fl_ct_cookie_list, list) {
		if (entry->cookie != cookie)
			continue;

		list_del(&entry->list);
		mutex_unlock(&sw_fl_ct_cookie_lock);
		sw_fl_ct_cb_put(entry->ft, entry->nic);
		kfree(entry);
		return;
	}
	mutex_unlock(&sw_fl_ct_cookie_lock);
}

static void sw_fl_ct_cb_flush(void)
{
	struct sw_fl_ct_cookie *cookie, *ctmp;
	struct sw_fl_ct_cb *entry;
	struct sw_fl_ct_drain drain;
	struct nf_flowtable *ft;
	struct otx2_nic *nic;

	mutex_lock(&sw_fl_ct_cookie_lock);
	list_for_each_entry_safe(cookie, ctmp, &sw_fl_ct_cookie_list, list) {
		list_del(&cookie->list);
		kfree(cookie);
	}
	mutex_unlock(&sw_fl_ct_cookie_lock);

	mutex_lock(&sw_fl_ct_cb_lock);
	while ((entry = list_first_entry_or_null(&sw_fl_ct_cb_list,
						 struct sw_fl_ct_cb, list))) {
		ft = entry->ft;
		nic = entry->nic;
		drain.ft = ft;
		drain.nic = nic;
		INIT_LIST_HEAD(&drain.list);
		list_add_tail(&drain.list, &sw_fl_ct_drain_list);

		list_del(&entry->list);
		mutex_unlock(&sw_fl_ct_cb_lock);

		nf_flow_table_offload_del_cb(ft,
					     sw_fl_setup_ft_block_ingress_cb,
					     nic);

		mutex_lock(&sw_fl_ct_cb_lock);
		list_del(&drain.list);
		kfree(entry);
	}
	mutex_unlock(&sw_fl_ct_cb_lock);
}

static int sw_fl_msg_send(struct otx2_nic *pf,
			  struct fl_tuple *tuple,
			  u64 flags,
			  unsigned long cookie)
{
	struct fl_notify_req *req;
	int rc;

	mutex_lock(&pf->mbox.lock);
	req = otx2_mbox_alloc_msg_fl_notify(&pf->mbox);
	if (!req) {
		rc = -ENOMEM;
		goto out;
	}

	req->tuple = *tuple;
	req->flags = flags;
	req->cookie = cookie;

	rc = otx2_sync_mbox_msg(&pf->mbox);
out:
	mutex_unlock(&pf->mbox.lock);
	return rc;
}

static void sw_fl_wq_handler(struct work_struct *work)
{
	struct sw_fl_list_entry *entry;
	LIST_HEAD(tlist);

	spin_lock_bh(&sw_fl_lock);
	list_splice_init(&sw_fl_lh, &tlist);
	spin_unlock_bh(&sw_fl_lock);

	while ((entry =
		list_first_entry_or_null(&tlist,
					 struct sw_fl_list_entry,
					 list)) != NULL) {
		list_del_init(&entry->list);
		if (sw_fl_msg_send(entry->pf, &entry->tuple,
				   entry->flags, entry->cookie)) {
			netdev_err(entry->pf->netdev,
				   "Failed to notify flow update to AF, will retry\n");
			spin_lock_bh(&sw_fl_lock);
			if (sw_fl_wq) {
				list_add(&entry->list, &sw_fl_lh);
				queue_delayed_work(sw_fl_wq, &sw_fl_work,
						   msecs_to_jiffies(100));
				spin_unlock_bh(&sw_fl_lock);
				continue;
			}
			spin_unlock_bh(&sw_fl_lock);
			netdev_put(entry->pf->netdev, &entry->dev_tracker);
			kfree(entry);
			continue;
		}
		netdev_put(entry->pf->netdev, &entry->dev_tracker);
		kfree(entry);
	}

	spin_lock_bh(&sw_fl_lock);
	if (!list_empty(&sw_fl_lh) && sw_fl_wq)
		queue_delayed_work(sw_fl_wq, &sw_fl_work, msecs_to_jiffies(10));
	spin_unlock_bh(&sw_fl_lock);
}

static int
sw_fl_add_to_list(struct otx2_nic *pf, struct fl_tuple *tuple,
		  unsigned long cookie, bool add_fl)
{
	struct sw_fl_list_entry *entry;

	entry = kcalloc(1, sizeof(*entry), GFP_ATOMIC);
	if (!entry)
		return -ENOMEM;

	entry->pf = pf;
	entry->flags = add_fl ? FL_ADD : FL_DEL;
	if (add_fl)
		entry->tuple = *tuple;
	entry->cookie = cookie;
	entry->tuple.uni_di = netif_is_ovs_port(pf->netdev);

	spin_lock_bh(&sw_fl_lock);
	if (!sw_fl_wq) {
		spin_unlock_bh(&sw_fl_lock);
		kfree(entry);
		return -EINVAL;
	}

	netdev_hold(pf->netdev, &entry->dev_tracker, GFP_ATOMIC);
	list_add_tail(&entry->list, &sw_fl_lh);
	queue_delayed_work(sw_fl_wq, &sw_fl_work, msecs_to_jiffies(10));
	spin_unlock_bh(&sw_fl_lock);

	return 0;
}

static int sw_fl_mangle_layer(enum flow_action_mangle_base htype)
{
	switch (htype) {
	case FLOW_ACT_MANGLE_HDR_TYPE_ETH:
		return 0;
	case FLOW_ACT_MANGLE_HDR_TYPE_IP4:
	case FLOW_ACT_MANGLE_HDR_TYPE_IP6:
		return 2;
	case FLOW_ACT_MANGLE_HDR_TYPE_TCP:
	case FLOW_ACT_MANGLE_HDR_TYPE_UDP:
		return 3;
	default:
		return -EOPNOTSUPP;
	}
}

static int sw_fl_parse_actions(struct otx2_nic *nic,
			       struct flow_action *flow_action,
			       struct flow_cls_offload *f,
			       struct fl_tuple *tuple, u64 *op,
			       struct nf_flowtable **ct_ft)
{
	struct flow_action_entry *act;
	struct nf_flowtable *parsed_ct_ft = NULL;
	struct otx2_nic *out_nic;
	int parsed_ct_refs = 0;
	int used = 0;
	int err;
	int i;

	if (!flow_action_has_entries(flow_action))
		return -EINVAL;

	flow_action_for_each(i, act, flow_action) {
		switch (act->id) {
		case FLOW_ACTION_REDIRECT:
			if (!act->dev || !sw_nb_is_valid_dev(act->dev)) {
				err = -EOPNOTSUPP;
				goto unwind_ct;
			}
			trace_sw_act_dump(__func__, "redirect to egress port", act->id);
			tuple->in_pf = nic->pcifunc;
			out_nic = netdev_priv(act->dev);
			tuple->xmit_pf = out_nic->pcifunc;
			*op |= BIT_ULL(FLOW_ACTION_REDIRECT);
			break;

		case FLOW_ACTION_CT:
			trace_sw_act_dump(__func__, "register conntrack offload callback", act->id);
			err = sw_fl_ct_cb_get(act->ct.flow_table, nic);
			if (err) {
				netdev_err(nic->netdev,
					   "%s: Error to offload flow, err=%d\n",
					   __func__, err);
				goto unwind_ct;
			}

			parsed_ct_ft = act->ct.flow_table;
			parsed_ct_refs++;
			if (ct_ft)
				*ct_ft = act->ct.flow_table;
			*op |= BIT_ULL(FLOW_ACTION_CT);
			break;

		case FLOW_ACTION_MANGLE: {
			int layer;

			if (used >= MANGLE_ARR_SZ) {
				netdev_err(nic->netdev,
					   "%s: More mangle entries than supported %u\n",
					   __func__, MANGLE_ARR_SZ);
				err = -ENOMEM;
				goto unwind_ct;
			}

			layer = sw_fl_mangle_layer(act->mangle.htype);
			if (layer < 0) {
				err = layer;
				goto unwind_ct;
			}

			trace_sw_act_dump(__func__, "header mangle action", act->id);
			tuple->mangle[used].type = act->mangle.htype;
			tuple->mangle[used].val = act->mangle.val;
			tuple->mangle[used].mask = act->mangle.mask;
			tuple->mangle[used].offset = act->mangle.offset;
			tuple->mangle_map[layer] |= BIT(used);
			used++;
			break;
		}

		default:
			trace_sw_act_dump(__func__, "unsupported flow action", act->id);
			break;
		}
	}

	tuple->mangle_cnt = used;

	if (!*op && !used) {
		netdev_dbg(nic->netdev, "%s: Op is not valid\n", __func__);
		return -EOPNOTSUPP;
	}

	return 0;

unwind_ct:
	while (parsed_ct_refs--)
		sw_fl_ct_cb_put(parsed_ct_ft, nic);
	return err;
}

static int sw_fl_get_route(struct net *net, struct fib_result *res, __be32 addr)
{
	struct flowi4 fl4;

	memset(&fl4, 0, sizeof(fl4));
	fl4.daddr = addr;
	return fib_lookup(net, &fl4, res, 0);
}

static int sw_fl_get_pcifunc(struct otx2_nic *pf, __be32 dst, u16 *pcifunc,
			     struct fl_tuple *ftuple, bool is_in_dev)
{
	struct fib_nh_common *fib_nhc;
	struct net_device *dev, *br;
	struct fib_result res;
	struct list_head *lh;
	struct otx2_nic *nic;
	int err;

	rcu_read_lock();

	err = sw_fl_get_route(dev_net(pf->netdev), &res, dst);
	if (err) {
		netdev_err(pf->netdev,
			   "%s: Failed to find route to dst %pI4\n",
			   __func__, &dst);
		goto done;
	}

	if (res.fi->fib_type != RTN_UNICAST) {
		netdev_err(pf->netdev,
			   "%s: Not unicast  route to dst %pi4\n",
			   __func__, &dst);
		err = -EFAULT;
		goto done;
	}

	fib_nhc = fib_info_nhc(res.fi, 0);
	if (!fib_nhc) {
		err = -EINVAL;
		netdev_err(pf->netdev,
			   "%s: Could not get fib_nhc for %pI4\n",
			   __func__, &dst);
		goto done;
	}

	if (unlikely(netif_is_bridge_master(fib_nhc->nhc_dev))) {
		br = fib_nhc->nhc_dev;

		if (is_in_dev)
			ftuple->is_indev_br = 1;
		else
			ftuple->is_xdev_br = 1;

		lh = &br->adj_list.lower;
		if (list_empty(lh)) {
			netdev_err(pf->netdev,
				   "%s: Unable to find any slave device\n",
				   __func__);
			err = -EINVAL;
			goto done;
		}
		dev = netdev_next_lower_dev_rcu(br, &lh);

	} else {
		dev = fib_nhc->nhc_dev;
	}

	if (!dev || !sw_nb_is_valid_dev(dev)) {
		netdev_err(pf->netdev,
			   "%s: flow acceleration support is only for cavium devices\n",
			   __func__);
		err = -EOPNOTSUPP;
		goto done;
	}

	nic = netdev_priv(dev);
	*pcifunc = nic->pcifunc;

done:
	rcu_read_unlock();
	return err;
}

static int sw_fl_parse_flow(struct otx2_nic *nic, struct flow_cls_offload *f,
			    struct fl_tuple *tuple, u64 *features)
{
	struct flow_rule *rule;
	u8 ip_proto = 0;

	*features = 0;

	rule = flow_cls_offload_flow_rule(f);

	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_BASIC)) {
		struct flow_match_basic match;

		flow_rule_match_basic(rule, &match);

		/* All EtherTypes can be matched, no hw limitation */

		if (match.mask->n_proto) {
			tuple->eth_type = match.key->n_proto;
			tuple->m_eth_type = match.mask->n_proto;
			*features |= BIT_ULL(NPC_ETYPE);
		}

		if (match.mask->ip_proto) {
			if (match.key->ip_proto != IPPROTO_TCP &&
			    match.key->ip_proto != IPPROTO_UDP)
				return -EOPNOTSUPP;

			ip_proto = match.key->ip_proto;
			if (ip_proto == IPPROTO_UDP)
				*features |= BIT_ULL(NPC_IPPROTO_UDP);
			else
				*features |= BIT_ULL(NPC_IPPROTO_TCP);
		}

		tuple->proto = ip_proto;
	}

	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_ETH_ADDRS)) {
		struct flow_match_eth_addrs match;

		flow_rule_match_eth_addrs(rule, &match);

		/* Switch flow offload matches unicast L2 addresses only.
		 * Multicast and broadcast MAC keys are not programmed in
		 * hardware; rules that rely solely on those keys are not
		 * supported here.
		 */
		if (!is_zero_ether_addr(match.key->dst) &&
		    is_unicast_ether_addr(match.key->dst)) {
			ether_addr_copy(tuple->dmac,
					match.key->dst);

			ether_addr_copy(tuple->m_dmac,
					match.mask->dst);

			*features |= BIT_ULL(NPC_DMAC);
		}

		if (!is_zero_ether_addr(match.key->src) &&
		    is_unicast_ether_addr(match.key->src)) {
			ether_addr_copy(tuple->smac,
					match.key->src);
			ether_addr_copy(tuple->m_smac,
					match.mask->src);
			*features |= BIT_ULL(NPC_SMAC);
		}
	}

	/* Switch flow offload parses IPv4 address keys only; IPv6 flow
	 * matching and FIB-based pcifunc resolution are not supported yet.
	 */
	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_IPV4_ADDRS)) {
		struct flow_match_ipv4_addrs match;

		flow_rule_match_ipv4_addrs(rule, &match);

		if (match.mask->dst) {
			tuple->ip4dst = match.key->dst;
			tuple->m_ip4dst = match.mask->dst;
			*features |= BIT_ULL(NPC_DIP_IPV4);
		}

		if (match.mask->src) {
			tuple->ip4src = match.key->src;
			tuple->m_ip4src = match.mask->src;
			*features |= BIT_ULL(NPC_SIP_IPV4);
		}
	}

	if (!(*features & BIT_ULL(NPC_DMAC))) {
		if (!tuple->m_ip4src || !tuple->m_ip4dst) {
			netdev_err(nic->netdev,
				   "%s: Invalid src=%pI4 and dst=%pI4 addresses\n",
				   __func__, &tuple->ip4src, &tuple->ip4dst);
			return -EINVAL;
		}

		if ((tuple->ip4src & tuple->m_ip4src) == (tuple->ip4dst & tuple->m_ip4dst)) {
			netdev_err(nic->netdev,
				   "%s: Masked values are same; Invalid src=%pI4 and dst=%pI4 addresses\n",
				   __func__, &tuple->ip4src, &tuple->ip4dst);
			return -EINVAL;
		}
	}

	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_PORTS)) {
		struct flow_match_ports match;

		flow_rule_match_ports(rule, &match);

		if (ip_proto == IPPROTO_UDP) {
			if (match.mask->dst)
				*features |= BIT_ULL(NPC_DPORT_UDP);

			if (match.mask->src)
				*features |= BIT_ULL(NPC_SPORT_UDP);
		} else if (ip_proto == IPPROTO_TCP) {
			if (match.mask->dst)
				*features |= BIT_ULL(NPC_DPORT_TCP);

			if (match.mask->src)
				*features |= BIT_ULL(NPC_SPORT_TCP);
		}

		if (match.mask->src) {
			tuple->sport = match.key->src;
			tuple->m_sport = match.mask->src;
		}

		if (match.mask->dst) {
			tuple->dport = match.key->dst;
			tuple->m_dport = match.mask->dst;
		}
	}

	if (!(*features & (BIT_ULL(NPC_DMAC) |
			   BIT_ULL(NPC_SMAC) |
			   BIT_ULL(NPC_DIP_IPV4) |
			   BIT_ULL(NPC_SIP_IPV4) |
			   BIT_ULL(NPC_DIP_IPV6) |
			   BIT_ULL(NPC_SIP_IPV6) |
			   BIT_ULL(NPC_DPORT_UDP) |
			   BIT_ULL(NPC_SPORT_UDP) |
			   BIT_ULL(NPC_DPORT_TCP) |
			   BIT_ULL(NPC_SPORT_TCP)))) {
		return -EINVAL;
	}

	tuple->features = *features;

	return 0;
}

static int sw_fl_add(struct otx2_nic *nic, struct flow_cls_offload *f)
{
	struct nf_flowtable *ct_ft = NULL;
	struct fl_tuple tuple = { 0 };
	struct flow_rule *rule;
	u64 features = 0;
	u64 op = 0;
	int rc;

	rule = flow_cls_offload_flow_rule(f);

	rc = sw_fl_parse_actions(nic, &rule->action, f, &tuple, &op, &ct_ft);
	if (rc)
		return rc;

	if (ct_ft) {
		rc = sw_fl_ct_cookie_add(f->cookie, ct_ft, nic);
		if (rc) {
			sw_fl_ct_cb_put(ct_ft, nic);
			return rc;
		}
	}

	if (op == BIT_ULL(FLOW_ACTION_CT) && !tuple.mangle_cnt)
		return 0;

	rc  = sw_fl_parse_flow(nic, f, &tuple, &features);
	if (rc) {
		trace_sw_fl_dump(__func__, "flow key parse failed", &tuple);
		if (ct_ft)
			sw_fl_ct_cookie_put(f->cookie);
		return -EFAULT;
	}

	/* Non-OVS ports resolve ingress and egress pcifunc via IPv4 FIB
	 * lookups below. IPv6 and L2-only flows are not supported yet.
	 */
	if (!netif_is_ovs_port(nic->netdev)) {
		rc = sw_fl_get_pcifunc(nic, tuple.ip4src, &tuple.in_pf,
				       &tuple, true);
		if (rc) {
			trace_sw_fl_dump(__func__, "ingress pcifunc lookup failed", &tuple);
			if (ct_ft)
				sw_fl_ct_cookie_put(f->cookie);
			return rc;
		}

		rc = sw_fl_get_pcifunc(nic, tuple.ip4dst,
				       &tuple.xmit_pf, &tuple, false);
		if (rc) {
			trace_sw_fl_dump(__func__, "egress pcifunc lookup failed", &tuple);
			if (ct_ft)
				sw_fl_ct_cookie_put(f->cookie);
			return rc;
		}
	}

	trace_sw_fl_dump(__func__, "offload flow add queued", &tuple);
	return sw_fl_add_to_list(nic, &tuple, f->cookie, true);
}

static int sw_fl_del(struct otx2_nic *nic, struct flow_cls_offload *f)
{
	sw_fl_ct_cookie_put(f->cookie);
	return sw_fl_add_to_list(nic, NULL, f->cookie, false);
}

static int sw_fl_stats(struct otx2_nic *nic, struct flow_cls_offload *f)
{
	struct fl_get_stats_req *req;
	struct fl_get_stats_rsp *rsp;
	u64 pkts_diff;
	int rc = 0;

	mutex_lock(&nic->mbox.lock);

	req = otx2_mbox_alloc_msg_fl_get_stats(&nic->mbox);
	if (!req) {
		netdev_err(nic->netdev,
			   "%s: Error happened while mcam alloc req\n",
			   __func__);
		rc = -ENOMEM;
		goto fail;
	}
	req->cookie = f->cookie;

	rc = otx2_sync_mbox_msg(&nic->mbox);
	if (rc)
		goto fail;

	rsp = (struct fl_get_stats_rsp *)otx2_mbox_get_rsp
		(&nic->mbox.mbox, 0, &req->hdr);
	if (IS_ERR(rsp)) {
		rc = PTR_ERR(rsp);
		goto fail;
	}
	pkts_diff = rsp->pkts_diff;
	mutex_unlock(&nic->mbox.lock);

	if (pkts_diff) {
		flow_stats_update(&f->stats, 0x0, pkts_diff,
				  0x0, jiffies,
				  FLOW_ACTION_HW_STATS_IMMEDIATE);
	}
	return 0;
fail:
	mutex_unlock(&nic->mbox.lock);
	return rc;
}

static bool init_done;

int sw_fl_setup_ft_block_ingress_cb(enum tc_setup_type type,
				    void *type_data, void *cb_priv)
{
	struct flow_cls_offload *cls = type_data;
	struct otx2_nic *nic = cb_priv;

	if (!smp_load_acquire(&init_done)) /* published in sw_fl_init() */
		return 0;

	switch (cls->command) {
	case FLOW_CLS_REPLACE:
		return sw_fl_add(nic, cls);
	case FLOW_CLS_DESTROY:
		return sw_fl_del(nic, cls);
	case FLOW_CLS_STATS:
		return sw_fl_stats(nic, cls);
	default:
		break;
	}

	return -EOPNOTSUPP;
}

int sw_fl_init(void)
{
	sw_fl_wq = alloc_workqueue("sw_fl_wq", 0, 0);
	if (!sw_fl_wq)
		return -ENOMEM;

	smp_store_release(&init_done, true); /* visible to sw_fl_setup_ft_block_ingress_cb() */
	return 0;
}

void sw_fl_deinit(void)
{
	struct sw_fl_list_entry *entry;
	struct workqueue_struct *wq;
	LIST_HEAD(tlist);

	smp_store_release(&init_done, false); /* visible to sw_fl_setup_ft_block_ingress_cb() */

	spin_lock_bh(&sw_fl_lock);
	wq = sw_fl_wq;
	sw_fl_wq = NULL;
	spin_unlock_bh(&sw_fl_lock);

	if (!wq)
		return;

	cancel_delayed_work_sync(&sw_fl_work);
	destroy_workqueue(wq);

	spin_lock_bh(&sw_fl_lock);
	list_splice_init(&sw_fl_lh, &tlist);
	spin_unlock_bh(&sw_fl_lock);

	while ((entry =
		list_first_entry_or_null(&tlist,
					 struct sw_fl_list_entry,
					 list)) != NULL) {
		list_del_init(&entry->list);
		netdev_put(entry->pf->netdev, &entry->dev_tracker);
		kfree(entry);
	}

	sw_fl_ct_cb_flush();
}
#endif
