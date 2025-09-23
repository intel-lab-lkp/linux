// SPDX-License-Identifier: GPL-2.0

#include <net/netlink.h>
#include <net/netfilter/nf_tables.h>
#include <linux/netfilter/nf_tables.h>

struct nft_math {
	u8		 	sreg;
	u8		 	dreg;
	u8		 	len;
	enum nft_math_op	op;
	enum nft_math_byteorder byteorder;
};

static const struct nla_policy nft_math_policy[NFTA_MATH_MAX + 1] = {
	[NFTA_MATH_SREG]	= { .type = NLA_U32 },
	[NFTA_MATH_DREG]	= { .type = NLA_U32 },
	[NFTA_MATH_OP]		= { .type = NLA_U8 },
	[NFTA_MATH_LEN]		= NLA_POLICY_MIN(NLA_U8, 8),
	[NFTA_MATH_BYTEORDER]	= { .type = NLA_U8 },
};

static void nft_math_eval_u8(uint32_t *src, uint32_t *dst,
			      const struct nft_math *priv)
{
	u8 tmp;

	/* For payload set if checksum needs to be adjusted 16 bits are stored
	 * in the source register instead of 8. Therefore, use a bitmask to
	 * operate with the less significant byte. */
	switch (priv->op) {
	case NFT_MATH_OP_INC:
		tmp = *src & 0xff;
		if (tmp != U8_MAX) {
			tmp++;
			*dst = (*src & ~0xff) | tmp;
		} else {
			*dst = *src;
		}
		break;
	case NFT_MATH_OP_DEC:
		tmp = *src & 0xff;
		if (tmp != 0) {
			tmp--;
			*dst = (*src & ~0xff) | tmp;
		} else {
			*dst = *src;
		}
		break;
	default:
		break;
	}
}

static void nft_math_eval_u16(uint32_t *src, uint32_t *dst,
			      const struct nft_math *priv)
{
	u16 tmp;

	switch (priv->op) {
	case NFT_MATH_OP_INC:
		switch (priv->byteorder) {
		case NFT_MATH_BYTEORDER_HOST:
			tmp = nft_reg_load16(src);
			if (tmp != U16_MAX)
				tmp++;
			nft_reg_store16(dst, tmp);
			break;
		case NFT_MATH_BYTEORDER_BIG:
			tmp = ntohs(nft_reg_load_be16(src));
			if (tmp != U16_MAX)
				tmp++;
			nft_reg_store_be16(dst, htons(tmp));
			break;
		default:
			break;
		}
		break;
	case NFT_MATH_OP_DEC:
		switch (priv->byteorder) {
		case NFT_MATH_BYTEORDER_HOST:
			tmp = nft_reg_load16(src);
			if (tmp != 0)
				tmp--;
			nft_reg_store16(dst, tmp);
			break;
		case NFT_MATH_BYTEORDER_BIG:
			tmp = ntohs(nft_reg_load_be16(src));
			if (tmp != 0)
				tmp--;
			nft_reg_store_be16(dst, htons(tmp));
			break;
		default:
			break;
		}
		break;
	default:
		break;
	}
}

static void nft_math_eval_u32(uint32_t *src, uint32_t *dst,
			      const struct nft_math *priv)
{
	u32 tmp;

	switch (priv->op) {
	case NFT_MATH_OP_INC:
		switch (priv->byteorder) {
		case NFT_MATH_BYTEORDER_HOST:
			if (*src != U32_MAX)
				*dst = *src + 1;
			else
				*dst = *src;
			break;
		case NFT_MATH_BYTEORDER_BIG:
			tmp = ntohl(*src);
			if (tmp != U32_MAX)
				tmp++;
			*dst = (__force __u32)htonl(tmp);
			break;
		default:
			break;
		}
		break;
	case NFT_MATH_OP_DEC:
		switch (priv->byteorder) {
		case NFT_MATH_BYTEORDER_HOST:
			if (*src != 0)
				*dst = *src - 1;
			break;
		case NFT_MATH_BYTEORDER_BIG:
			tmp = ntohl(*src);
			if (tmp != 0)
				tmp--;
			*dst = (__force __u32)htonl(tmp);
			break;
		default:
			break;
		}
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
	case 8:
		nft_math_eval_u8(src, dst, priv);
		break;
	case 16:
		nft_math_eval_u16(src, dst, priv);
		break;
	case 32:
		nft_math_eval_u32(src, dst, priv);
		break;
	default:
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
	    tb[NFTA_MATH_OP] == NULL ||
	    tb[NFTA_MATH_BYTEORDER] == NULL)
		return -EINVAL;

	priv->op = nla_get_u8(tb[NFTA_MATH_OP]);
	priv->byteorder = nla_get_u8(tb[NFTA_MATH_BYTEORDER]);
	priv->len = nla_get_u8(tb[NFTA_MATH_LEN]);

	if (priv->byteorder > NFT_MATH_BYTEORDER_MAX)
		return -EINVAL;

	if (priv->op > NFT_MATH_OP_MAX)
		return -EOPNOTSUPP;

	switch (priv->len) {
	case 8:
	case 16:
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
	if (nla_put_u8(skb, NFTA_MATH_LEN, priv->len))
		goto nla_put_failure;
	if (nla_put_u8(skb, NFTA_MATH_OP, priv->op))
		goto nla_put_failure;
	if (nla_put_u8(skb, NFTA_MATH_BYTEORDER, priv->byteorder))
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
