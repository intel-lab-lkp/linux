// SPDX-License-Identifier: GPL-2.0-only
/*
 * Stream classification service (SCS) and mirrored SCS (MSCS)
 *
 * Copyright (C) 2026 Felix Fietkau <nbd@nbd.name>
 */
#include <linux/ieee80211.h>
#include <net/cfg80211.h>
#include "driver-ops.h"
#include "ieee80211_i.h"
#include "sta_info.h"

/* An hour, in TUs of 1024 microseconds */
#define IEEE80211_MSCS_TIMEOUT_MAX_TU	3515625

/* Limit on learned MSCS entries per station */
#define IEEE80211_MSCS_ENTRIES_MAX	64

/* The hashed bytes must hold no padding between the peer and the key */
static_assert(offsetofend(struct ieee80211_flow_hkey, key) ==
	      sizeof(struct sta_info *) + sizeof(struct cfg80211_flow_key));

static const struct rhashtable_params flow_rht_params = {
	.head_offset = offsetof(struct ieee80211_flow_entry, node),
	.key_offset = offsetof(struct ieee80211_flow_entry, hkey),
	.key_len = offsetofend(struct ieee80211_flow_hkey, key),
	.automatic_shrinking = true,
};

static size_t ieee80211_scs_rule_size(const struct cfg80211_scs_desc *desc)
{
	return ALIGN(struct_size(desc, tclas, desc->n_tclas) +
		     desc->qos_char_len, __alignof__(*desc));
}

static void *ieee80211_scs_rule_copy(const struct cfg80211_scs_desc *src,
				     void *pos)
{
	size_t len = struct_size(src, tclas, src->n_tclas);
	struct cfg80211_scs_desc *dst = pos;

	memcpy(dst, src, len);
	/* A stored rule is always handed back to the driver as an install */
	dst->req_type = NL80211_SCS_REQ_ADD;
	pos += len;

	if (src->qos_char_len) {
		memcpy(pos, src->qos_char, src->qos_char_len);
		dst->qos_char = pos;
	} else {
		dst->qos_char = NULL;
	}

	return PTR_ALIGN(pos + src->qos_char_len, __alignof__(*dst));
}

static u8 ieee80211_scs_rule_find(struct cfg80211_scs_desc * const *rule,
				  u8 n_rules, u8 id)
{
	u8 i;

	for (i = 0; i < n_rules; i++)
		if (rule[i]->id == id)
			break;

	return i;
}

static size_t ieee80211_scs_sta_head(unsigned int n_rules)
{
	struct ieee80211_scs_sta *scs;

	return ALIGN(struct_size(scs, rule, n_rules),
		     __alignof__(struct cfg80211_scs_desc));
}

/*
 * Size for every rule that can end up in the set, so that no allocation
 * fails after the driver programmed the accepted descriptors.
 */
static size_t ieee80211_scs_sta_size(const struct ieee80211_scs_sta *old,
				     struct cfg80211_scs_desc * const *desc,
				     u8 n_desc, unsigned int n_alloc)
{
	size_t size = ieee80211_scs_sta_head(n_alloc);
	unsigned int i;

	for (i = 0; old && i < old->n_rules; i++)
		size += ieee80211_scs_rule_size(old->rule[i]);

	for (i = 0; i < n_desc; i++)
		size += ieee80211_scs_rule_size(desc[i]);

	return size;
}

/*
 * @rule points into the old set and into the request, so copy each rule.
 * The block was sized for @n_alloc rules.
 */
static void ieee80211_scs_sta_fill(struct ieee80211_scs_sta *scs,
				   struct cfg80211_scs_desc * const *rule,
				   u8 n_rules, unsigned int n_alloc)
{
	void *pos = (void *)scs + ieee80211_scs_sta_head(n_alloc);
	u8 i;

	/* Set before the array is filled, for __counted_by() */
	scs->n_rules = n_rules;

	for (i = 0; i < n_rules; i++) {
		scs->rule[i] = pos;
		pos = ieee80211_scs_rule_copy(rule[i], pos);
	}
}

/**
 * ieee80211_flow_classify - give an MSDU the user priority of its stream
 *
 * @sta: the receiver
 * @skb: the MSDU, in IEEE 802.3 format
 *
 * Return: %true when it set skb->priority, %false to leave the frame to the
 *	QoS map.
 */
