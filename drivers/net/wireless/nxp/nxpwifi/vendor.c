// SPDX-License-Identifier: GPL-2.0-only
/*
 * NXP Wireless LAN device driver: vendor command interface
 *
 * Copyright 2011-2026 NXP
 */

#include <net/mac80211.h>
#include <net/netlink.h>
#include "vendor.h"
#include "main.h"

static const struct nla_policy
nxpwifi_txpwr_policy[NXPWIFI_ATTR_TXPWR_MAX + 1] = {
	[NXPWIFI_ATTR_TXPWR_ACTION]  = NLA_POLICY_MAX(NLA_U8,
						      NXPWIFI_TXPWR_ACTION_SET),
	[NXPWIFI_ATTR_TXPWR_SUBBAND] = { .type = NLA_U8 },
	[NXPWIFI_ATTR_TXPWR_DATA]    = { .type = NLA_BINARY,
					 .len  = NXPWIFI_MAX_TRPC_BUF },
};

/* The subband selector is not a contiguous range: 0x00 selects the 2.4 GHz
 * band and 0x10..0x13 select the four 5 GHz subbands. Validate it explicitly
 * instead of relying on a simple min/max policy bound.
 */
static bool nxpwifi_txpwr_subband_valid(u8 subband)
{
	switch (subband) {
	case NXPWIFI_TXPWR_SUBBAND_2G:
	case NXPWIFI_TXPWR_SUBBAND_5G_0:
	case NXPWIFI_TXPWR_SUBBAND_5G_1:
	case NXPWIFI_TXPWR_SUBBAND_5G_2:
	case NXPWIFI_TXPWR_SUBBAND_5G_3:
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
	struct nlattr *tb[NXPWIFI_ATTR_TXPWR_MAX + 1];
	struct nxpwifi_ds_chan_trpc_cfg *ch_trpc;
	struct nxpwifi_private *priv;
	struct sk_buff *resp;
	u8 action, subband = 0;
	u16 tlv_len = 0;
	int ret;

	ret = nla_parse(tb, NXPWIFI_ATTR_TXPWR_MAX, data, data_len,
			nxpwifi_txpwr_policy, NULL);
	if (ret)
		return ret;

	if (!tb[NXPWIFI_ATTR_TXPWR_ACTION])
		return -EINVAL;
	action = nla_get_u8(tb[NXPWIFI_ATTR_TXPWR_ACTION]);

	if (tb[NXPWIFI_ATTR_TXPWR_SUBBAND]) {
		subband = nla_get_u8(tb[NXPWIFI_ATTR_TXPWR_SUBBAND]);
		if (!nxpwifi_txpwr_subband_valid(subband))
			return -EINVAL;
	} else if (action == NXPWIFI_TXPWR_ACTION_GET) {
		/* A GET selects which per-subband table to read back, so the
		 * subband selector is mandatory for a GET.
		 */
		return -EINVAL;
	}

	if (action == NXPWIFI_TXPWR_ACTION_SET) {
		if (!tb[NXPWIFI_ATTR_TXPWR_DATA])
			return -EINVAL;
		tlv_len = nla_len(tb[NXPWIFI_ATTR_TXPWR_DATA]);
	}

	priv = nxpwifi_get_priv(adapter, NXPWIFI_BSS_ROLE_STA);
	if (!priv)
		return -ENODEV;

	/* A SET only needs room for the supplied TLVs; a GET needs room for
	 * the firmware to write back the full per-subband table.
	 */
	ch_trpc = kzalloc_flex(*ch_trpc, tlvbuffer,
			       (action == NXPWIFI_TXPWR_ACTION_SET) ?
			       tlv_len : NXPWIFI_MAX_TRPC_BUF, GFP_KERNEL);
	if (!ch_trpc)
		return -ENOMEM;

	ch_trpc->action = (action == NXPWIFI_TXPWR_ACTION_SET) ?
			  HOST_ACT_GEN_SET : HOST_ACT_GEN_GET;
	ch_trpc->subband = subband;
	if (tlv_len)
		memcpy(ch_trpc->tlvbuffer,
		       nla_data(tb[NXPWIFI_ATTR_TXPWR_DATA]), tlv_len);
	/* Firmware payload is action + subband followed by the TLV table. */
	ch_trpc->len = sizeof(ch_trpc->action) + sizeof(ch_trpc->subband) +
		       tlv_len;

	ret = nxpwifi_chan_trpc_cfg(priv, NXPWIFI_SYNC_CMD, ch_trpc);
	if (ret)
		goto done;

	if (action == NXPWIFI_TXPWR_ACTION_GET) {
		u16 hdr_len = sizeof(ch_trpc->action) +
			      sizeof(ch_trpc->subband);
		u16 resp_len, skb_len;

		/* Guard against a malformed firmware response before using
		 * ch_trpc->len to size the reply.
		 */
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

		if (nla_put(resp, NXPWIFI_ATTR_TXPWR_DATA, resp_len,
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
			.subcmd = NXPWIFI_VENDOR_CMD_TXPOWER_LIMIT,
		},
		/* TX power limits are a configuration/calibration setting and
		 * can be programmed while the interface is down, so no
		 * WIPHY_VENDOR_CMD_NEED_RUNNING flag is required.
		 */
		.flags = 0,
		.doit = nxpwifi_vendor_txpower_limit,
		.policy = nxpwifi_txpwr_policy,
		.maxattr = NXPWIFI_ATTR_TXPWR_MAX,
	},
};

void nxpwifi_set_vendor_commands(struct wiphy *wiphy)
{
	wiphy->vendor_commands = nxpwifi_vendor_commands;
	wiphy->n_vendor_commands = ARRAY_SIZE(nxpwifi_vendor_commands);
}
