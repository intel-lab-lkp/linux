// SPDX-License-Identifier: GPL-2.0-only
/*
 * Stream classification service (SCS) and mirrored SCS (MSCS)
 *
 * Copyright (C) 2026 Felix Fietkau <nbd@nbd.name>
 */
#include <linux/ieee80211.h>
#include <linux/module.h>
#include <net/cfg80211.h>
#include "ieee80211_i.h"
#include "sta_info.h"

/* An hour, in TUs of 1024 microseconds */
#define IEEE80211_MSCS_TIMEOUT_MAX_TU	3515625

/*
 * Only the station itself can fill its own table, because every entry comes
 * from one of its uplink frames, so the bound is per station and needs no
 * admission rule. See bss_entries_limit in net/wireless/scan.c for the same
 * shape of knob.
 */
static u32 mscs_entries_limit = 64;
module_param(mscs_entries_limit, uint, 0644);
MODULE_PARM_DESC(mscs_entries_limit, "limit on learned MSCS flows per station");

/* The hash covers the peer and the key, and not the padding behind them */
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

/* Copy one rule to @pos and return the aligned octet after it */
static void *ieee80211_scs_rule_copy(const struct cfg80211_scs_desc *src,
				     void *pos)
{
	size_t len = struct_size(src, tclas, src->n_tclas);
	struct cfg80211_scs_desc *dst = pos;

	memcpy(dst, src, len);
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

/*
 * One request is an edit of the rule set, so the result mixes descriptors of
 * this request with descriptors of earlier ones. @rule therefore holds a mix
 * of pointers into the old set and pointers the caller owns, and the block
 * built from it owns a copy of every one of them.
 */
static struct ieee80211_scs_sta *
ieee80211_scs_sta_build(struct cfg80211_scs_desc * const *rule, u8 n_rules)
{
	struct ieee80211_scs_sta *scs;
	size_t head, size;
	void *pos;
	u8 i;

	if (!n_rules)
		return NULL;

	head = ALIGN(struct_size(scs, rule, n_rules), __alignof__(**rule));

	size = head;
	for (i = 0; i < n_rules; i++)
		size += ieee80211_scs_rule_size(rule[i]);

	/*
	 * A station may hold 255 rules of 255 classifiers, which is past what
	 * the page allocator hands out in one piece.
	 */
	scs = kvzalloc(size, GFP_KERNEL);
	if (!scs)
		return ERR_PTR(-ENOMEM);

	/* Set before the array is filled, for __counted_by() */
	scs->n_rules = n_rules;

	pos = (void *)scs + head;
	for (i = 0; i < n_rules; i++) {
		scs->rule[i] = pos;
		pos = ieee80211_scs_rule_copy(rule[i], pos);
	}

	return scs;
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

	/* 11.25.3 item d) 2) leaves an MSDU of an SCS stream to that stream */
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

/* Drop everything one station learned, with @mscs->lock held if it can race */
static void ieee80211_flow_purge(struct ieee80211_if_ap *ap,
				 struct ieee80211_mscs_sta *mscs)
{
	struct ieee80211_flow_entry *entry, *tmp;

	list_for_each_entry_safe(entry, tmp, &mscs->entries, list)
		ieee80211_flow_entry_free(ap, entry);

	mscs->n_entries = 0;
}

/*
 * 9.4.2.242 calls the Stream Timeout a minimum for keeping a variable, so an
 * entry is dropped once it has been idle for longer than that. Each run
 * reports how long the earliest survivor has left, and the work sleeps for
 * that; with nothing left it stops until a station learns again.
 */
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

		/* A survivor has at least one jiffy left, so zero means none */
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

	/*
	 * The station may have been given another MSCS since this one was
	 * read, and that one took this list apart under the same lock.
	 */
	if (rcu_dereference(hkey->sta->mscs) != mscs)
		goto drop;

	if (mscs->n_entries >= mscs_entries_limit)
		goto drop;

	other = rhashtable_lookup_get_insert_fast(&sdata->bss->flow_tbl,
						  &entry->node,
						  flow_rht_params);
	if (other) {
		/*
		 * Another CPU learned the same tuple first, which a coarse
		 * mask makes ordinary. 11.25.3 item c) 3) discards the
		 * previous value, so either writer may win. A resize gives an
		 * error instead, and the next frame of the flow learns again.
		 */
		if (!IS_ERR(other))
			WRITE_ONCE(other->up, up);

		goto drop;
	}

	list_add_tail(&entry->list, &mscs->entries);
	first = mscs->n_entries++ == 0;
	spin_unlock_bh(&mscs->lock);

	/*
	 * The collector reschedules itself while any entry is left, so it only
	 * has to be woken when a station fills an empty list. Waking it at once
	 * rather than at this station's deadline is deliberate: the queue is a
	 * mod_timer(), so a later deadline would displace an earlier one.
	 */
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
 * The AP mirrors the priority a station gives its own uplink onto the
 * downlink of the same flow, so the key is built for the reverse direction
 * and the transmit path finds it without a second layout.
 */
void ieee80211_flow_learn(struct ieee80211_rx_data *rx)
{
	struct ieee80211_sub_if_data *sdata = rx->sdata;
	const struct ethhdr *eth = (void *)rx->skb->data;
	struct ieee80211_flow_hkey hkey;
	struct ieee80211_mscs_sta *mscs;
	struct ieee80211_flow_entry *entry;
	struct cfg80211_flow_info info;
	/*
	 * The TID that ieee80211_parse_qos() recorded, or for a frame the
	 * device decapped, whatever the driver put there. The forwarding path
	 * a few lines below the caller reads the same field.
	 */
	u32 up = rx->skb->priority;

	if (sdata->vif.type != NL80211_IFTYPE_AP &&
	    sdata->vif.type != NL80211_IFTYPE_AP_VLAN)
		return;

	mscs = rcu_dereference(rx->sta->mscs);
	if (!mscs)
		return;

	/*
	 * 11.25.3 item c) learns from an individually addressed MSDU that the
	 * station sent itself, and only for a user priority its request named.
	 */
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
	u8 i, n_rules = 0;

	lockdep_assert_wiphy(wiphy);

	sta = sta_info_get_bss(sdata, peer);
	if (!sta)
		return -ENOENT;

	old = wiphy_dereference(wiphy, sta->scs);

	/*
	 * Every rule carries an SCSID of its own and an SCSID is 1 to 255, so
	 * this bounds the working array, and the resulting set with it.
	 */
	rule = kcalloc((old ? old->n_rules : 0) + n_desc, sizeof(*rule),
		       GFP_KERNEL);
	if (!rule)
		return -ENOMEM;

	if (old) {
		n_rules = old->n_rules;
		memcpy(rule, old->rule, n_rules * sizeof(*rule));
	}

	for (i = 0; i < n_desc; i++) {
		u8 at = ieee80211_scs_rule_find(rule, n_rules, desc[i]->id);

		if (desc[i]->req_type == NL80211_SCS_REQ_REMOVE) {
			/* 11.25.2 answers any removal with its own status */
			res[i].status = WLAN_STATUS_TCLAS_PROCESSING_TERMINATED;

			if (at < n_rules) {
				n_rules--;
				memmove(&rule[at], &rule[at + 1],
					(n_rules - at) * sizeof(*rule));
			}

			continue;
		}

		if (at < n_rules)
			rule[at] = desc[i];
		else
			rule[n_rules++] = desc[i];
	}

	scs = ieee80211_scs_sta_build(rule, n_rules);
	kfree(rule);
	if (IS_ERR(scs))
		return PTR_ERR(scs);

	rcu_assign_pointer(sta->scs, scs);
	if (old)
		kvfree_rcu(old, rcu_head);

	return 0;
}

int ieee80211_set_mscs(struct wiphy *wiphy, struct net_device *dev,
		       const u8 *peer, struct cfg80211_mscs_desc *desc)
{
	struct ieee80211_sub_if_data *sdata = IEEE80211_DEV_TO_SUB_IF(dev);
	struct ieee80211_mscs_sta *mscs = NULL, *old;
	struct sta_info *sta;
	u32 tu;

	lockdep_assert_wiphy(wiphy);

	/* 11.25.3 makes MSCS a service an AP provides, so it needs a BSS */
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
		 * The field is a u32 of TUs, up to about 50 days, which
		 * jiffies arithmetic cannot carry: time_after_eq() needs the
		 * difference to stay well inside the wrap. 11.25.3 only
		 * permits dropping a variable once the timeout has passed and
		 * nowhere requires keeping one until then, so capping is a
		 * choice about usefulness rather than conformance, and an idle
		 * hour leaves nothing worth keeping. A short timeout rounds
		 * down to nothing at a low HZ, and zero would mean never, so
		 * keep a jiffy.
		 */
		tu = min(desc->stream_timeout, IEEE80211_MSCS_TIMEOUT_MAX_TU);
		mscs->timeout = max(1UL, usecs_to_jiffies(tu * 1024));

		spin_lock_init(&mscs->lock);
		INIT_LIST_HEAD(&mscs->entries);
	}

	old = wiphy_dereference(wiphy, sta->mscs);
	rcu_assign_pointer(sta->mscs, mscs);

	if (old) {
		/*
		 * The keys of the learned entries were built with the layout
		 * that is going away, and 11.25.3 deletes the list of
		 * UP{tuple} variables on a teardown, so they go either way. A
		 * receive path that still holds the old MSCS takes the same
		 * lock, sees that the station has moved on and adds nothing.
		 */
		spin_lock_bh(&old->lock);
		ieee80211_flow_purge(sta->sdata->bss, old);
		spin_unlock_bh(&old->lock);

		kfree_rcu(old, rcu_head);
	}

	return 0;
}

void ieee80211_sta_scs_free(struct sta_info *sta)
{
	struct ieee80211_mscs_sta *mscs;
	struct ieee80211_scs_sta *scs;

	scs = rcu_dereference_raw(sta->scs);
	RCU_INIT_POINTER(sta->scs, NULL);
	kvfree(scs);

	mscs = rcu_dereference_raw(sta->mscs);
	RCU_INIT_POINTER(sta->mscs, NULL);
	if (!mscs)
		return;

	/*
	 * sta_info_free() runs a grace period after the station left the hash,
	 * so nothing can reach this list any more and it needs no lock.
	 */
	ieee80211_flow_purge(sta->sdata->bss, mscs);
	kfree(mscs);
}