bool ieee80211_flow_classify(struct sta_info *sta, struct sk_buff *skb)
{
	struct ieee80211_flow_entry *entry;
	struct cfg80211_scs_verdict verdict;
	struct ieee80211_mscs_sta *mscs;
	struct ieee80211_flow_hkey hkey;
	struct cfg80211_flow_info info;
	struct ieee80211_scs_sta *scs;

	scs = rcu_dereference(sta->scs);
	mscs = rcu_dereference(sta->mscs);
	if (!scs && !mscs)
		return false;

	if (!cfg80211_flow_parse(skb, &info))
		return false;

	if (scs) {
		cfg80211_scs_evaluate(scs->rule, scs->n_rules, &info,
				      &verdict);
		if (verdict.match) {
			skb->priority = verdict.up;

			return true;
		}
	}

	/* An SCS match takes precedence over the MSCS */
	if (!mscs)
		return false;

	hkey.sta = sta;
	if (!cfg80211_flow_key_build(&info, mscs->layout, CFG80211_FLOW_AS_IS,
				     &hkey.key))
		return false;

	entry = rhashtable_lookup(&sta->sdata->bss->flow_tbl, &hkey,
				  flow_rht_params);
	if (!entry)
		return false;

	skb->priority = min_t(u8, READ_ONCE(entry->up), mscs->up_limit);

	return true;
}

static void ieee80211_flow_entry_free(struct ieee80211_if_ap *ap,
				      struct ieee80211_flow_entry *entry)
{
	rhashtable_remove_fast(&ap->flow_tbl, &entry->node, flow_rht_params);
	list_del(&entry->list);
	kfree_rcu(entry, rcu_head);
}

/* The caller holds @mscs->lock unless nothing else can reach the list */
static void ieee80211_flow_purge(struct ieee80211_if_ap *ap,
				 struct ieee80211_mscs_sta *mscs)
{
	struct ieee80211_flow_entry *entry, *tmp;

	list_for_each_entry_safe(entry, tmp, &mscs->entries, list)
		ieee80211_flow_entry_free(ap, entry);

	mscs->n_entries = 0;
}

/* Return the time until the next entry expires, zero when none is left */
static unsigned long ieee80211_flow_gc_sta(struct ieee80211_if_ap *ap,
					   struct sta_info *sta)
{
	struct ieee80211_flow_entry *entry, *tmp;
	struct ieee80211_mscs_sta *mscs;
	unsigned long delay = 0;

	mscs = wiphy_dereference(sta->local->hw.wiphy, sta->mscs);
	if (!mscs)
		return 0;

	spin_lock_bh(&mscs->lock);

	list_for_each_entry_safe(entry, tmp, &mscs->entries, list) {
		unsigned long dead = READ_ONCE(entry->last_update) +
				     mscs->timeout;

		if (time_after_eq(jiffies, dead)) {
			ieee80211_flow_entry_free(ap, entry);
			mscs->n_entries--;
			continue;
		}

		if (!delay || time_before(dead, jiffies + delay))
			delay = dead - jiffies;
	}

	spin_unlock_bh(&mscs->lock);

	return delay;
}

static void ieee80211_flow_gc_work(struct wiphy *wiphy, struct wiphy_work *work)
{
	struct ieee80211_sub_if_data *sdata;
	struct ieee80211_if_ap *ap;
	unsigned long delay = 0;
	struct sta_info *sta;

	ap = container_of(work, struct ieee80211_if_ap, flow_gc_work.work);
	sdata = container_of(ap, struct ieee80211_sub_if_data, u.ap);

	list_for_each_entry(sta, &sdata->local->sta_list, list) {
		unsigned long sta_delay;

		if (sta->sdata->bss != ap)
			continue;

		sta_delay = ieee80211_flow_gc_sta(ap, sta);
		if (sta_delay && (!delay || sta_delay < delay))
			delay = sta_delay;
	}

	if (delay)
		wiphy_delayed_work_queue(wiphy, &ap->flow_gc_work, delay);
}

int ieee80211_flow_tbl_init(struct ieee80211_sub_if_data *sdata)
{
	wiphy_delayed_work_init(&sdata->u.ap.flow_gc_work,
				ieee80211_flow_gc_work);

	return rhashtable_init(&sdata->u.ap.flow_tbl, &flow_rht_params);
}

static void ieee80211_flow_tbl_free(void *ptr, void *arg)
{
	struct ieee80211_flow_entry *entry = ptr;

	kfree(entry);
}

void ieee80211_flow_tbl_destroy(struct ieee80211_sub_if_data *sdata)
{
	wiphy_delayed_work_cancel(sdata->local->hw.wiphy,
				  &sdata->u.ap.flow_gc_work);
	rhashtable_free_and_destroy(&sdata->u.ap.flow_tbl,
				    ieee80211_flow_tbl_free, NULL);
}

