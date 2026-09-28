// SPDX-License-Identifier: GPL-2.0-only
/*
 * 802_3
 *
 * Author:
 * Chris Vitale csv@bluetail.com
 *
 * May 2003
 *
 */
#include <linux/module.h>
#include <linux/netfilter/x_tables.h>
#include <linux/netfilter_bridge/ebtables.h>
#include <linux/skbuff.h>
#include <uapi/linux/netfilter_bridge/ebt_802_3.h>

static bool
ebt_802_3_mt(const struct sk_buff *skb, struct xt_action_param *par)
{
	const struct ebt_802_3_info *info = par->matchinfo;
	struct ebt_802_3_hdr _frame;
	const struct ebt_802_3_hdr *frame;
	int mac_offset = skb_mac_offset(skb);
	__be16 type;

	/* skb->data sits past the Ethernet header when the match runs, so the
	 * frame starts before skb->data. skb_header_pointer() takes an offset
	 * relative to skb->data and cannot express that, so check the linear
	 * area against the mac header ourselves and use skb_mac_header()
	 * directly once the check has passed.
	 */
	if (mac_offset < 0)
		mac_offset = -mac_offset;
	if (mac_offset + sizeof(_frame) > skb_headlen(skb)) {
		/* The frame is too short to hold an LLC header. */
		par->hotdrop = true;
		return false;
	}

	frame = (const struct ebt_802_3_hdr *)skb_mac_header(skb);
	type = frame->llc.ui.ctrl & IS_UI ? frame->llc.ui.type :
					    frame->llc.ni.type;

	if (info->bitmask & EBT_802_3_SAP) {
		if (NF_INVF(info, EBT_802_3_SAP, info->sap != frame->llc.ui.ssap))
			return false;
		if (NF_INVF(info, EBT_802_3_SAP, info->sap != frame->llc.ui.dsap))
			return false;
	}

	if (info->bitmask & EBT_802_3_TYPE) {
		if (!(frame->llc.ui.dsap == CHECK_TYPE &&
		      frame->llc.ui.ssap == CHECK_TYPE))
			return false;
		if (NF_INVF(info, EBT_802_3_TYPE, info->type != type))
			return false;
	}

	return true;
}

static int ebt_802_3_mt_check(const struct xt_mtchk_param *par)
{
	const struct ebt_802_3_info *info = par->matchinfo;

	if (info->bitmask & ~EBT_802_3_MASK || info->invflags & ~EBT_802_3_MASK)
		return -EINVAL;

	return 0;
}

static struct xt_match ebt_802_3_mt_reg __read_mostly = {
	.name		= "802_3",
	.revision	= 0,
	.family		= NFPROTO_BRIDGE,
	.match		= ebt_802_3_mt,
	.checkentry	= ebt_802_3_mt_check,
	.matchsize	= sizeof(struct ebt_802_3_info),
	.me		= THIS_MODULE,
};

static int __init ebt_802_3_init(void)
{
	return xt_register_match(&ebt_802_3_mt_reg);
}

static void __exit ebt_802_3_fini(void)
{
	xt_unregister_match(&ebt_802_3_mt_reg);
}

module_init(ebt_802_3_init);
module_exit(ebt_802_3_fini);
MODULE_DESCRIPTION("Ebtables: DSAP/SSAP field and SNAP type matching");
MODULE_LICENSE("GPL");
