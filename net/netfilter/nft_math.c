// SPDX-License-Identifier: GPL-2.0

#include <net/netlink.h>
#include <net/netfilter/nf_tables.h>
#include <linux/netfilter/nf_tables.h>

struct nft_math {
	u8		 	sreg;
	u8			dreg;
	u32			len;
	u32			bitmask;
	enum nft_math_op	op;
};

static const struct nla_policy nft_math_policy[NFTA_MATH_MAX + 1] = {
	[NFTA_MATH_SREG]	= { .type = NLA_U32 },
	[NFTA_MATH_DREG]	= { .type = NLA_U32 },
	[NFTA_MATH_OP]		= { .type = NLA_U32 },
	[NFTA_MATH_BITMASK]	= { .type = NLA_U32 },
	[NFTA_MATH_LEN]		= NLA_POLICY_MIN(NLA_U32, 8),
};

static void nft_math_eval_bitmask(uint32_t *src, uint32_t *dst,
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
		break;
	}
}

static void nft_math_eval_u32(uint32_t *src, uint32_t *dst,
			      const struct nft_math *priv)
{
	switch (priv->op) {
	case NFT_MATH_OP_INC:
		if (*src != U32_MAX)
			*dst = *src + 1;
		else
			*dst = *src;
		break;
	case NFT_MATH_OP_DEC:
		if (*src != 0)
			*dst = *src - 1;
		else
			*dst = *src;
		break;
	default:
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

	switch (priv->len) {
	case 32:
		nft_math_eval_u32(src, dst, priv);
		break;
	default:
		nft_math_eval_bitmask(src, dst, priv);
		break;
	}
}

static int nft_math_init(const struct nft_ctx *ctx,
			 const struct nft_expr *expr,
			 const struct nlattr * const tb[])
{
	struct nft_math *priv = nft_expr_priv(expr);
	int err;

	if (tb[NFTA_MATH_SREG] == NULL ||
	    tb[NFTA_MATH_DREG] == NULL ||
	    tb[NFTA_MATH_LEN] == NULL ||
	    tb[NFTA_MATH_OP] == NULL)
		return -EINVAL;

	priv->op = ntohl(nla_get_u32(tb[NFTA_MATH_OP]));
	priv->len = ntohl(nla_get_u32(tb[NFTA_MATH_LEN]));

	if (tb[NFTA_MATH_BITMASK])
		priv->bitmask = ntohl(nla_get_u32(tb[NFTA_MATH_BITMASK]));

	if (priv->op > NFT_MATH_OP_MAX)
		return -EOPNOTSUPP;

	switch (priv->len) {
	case 8:
		if (!priv->bitmask)
			priv->bitmask = 0xff;
		if (priv->bitmask != 0xff && priv->bitmask != 0xff000000)
			return -EINVAL;
		break;
	case 16:
		if (!priv->bitmask)
			priv->bitmask = 0xffff;
		if (priv->bitmask != 0xffff && priv->bitmask != 0xffff0000)
			return EINVAL;
		break;
	case 32:
		break;
	default:
		return -EOPNOTSUPP;
	}

	err = nft_parse_register_load(ctx, tb[NFTA_MATH_SREG], &priv->sreg,
				      priv->len / BITS_PER_BYTE);
	if (err < 0)
		return err;

	return nft_parse_register_store(ctx, tb[NFTA_MATH_DREG],
					&priv->dreg, NULL, NFT_DATA_VALUE,
					priv->len / BITS_PER_BYTE);
}

static int nft_math_dump(struct sk_buff *skb,
			 const struct nft_expr *expr, bool reset)
{
	const struct nft_math *priv = nft_expr_priv(expr);

	if (nft_dump_register(skb, NFTA_MATH_SREG, priv->sreg))
		goto nla_put_failure;
	if (nft_dump_register(skb, NFTA_MATH_DREG, priv->dreg))
		goto nla_put_failure;
	if (priv->bitmask &&
	    nla_put_u32(skb, NFTA_MATH_BITMASK, htonl(priv->bitmask)))
		goto nla_put_failure;
	if (nla_put_u32(skb, NFTA_MATH_OP, htonl(priv->op)))
		goto nla_put_failure;
	if (nla_put_u32(skb, NFTA_MATH_LEN, htonl(priv->len)))
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
	return nft_unregister_expr(&nft_math_type);
}

module_init(nft_math_module_init);
module_exit(nft_math_module_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Fernando Fernandez Mancera <fmancera@suse.de>");
MODULE_ALIAS_NFT_EXPR("math");
MODULE_DESCRIPTION("nftables math support to operate with values");
