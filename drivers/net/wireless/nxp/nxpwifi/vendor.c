// SPDX-License-Identifier: GPL-2.0-only
/*
 * NXP Wireless LAN device driver: vendor command interface
 *
 * Copyright 2026 NXP
 */

#include <net/mac80211.h>
#include <net/netlink.h>
#include "vendor.h"
#include "main.h"

/* Attributes for NXP_VENDOR_CMD_TXPOWER_LIMIT. */
static const struct nla_policy
nxpwifi_txpwrlimit_policy[NXP_ATTR_TXPWRLIMIT_MAX + 1] = {
	[NXP_ATTR_TXPWRLIMIT_ACTION] =
		NLA_POLICY_MAX(NLA_U8, NXP_TXPWRLIMIT_ACTION_SET),
	[NXP_ATTR_TXPWRLIMIT_SUBBAND] = { .type = NLA_U8 },
	[NXP_ATTR_TXPWRLIMIT_DATA] = { .type = NLA_BINARY,
				       .len  = NXPWIFI_MAX_TRPC_BUF },
};

static bool nxpwifi_txpwrlimit_subband_valid(u8 subband)
{
	switch (subband) {
	case NXP_TXPWRLIMIT_SUBBAND_2G:
	case NXP_TXPWRLIMIT_SUBBAND_5G_0:
	case NXP_TXPWRLIMIT_SUBBAND_5G_1:
	case NXP_TXPWRLIMIT_SUBBAND_5G_2:
	case NXP_TXPWRLIMIT_SUBBAND_5G_3:
		return true;
	default:
		return false;
	}
}

static int nxpwifi_vendor_txpower_limit(struct wiphy *wiphy,
					struct wireless_dev *wdev,
					const void *data, int data_len)
{
	struct nxpwifi_adapter *adapter = nxpwifi_cfg80211_get_adapter(wiphy);
	struct nlattr *tb[NXP_ATTR_TXPWRLIMIT_MAX + 1];
	struct nxpwifi_ds_chan_trpc_cfg *ch_trpc;
	struct nxpwifi_private *priv;
	struct sk_buff *resp;
	u8 action, subband = 0;
	u16 tlv_len = 0;
	int ret;

	ret = nla_parse(tb, NXP_ATTR_TXPWRLIMIT_MAX, data, data_len,
			nxpwifi_txpwrlimit_policy, NULL);
	if (ret)
		return ret;

	if (!tb[NXP_ATTR_TXPWRLIMIT_ACTION])
		return -EINVAL;
	action = nla_get_u8(tb[NXP_ATTR_TXPWRLIMIT_ACTION]);

	if (tb[NXP_ATTR_TXPWRLIMIT_SUBBAND]) {
		subband = nla_get_u8(tb[NXP_ATTR_TXPWRLIMIT_SUBBAND]);
		if (!nxpwifi_txpwrlimit_subband_valid(subband))
			return -EINVAL;
	} else if (action == NXP_TXPWRLIMIT_ACTION_GET) {
		/* A subband selector is required for GET operations. */
		return -EINVAL;
	}

	if (action == NXP_TXPWRLIMIT_ACTION_SET) {
		if (!tb[NXP_ATTR_TXPWRLIMIT_DATA])
			return -EINVAL;
		tlv_len = nla_len(tb[NXP_ATTR_TXPWRLIMIT_DATA]);
	}

	priv = nxpwifi_get_priv(adapter, NXPWIFI_BSS_ROLE_STA);
	if (!priv)
		return -ENODEV;

	/* Reserve space for the returned TRPC table on GET requests. */
	ch_trpc = kzalloc_flex(*ch_trpc, tlvbuffer,
			       (action == NXP_TXPWRLIMIT_ACTION_SET) ?
			       tlv_len : NXPWIFI_MAX_TRPC_BUF, GFP_KERNEL);
	if (!ch_trpc)
		return -ENOMEM;

	ch_trpc->action = (action == NXP_TXPWRLIMIT_ACTION_SET) ?
			  HOST_ACT_GEN_SET : HOST_ACT_GEN_GET;
	ch_trpc->subband = subband;

	if (tlv_len)
		memcpy(ch_trpc->tlvbuffer,
		       nla_data(tb[NXP_ATTR_TXPWRLIMIT_DATA]), tlv_len);

	ch_trpc->len = sizeof(ch_trpc->action) + sizeof(ch_trpc->subband) +
		       tlv_len;

	ret = nxpwifi_chan_trpc_cfg(priv, NXPWIFI_SYNC_CMD, ch_trpc);
	if (ret)
		goto done;

	if (action == NXP_TXPWRLIMIT_ACTION_GET) {
		u16 hdr_len = sizeof(ch_trpc->action) +
			      sizeof(ch_trpc->subband);
		u16 resp_len, skb_len;

		if (ch_trpc->len < hdr_len ||
		    ch_trpc->len > hdr_len + NXPWIFI_MAX_TRPC_BUF) {
			ret = -EIO;
			goto done;
		}

		resp_len = ch_trpc->len - hdr_len;
		skb_len = nla_total_size(resp_len);

		resp = cfg80211_vendor_cmd_alloc_reply_skb(wiphy, skb_len);
		if (!resp) {
			ret = -ENOMEM;
			goto done;
		}

		if (nla_put(resp, NXP_ATTR_TXPWRLIMIT_DATA, resp_len,
			    ch_trpc->tlvbuffer)) {
			kfree_skb(resp);
			ret = -ENOBUFS;
			goto done;
		}

		ret = cfg80211_vendor_cmd_reply(resp);
	}

done:
	kfree(ch_trpc);
	return ret;
}

static const struct wiphy_vendor_command nxpwifi_vendor_commands[] = {
	{
		.info = {
			.vendor_id = NXP_OUI,
			.subcmd = NXP_VENDOR_CMD_TXPOWER_LIMIT,
		},
		.flags = 0,
		.doit = nxpwifi_vendor_txpower_limit,
		.policy = nxpwifi_txpwrlimit_policy,
		.maxattr = NXP_ATTR_TXPWRLIMIT_MAX,
	},
};

void nxpwifi_set_vendor_commands(struct wiphy *wiphy)
{
	wiphy->vendor_commands = nxpwifi_vendor_commands;
	wiphy->n_vendor_commands = ARRAY_SIZE(nxpwifi_vendor_commands);
}