static void ieee80211_flow_insert(struct ieee80211_sub_if_data *sdata,
				  struct ieee80211_mscs_sta *mscs,
				  const struct ieee80211_flow_hkey *hkey, u8 up)
{
	struct ieee80211_flow_entry *entry, *other;
	bool first;

	entry = kzalloc_obj(*entry, GFP_ATOMIC);
	if (!entry)
		return;

	entry->hkey.sta = hkey->sta;
	entry->hkey.key = hkey->key;
	entry->up = up;
	entry->last_update = jiffies;

	spin_lock_bh(&mscs->lock);

	/* Another MSCS may have been installed, which emptied this list */
	if (rcu_dereference(hkey->sta->mscs) != mscs)
		goto drop;

	if (mscs->n_entries >= IEEE80211_MSCS_ENTRIES_MAX)
		goto drop;

	other = rhashtable_lookup_get_insert_fast(&sdata->bss->flow_tbl,
						  &entry->node,
						  flow_rht_params);
	if (other) {
		/* Concurrent insert of the same key, the last writer wins */
		if (!IS_ERR(other)) {
			WRITE_ONCE(other->up, up);
			WRITE_ONCE(other->last_update, jiffies);
		}

		goto drop;
	}

	list_add_tail(&entry->list, &mscs->entries);
	first = mscs->n_entries++ == 0;
	spin_unlock_bh(&mscs->lock);

	/* The work requeues itself while entries remain */
	if (first)
		wiphy_delayed_work_queue(sdata->local->hw.wiphy,
					 &sdata->bss->flow_gc_work, 1);

	return;

drop:
	spin_unlock_bh(&mscs->lock);
	kfree(entry);
}

/**
 * ieee80211_flow_learn - record the user priority of an uplink flow
 *
 * @rx: the received frame, in IEEE 802.3 format
 *
 * The key is built for the downlink direction, so that the transmit path
 * can look it up as is.
 */
void ieee80211_flow_learn(struct ieee80211_rx_data *rx)
{
	struct ieee80211_sub_if_data *sdata = rx->sdata;
	const struct ethhdr *eth = (void *)rx->skb->data;
	struct ieee80211_flow_hkey hkey;
	struct ieee80211_mscs_sta *mscs;
	struct ieee80211_flow_entry *entry;
	struct cfg80211_flow_info info;
	/* The TID from ieee80211_parse_qos(), or from a decap offload driver */
	u32 up = rx->skb->priority;

	if (sdata->vif.type != NL80211_IFTYPE_AP &&
	    sdata->vif.type != NL80211_IFTYPE_AP_VLAN)
		return;

	mscs = rcu_dereference(rx->sta->mscs);
	if (!mscs)
		return;

	if (up > 7 || !(mscs->up_bitmap & BIT(up)))
		return;

	if (is_multicast_ether_addr(eth->h_dest) ||
	    !ether_addr_equal(eth->h_source, rx->sta->addr))
		return;

	if (!cfg80211_flow_parse(rx->skb, &info))
		return;

	hkey.sta = rx->sta;
	if (!cfg80211_flow_key_build(&info, mscs->layout,
				     CFG80211_FLOW_MIRRORED, &hkey.key))
		return;

	entry = rhashtable_lookup(&sdata->bss->flow_tbl, &hkey,
				  flow_rht_params);
	if (entry) {
		WRITE_ONCE(entry->up, up);
		WRITE_ONCE(entry->last_update, jiffies);
		return;
	}

	ieee80211_flow_insert(sdata, mscs, &hkey, up);
}

int ieee80211_set_scs(struct wiphy *wiphy, struct net_device *dev,
		      const u8 *peer, struct cfg80211_scs_desc * const *desc,
		      struct cfg80211_scs_result *res, u8 n_desc)
{
	struct ieee80211_sub_if_data *sdata = IEEE80211_DEV_TO_SUB_IF(dev);
	struct ieee80211_scs_sta *scs, *old;
	struct cfg80211_scs_desc **rule;
	struct sta_info *sta;
	unsigned int n_alloc;
	u8 i, n_rules = 0;
	int ret;

	lockdep_assert_wiphy(wiphy);

	sta = sta_info_get_bss(sdata, peer);
	if (!sta)
		return -ENOENT;

	old = wiphy_dereference(wiphy, sta->scs);

	/* One rule per SCSID bounds the size of the new set */
	n_alloc = (old ? old->n_rules : 0) + n_desc;

	rule = kcalloc(n_alloc, sizeof(*rule), GFP_KERNEL);
	if (!rule)
		return -ENOMEM;

	/* 255 rules with 255 classifiers each exceed what kmalloc hands out */
	scs = kvzalloc(ieee80211_scs_sta_size(old, desc, n_desc, n_alloc),
		       GFP_KERNEL);
	if (!scs) {
		ret = -ENOMEM;
		goto free;
	}

	if (sdata->local->ops->sta_set_scs && sta->uploaded) {
		ret = drv_sta_set_scs(sdata->local, sdata, sta, desc, res,
				      n_desc);
		if (ret)
			goto free;
	} else {
		/* Without a driver, no traffic description can be served */
		for (i = 0; i < n_desc; i++)
			if (desc[i]->qos_char)
				res[i].status = WLAN_STATUS_REQUEST_DECLINED;
	}

