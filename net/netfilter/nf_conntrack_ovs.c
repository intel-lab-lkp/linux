// SPDX-License-Identifier: GPL-2.0-only
/* Support ct functions for openvswitch and used by OVS and TC conntrack. */

#include <net/netfilter/nf_conntrack_helper.h>
#include <net/netfilter/nf_conntrack_seqadj.h>
#include <net/netfilter/ipv6/nf_defrag_ipv6.h>
#include <net/ipv6_frag.h>
#include <net/ip.h>
#include <linux/netfilter_ipv6.h>

/* 'skb' should already be pulled to nh_ofs. */
int nf_ct_helper(struct sk_buff *skb, struct nf_conn *ct,
		 enum ip_conntrack_info ctinfo, u16 proto)
{
	const struct nf_conntrack_helper *helper;
	const struct nf_conn_help *help;

	if (ctinfo == IP_CT_RELATED_REPLY)
		return NF_ACCEPT;

	help = nfct_help(ct);
	if (!help)
		return NF_ACCEPT;

	helper = rcu_dereference(help->helper);
	if (!helper)
		return NF_ACCEPT;

	if (helper->nfproto != NFPROTO_UNSPEC &&
	    helper->nfproto != proto)
		return NF_ACCEPT;

	return nf_ct_call_helper(skb, ct, ctinfo);
}
EXPORT_SYMBOL_GPL(nf_ct_helper);

int nf_ct_add_helper(struct nf_conn *ct, const char *name, u8 family,
		     u8 proto, bool nat, struct nf_conntrack_helper **hp)
{
	struct nf_conntrack_helper *helper;
	struct nf_conn_help *help;
	int ret = 0;

	helper = nf_conntrack_helper_try_module_get(name, family, proto);
	if (!helper)
		return -EINVAL;

	help = nf_ct_helper_ext_add(ct, GFP_KERNEL);
	if (!help) {
		nf_conntrack_helper_put(helper);
		return -ENOMEM;
	}
#if IS_ENABLED(CONFIG_NF_NAT)
	if (nat) {
		ret = nf_nat_helper_try_module_get(name, family, proto);
		if (ret) {
			nf_conntrack_helper_put(helper);
			return ret;
		}
	}
#endif
	rcu_assign_pointer(help->helper, helper);
	*hp = helper;
	return ret;
}
EXPORT_SYMBOL_GPL(nf_ct_add_helper);

/* Trim the skb to the length specified by the IP/IPv6 header,
 * removing any trailing lower-layer padding. This prepares the skb
 * for higher-layer processing that assumes skb->len excludes padding
 * (such as nf_ip_checksum). The caller needs to pull the skb to the
 * network header, and ensure ip_hdr/ipv6_hdr points to valid data.
 */
int nf_ct_skb_network_trim(struct sk_buff *skb, int family)
{
	unsigned int len;

	switch (family) {
	case NFPROTO_IPV4:
		len = skb_ip_totlen(skb);
		break;
	case NFPROTO_IPV6:
		len = skb_ipv6_payload_len(skb);
		if (ipv6_hdr(skb)->nexthdr == NEXTHDR_HOP) {
			int err = nf_ip6_check_hbh_len(skb, &len);

			if (err)
				return err;
		}
		len += sizeof(struct ipv6hdr);
		break;
	default:
		len = skb->len;
	}

	return pskb_trim_rcsum(skb, len);
}
EXPORT_SYMBOL_GPL(nf_ct_skb_network_trim);

/* Returns 0 on success, -EINPROGRESS if 'skb' is stolen, or other nonzero
 * value if 'skb' is freed.
 */
int nf_ct_handle_fragments(struct net *net, struct sk_buff *skb,
			   u16 zone, u8 family, u8 *proto, u16 *mru)
{
	int err;

	if (family == NFPROTO_IPV4) {
		enum ip_defrag_users user = IP_DEFRAG_CONNTRACK_IN + zone;

		memset(IPCB(skb), 0, sizeof(struct inet_skb_parm));
		local_bh_disable();
		err = ip_defrag(net, skb, user);
		local_bh_enable();
		if (err)
			return err;

		*mru = IPCB(skb)->frag_max_size;
#if IS_ENABLED(CONFIG_NF_DEFRAG_IPV6)
	} else if (family == NFPROTO_IPV6) {
		enum ip6_defrag_users user = IP6_DEFRAG_CONNTRACK_IN + zone;

		memset(IP6CB(skb), 0, sizeof(struct inet6_skb_parm));
		err = nf_ct_frag6_gather(net, skb, user);
		if (err) {
			if (err != -EINPROGRESS)
				kfree_skb(skb);
			return err;
		}

		*proto = ipv6_hdr(skb)->nexthdr;
		*mru = IP6CB(skb)->frag_max_size;
#endif
	} else {
		kfree_skb(skb);
		return -EPFNOSUPPORT;
	}

	skb_clear_hash(skb);
	skb->ignore_df = 1;

	return 0;
}
EXPORT_SYMBOL_GPL(nf_ct_handle_fragments);
