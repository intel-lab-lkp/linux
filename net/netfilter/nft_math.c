// SPDX-License-Identifier: GPL-2.0

#include <net/netlink.h>
#include <net/netfilter/nf_tables.h>

struct nft_math {
	u8			sreg;
	u8			dreg;
	u32			bitmask;
	enum nft_math_op	op;
};

static const struct nla_policy nft_math_policy[NFTA_MATH_MAX + 1] = {
	[NFTA_MATH_SREG]	= { .type = NLA_U32 },
	[NFTA_MATH_DREG]	= { .type = NLA_U32 },
	[NFTA_MATH_OP]		= { .type = NLA_U32 },
	[NFTA_MATH_BITMASK]	= { .type = NLA_U32 },
};

static void nft_math_eval_bitmask(u32 *src, u32 *dst,
				  const struct nft_math *priv)
{
	u32 target, keep, bit_unit;

	target = *src & priv->bitmask;
	keep = *src & ~priv->bitmask;
	bit_unit = priv->bitmask & -priv->bitmask;

	switch (priv->op) {
	case NFT_MATH_OP_INC:
		if (target == priv->bitmask) {
			*dst = *src;
			break;
		}

		target = target + bit_unit;
		*dst = target | keep;
		break;
	case NFT_MATH_OP_DEC:
		if (!target) {
			*dst = *src;
			break;
		}

		target = target - bit_unit;
		*dst = target | keep;
		break;
	default:
		DEBUG_NET_WARN_ONCE(true, "unknown operation path in nft_math");
		*dst = *src;
		break;
	}
}

static void nft_math_eval(const struct nft_expr *expr,
			  struct nft_regs *regs,
			  const struct nft_pktinfo *pkt)
{
	const struct nft_math *priv = nft_expr_priv(expr);
	u32 *src = &regs->data[priv->sreg];
	u32 *dst = &regs->data[priv->dreg];

	nft_math_eval_bitmask(src, dst, priv);
}

static int nft_math_init(const struct nft_ctx *ctx,
			 const struct nft_expr *expr,
			 const struct nlattr * const tb[])
{
	struct nft_math *priv = nft_expr_priv(expr);
	u32 bitmask_check;
	int err;
	u32 op;

	if (!tb[NFTA_MATH_SREG] ||
	    !tb[NFTA_MATH_DREG] ||
	    !tb[NFTA_MATH_BITMASK] ||
	    !tb[NFTA_MATH_OP])
		return -EINVAL;

	op = nla_get_u32(tb[NFTA_MATH_OP]);
	if (op > NFT_MATH_OP_MAX)
		return -EOPNOTSUPP;
	priv->op = op;

	priv->bitmask = nla_get_u32(tb[NFTA_MATH_BITMASK]);
	if (!priv->bitmask)
		return -EINVAL;

	/* check if the bitmask is contiguous, otherwise reject it */
	bitmask_check = priv->bitmask + (priv->bitmask & -priv->bitmask);
	if (bitmask_check & (bitmask_check - 1))
		return -EINVAL;

	err = nft_parse_register_load(ctx, tb[NFTA_MATH_SREG], &priv->sreg,
				      sizeof(u32));
	if (err < 0)
		return err;

	return nft_parse_register_store(ctx, tb[NFTA_MATH_DREG],
					&priv->dreg, NULL, NFT_DATA_VALUE,
					sizeof(u32));
}

static int nft_math_dump(struct sk_buff *skb,
			 const struct nft_expr *expr, bool reset)
{
	const struct nft_math *priv = nft_expr_priv(expr);

	if (nft_dump_register(skb, NFTA_MATH_SREG, priv->sreg))
		goto nla_put_failure;
	if (nft_dump_register(skb, NFTA_MATH_DREG, priv->dreg))
		goto nla_put_failure;
	if (nla_put_u32(skb, NFTA_MATH_BITMASK, priv->bitmask))
		goto nla_put_failure;
	if (nla_put_u32(skb, NFTA_MATH_OP, priv->op))
		goto nla_put_failure;
	return 0;

nla_put_failure:
	return -1;
}

static struct nft_expr_type nft_math_type;
static const struct nft_expr_ops nft_math_op = {
	.eval		= nft_math_eval,
	.size		= NFT_EXPR_SIZE(sizeof(struct nft_math)),
	.init		= nft_math_init,
	.dump		= nft_math_dump,
	.type		= &nft_math_type,
};

static struct nft_expr_type nft_math_type __read_mostly = {
	.ops		= &nft_math_op,
	.name		= "math",
	.owner		= THIS_MODULE,
	.policy		= nft_math_policy,
	.maxattr	= NFTA_MATH_MAX,
};

static int __init nft_math_module_init(void)
{
	return nft_register_expr(&nft_math_type);
}

static void __exit nft_math_module_exit(void)
{
	nft_unregister_expr(&nft_math_type);
}

module_init(nft_math_module_init);
module_exit(nft_math_module_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Fernando Fernandez Mancera <fmancera@suse.de>");
MODULE_ALIAS_NFT_EXPR("math");
MODULE_DESCRIPTION("nftables math support to operate with values");