	if (old) {
		n_rules = old->n_rules;
		memcpy(rule, old->rule, n_rules * sizeof(*rule));
	}

	for (i = 0; i < n_desc; i++) {
		u8 at = ieee80211_scs_rule_find(rule, n_rules, desc[i]->id);

		if (desc[i]->req_type == NL80211_SCS_REQ_REMOVE) {
			/* A removal is answered even when no rule matched */
			res[i].status = WLAN_STATUS_TCLAS_PROCESSING_TERMINATED;

			if (at < n_rules) {
				n_rules--;
				memmove(&rule[at], &rule[at + 1],
					(n_rules - at) * sizeof(*rule));
			}

			continue;
		}

		if (res[i].status != WLAN_STATUS_SUCCESS)
			continue;

		if (at < n_rules)
			rule[at] = desc[i];
		else
			rule[n_rules++] = desc[i];
	}

	if (n_rules) {
		ieee80211_scs_sta_fill(scs, rule, n_rules, n_alloc);
	} else {
		kvfree(scs);
		scs = NULL;
	}

	kfree(rule);

	rcu_assign_pointer(sta->scs, scs);
	if (old)
		kvfree_rcu(old, rcu_head);

	return 0;

free:
	kvfree(scs);
	kfree(rule);

	return ret;
}

int ieee80211_set_mscs(struct wiphy *wiphy, struct net_device *dev,
		       const u8 *peer, struct cfg80211_mscs_desc *desc)
{
	struct ieee80211_sub_if_data *sdata = IEEE80211_DEV_TO_SUB_IF(dev);
	struct ieee80211_mscs_sta *mscs = NULL, *old;
	struct sta_info *sta;
	u32 tu;

	lockdep_assert_wiphy(wiphy);

	/* Only an AP provides MSCS */
	if (!sdata->bss)
		return -EOPNOTSUPP;

	sta = sta_info_get_bss(sdata, peer);
	if (!sta)
		return -ENOENT;

	if (desc->req_type != NL80211_SCS_REQ_REMOVE) {
		mscs = kzalloc_obj(*mscs);
		if (!mscs)
			return -ENOMEM;

		mscs->layout = desc->fields;
		mscs->up_bitmap = desc->up_bitmap;
		mscs->up_limit = desc->up_limit;

		/*
		 * Cap at an hour to keep tu * 1024 within the unsigned int
		 * that usecs_to_jiffies() takes. A timeout of zero jiffies
		 * would expire an entry at the next collection.
		 */
		tu = min(desc->stream_timeout, IEEE80211_MSCS_TIMEOUT_MAX_TU);
		mscs->timeout = max(1UL, usecs_to_jiffies(tu * 1024));

		spin_lock_init(&mscs->lock);
		INIT_LIST_HEAD(&mscs->entries);
	}

	old = wiphy_dereference(wiphy, sta->mscs);
	rcu_assign_pointer(sta->mscs, mscs);

	if (old) {
		/* The entries were keyed with the old layout */
		spin_lock_bh(&old->lock);
		ieee80211_flow_purge(sta->sdata->bss, old);
		spin_unlock_bh(&old->lock);

		kfree_rcu(old, rcu_head);
	}

	return 0;
}

/*
 * A restarted device lost every stream, so program them again. A refusal
 * changes nothing here: the rule stays and mac80211 keeps classifying for it.
 */
void ieee80211_sta_scs_reconfig(struct sta_info *sta)
{
	struct ieee80211_local *local = sta->local;
	struct ieee80211_scs_sta *scs;
	u8 i;

	lockdep_assert_wiphy(local->hw.wiphy);

	scs = wiphy_dereference(local->hw.wiphy, sta->scs);
	if (!scs)
		return;

	/* One rule per call, so no result array has to be allocated */
	for (i = 0; i < scs->n_rules; i++) {
		struct cfg80211_scs_result res = {};

		drv_sta_set_scs(local, sta->sdata, sta, &scs->rule[i], &res, 1);
	}
}

void ieee80211_sta_scs_free(struct sta_info *sta)
{
	struct ieee80211_mscs_sta *mscs;
	struct ieee80211_scs_sta *scs;

	/* Nothing can reach the station any more, so no lock is needed */
	scs = rcu_dereference_raw(sta->scs);
	RCU_INIT_POINTER(sta->scs, NULL);
	kvfree(scs);

	mscs = rcu_dereference_raw(sta->mscs);
	RCU_INIT_POINTER(sta->mscs, NULL);
	if (!mscs)
		return;

	ieee80211_flow_purge(sta->sdata->bss, mscs);
	kfree(mscs);
}
