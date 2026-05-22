// SPDX-License-Identifier: GPL-2.0-only
/* (C) 2001-2002 Magnus Boden <mb@ozaba.mine.nu>
 * (C) 2006-2012 Patrick McHardy <kaber@trash.net>
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/in.h>
#include <linux/udp.h>
#include <linux/netfilter.h>

#include <net/netfilter/nf_conntrack.h>
#include <net/netfilter/nf_conntrack_tuple.h>
#include <net/netfilter/nf_conntrack_expect.h>
#include <net/netfilter/nf_conntrack_ecache.h>
#include <net/netfilter/nf_conntrack_helper.h>
#include <linux/netfilter/nf_conntrack_tftp.h>

#define HELPER_NAME "tftp"

MODULE_AUTHOR("Magnus Boden <mb@ozaba.mine.nu>");
MODULE_DESCRIPTION("TFTP connection tracking helper");
MODULE_LICENSE("GPL");
MODULE_ALIAS("ip_conntrack_tftp");
MODULE_ALIAS_NFCT_HELPER(HELPER_NAME);

nf_nat_tftp_hook_fn __rcu *nf_nat_tftp_hook __read_mostly;
EXPORT_SYMBOL_GPL(nf_nat_tftp_hook);

static int tftp_help(struct sk_buff *skb,
		     unsigned int protoff,
		     struct nf_conn *ct,
		     enum ip_conntrack_info ctinfo)
{
	const struct tftphdr *tfh;
	struct tftphdr _tftph;
	struct nf_conntrack_expect *exp;
	struct nf_conntrack_tuple *tuple;
	unsigned int ret = NF_ACCEPT;
	nf_nat_tftp_hook_fn *nf_nat_tftp;

	tfh = skb_header_pointer(skb, protoff + sizeof(struct udphdr),
				 sizeof(_tftph), &_tftph);
	if (tfh == NULL)
		return NF_ACCEPT;

	switch (ntohs(tfh->opcode)) {
	case TFTP_OPCODE_READ:
	case TFTP_OPCODE_WRITE:
		/* RRQ and WRQ works the same way */
		nf_ct_dump_tuple(&ct->tuplehash[IP_CT_DIR_ORIGINAL].tuple);
		nf_ct_dump_tuple(&ct->tuplehash[IP_CT_DIR_REPLY].tuple);

		exp = nf_ct_expect_alloc(ct);
		if (exp == NULL) {
			nf_ct_helper_log(skb, ct, "cannot alloc expectation");
			return NF_DROP;
		}
		tuple = &ct->tuplehash[IP_CT_DIR_REPLY].tuple;
		nf_ct_expect_init(exp, NF_CT_EXPECT_CLASS_DEFAULT,
				  nf_ct_l3num(ct),
				  &tuple->src.u3, &tuple->dst.u3,
				  IPPROTO_UDP, NULL, &tuple->dst.u.udp.port);

		pr_debug("expect: ");
		nf_ct_dump_tuple(&exp->tuple);

		nf_nat_tftp = rcu_dereference(nf_nat_tftp_hook);
		if (nf_nat_tftp && ct->status & IPS_NAT_MASK)
			ret = nf_nat_tftp(skb, ctinfo, exp);
		else if (nf_ct_expect_related(exp, 0) != 0) {
			nf_ct_helper_log(skb, ct, "cannot add expectation");
			ret = NF_DROP;
		}
		nf_ct_expect_put(exp);
		break;
	case TFTP_OPCODE_DATA:
	case TFTP_OPCODE_ACK:
		pr_debug("Data/ACK opcode\n");
		break;
	case TFTP_OPCODE_ERROR:
		pr_debug("Error opcode\n");
		break;
	default:
		pr_debug("Unknown opcode\n");
	}
	return ret;
}

static const struct nf_conntrack_expect_policy tftp_exp_policy = {
	.max_expected	= 1,
	.timeout	= 5 * 60,
};

static struct nf_conntrack_helper tftp __read_mostly = {
	.name			= HELPER_NAME,
	.me			= THIS_MODULE,
	.nfproto		= NFPROTO_UNSPEC,
	.l4proto		= IPPROTO_UDP,
	.help			= tftp_help,
	.expect_policy		= &tftp_exp_policy,
};

static int __init nf_conntrack_tftp_init(void)
{
	NF_CT_HELPER_BUILD_BUG_ON(0);

	return nf_conntrack_helper_register(&tftp);
}

static void __exit nf_conntrack_tftp_fini(void)
{
	nf_conntrack_helper_unregister(&tftp);
}
module_init(nf_conntrack_tftp_init);
module_exit(nf_conntrack_tftp_fini);
