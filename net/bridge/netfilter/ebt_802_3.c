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
#include <linux/stddef.h>
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
	unsigned int hlen;
	__be16 type;

	/* Without SAP or TYPE constraints the match only selects 802.3
	 * frames, which ebt_basic_match() has already decided from the
	 * ethertype/length field. Nothing in the LLC header is read.
	 */
	if (!(info->bitmask & (EBT_802_3_SAP | EBT_802_3_TYPE)))
		return true;

	/* Only the dsap/ssap pair is examined for a SAP-only rule; the
	 * type field (and the control byte that selects between the ui
	 * and ni views of the union) is needed for a TYPE rule alone.
	 */
	hlen = sizeof(_frame);
	if (!(info->bitmask & EBT_802_3_TYPE))
		hlen = offsetof(struct ebt_802_3_hdr, llc.ui.ctrl);

	/* skb->data sits past the Ethernet header on the receive path, so
	 * the frame starts at a negative offset from skb->data. Pass the
	 * mac header offset so the check follows the frame wherever the
	 * hook reset the mac header, and covers paged frames too. A frame
	 * too short to hold the LLC fields simply does not match; users
	 * who want such frames dropped can match 802.3 alone.
	 */
	frame = skb_header_pointer(skb, skb_mac_offset(skb), hlen, &_frame);
	if (!frame)
		return false;

	if (info->bitmask & EBT_802_3_SAP) {
		if (NF_INVF(info, EBT_802_3_SAP, info->sap != frame->llc.ui.ssap))
			return false;
		if (NF_INVF(info, EBT_802_3_SAP, info->sap != frame->llc.ui.dsap))
			return false;
	}

	if (info->bitmask & EBT_802_3_TYPE) {
		type = frame->llc.ui.ctrl & IS_UI ? frame->llc.ui.type :
						    frame->llc.ni.type;
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
