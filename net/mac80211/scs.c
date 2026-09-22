// SPDX-License-Identifier: GPL-2.0-only
/*
 * Stream classification service (SCS) and mirrored SCS (MSCS)
 *
 * Copyright (C) 2026 Felix Fietkau <nbd@nbd.name>
 */
#include <linux/ieee80211.h>
#include <net/cfg80211.h>
#include "ieee80211_i.h"
#include "sta_info.h"

/* An hour, in TUs of 1024 microseconds */
#define IEEE80211_MSCS_TIMEOUT_MAX_TU	3515625

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

/* @rule points into the old set and into the request, so copy each rule */
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

	/* 255 rules with 255 classifiers each exceed what kmalloc hands out */
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

	/* One rule per SCSID bounds the size of the new set */
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
			/* A removal is answered even when no rule matched */
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
	}

	old = wiphy_dereference(wiphy, sta->mscs);
	rcu_assign_pointer(sta->mscs, mscs);
	if (old)
		kfree_rcu(old, rcu_head);

	return 0;
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
	kfree(mscs);
}
