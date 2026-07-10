// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *  SRv6 Mobile User Plane implementation
 *
 *  Author:
 *  Yuya Kusakabe <yuya.kusakabe@gmail.com>
 */

#include <linux/icmpv6.h>
#include <linux/in6.h>
#include <linux/ipv6.h>
#include <linux/module.h>
#include <linux/skbuff.h>
#include <linux/tcp.h>
#include <linux/types.h>
#include <linux/udp.h>
#include <net/checksum.h>
#include <net/ipv6.h>
#include <net/lwtunnel.h>
#include <net/seg6.h>
#ifdef CONFIG_IPV6_SEG6_HMAC
#include <net/seg6_hmac.h>
#endif
#include <uapi/linux/seg6_mobile.h>

#define SEG6_MOBILE_F_ATTR(i)		BIT(i)
#define SEG6_F_MOBILE_COUNTERS		SEG6_MOBILE_F_ATTR(SEG6_MOBILE_COUNTERS)

struct seg6_mobile_lwt;

struct seg6_mobile_action_desc {
	int action;
	unsigned long attrs;
	unsigned long optattrs;
	int (*input)(struct sk_buff *skb, struct seg6_mobile_lwt *slwt);
};

struct seg6_mobile_action_param {
	int (*parse)(struct nlattr **attrs, struct seg6_mobile_lwt *slwt,
		     struct netlink_ext_ack *extack);
	int (*put)(struct sk_buff *skb, struct seg6_mobile_lwt *slwt);
	int (*cmp)(struct seg6_mobile_lwt *a, struct seg6_mobile_lwt *b);

	/* optional destroy() callback to release resources acquired in
	 * the corresponding parse() function.
	 */
	void (*destroy)(struct seg6_mobile_lwt *slwt);
};

struct pcpu_seg6_mobile_counters {
	u64_stats_t packets;
	u64_stats_t bytes;
	u64_stats_t errors;

	struct u64_stats_sync syncp;
};

/* User-space aggregate format for the per-CPU counters.  Kept private
 * to the kernel; userspace receives the values through SEG6_MOBILE_CNT_*
 * nested netlink attributes.
 */
struct seg6_mobile_counters {
	__u64 packets;
	__u64 bytes;
	__u64 errors;
};

#define seg6_mobile_alloc_pcpu_counters(__gfp)				\
	__netdev_alloc_pcpu_stats(struct pcpu_seg6_mobile_counters,	\
				  ((__gfp) | __GFP_ZERO))

struct seg6_mobile_lwt {
	int action;
	struct in6_addr nh6;
	const struct seg6_mobile_action_desc *desc;
	struct pcpu_seg6_mobile_counters __percpu *pcpu_counters;

	/* required attrs are tracked by desc->attrs; optional attrs that
	 * the user actually configured are tracked here so that fill_encap
	 * / cmp / destroy can iterate only over what was parsed.
	 */
	unsigned long parsed_optattrs;
};

static struct seg6_mobile_lwt *seg6_mobile_lwtunnel(struct lwtunnel_state *lwt)
{
	return (struct seg6_mobile_lwt *)lwt->data;
}

enum seg6_mobile_srh_state {
	SEG6_MOBILE_SRH_ABSENT,
	SEG6_MOBILE_SRH_PRESENT,
	SEG6_MOBILE_SRH_MALFORMED,
};

/* Return the SRH if present and valid.  @state separates ABSENT from
 * MALFORMED so End.MAP can forward an SRH-less packet while still
 * dropping a malformed one.
 */
static struct ipv6_sr_hdr *
seg6_mobile_get_and_validate_srh(struct sk_buff *skb,
				 enum seg6_mobile_srh_state *state)
{
	struct ipv6_sr_hdr *srh;
	unsigned int srhoff = 0;
	int hdr_proto;
	int flags = 0;

	srh = seg6_get_srh(skb, 0);
	if (srh) {
#ifdef CONFIG_IPV6_SEG6_HMAC
		if (!seg6_hmac_validate_skb(skb)) {
			*state = SEG6_MOBILE_SRH_MALFORMED;
			return NULL;
		}
#endif
		*state = SEG6_MOBILE_SRH_PRESENT;
		return srh;
	}

	hdr_proto = ipv6_find_hdr(skb, &srhoff, IPPROTO_ROUTING, NULL, &flags);
	*state = hdr_proto == -ENOENT ? SEG6_MOBILE_SRH_ABSENT
				      : SEG6_MOBILE_SRH_MALFORMED;
	return NULL;
}

/* Length of the L4 header that must be made writable so its checksum
 * field can be patched when the IPv6 DA changes.  Returns 0 for L4
 * protocols whose checksum does not cover the IPv6 pseudo-header.
 */
static int seg6_mobile_l4_csum_hlen(u8 nexthdr)
{
	switch (nexthdr) {
	case IPPROTO_TCP:
		return sizeof(struct tcphdr);
	case IPPROTO_UDP:
		return sizeof(struct udphdr);
	case IPPROTO_ICMPV6:
		return sizeof(struct icmp6hdr);
	}
	return 0;
}

/* Return a pointer to the L4 checksum field that needs the IPv6 DA
 * diff applied, or NULL if patching must be skipped.  Must be called
 * after the L4 header has been made writable.
 */
static __sum16 *seg6_mobile_l4_csum(struct sk_buff *skb, int l4_off,
				    u8 nexthdr)
{
	switch (nexthdr) {
	case IPPROTO_TCP:
		return &((struct tcphdr *)(skb->data + l4_off))->check;
	case IPPROTO_UDP: {
		struct udphdr *uh = (struct udphdr *)(skb->data + l4_off);

		/* A zero UDPv6 checksum on a fully assembled skb signals
		 * "no checksum" (e.g. tunneled UDP); patching it would
		 * invent a spurious non-zero value.
		 */
		if (!uh->check && skb->ip_summed != CHECKSUM_PARTIAL)
			return NULL;
		return &uh->check;
	}
	case IPPROTO_ICMPV6:
		return &((struct icmp6hdr *)(skb->data + l4_off))->icmp6_cksum;
	}
	return NULL;
}

/* Rewrite the IPv6 destination address with @nh.  When @srh_present is
 * false the packet has no routing header, so the receiver delivers it
 * straight to the transport: walk any Hop-by-Hop / Destination Options /
 * Fragment chain to the L4 header and, when that transport uses the IPv6
 * pseudo-header, patch its checksum by the DA diff.  When a routing
 * header is present the receiver first advances the SID list (SRv6
 * restores DA to segments[0]) before delivering to L4, so the original
 * checksum stays valid and only skb->csum needs maintenance for
 * CHECKSUM_COMPLETE skbs.
 */
static int seg6_mobile_advance_da(struct sk_buff *skb,
				  const struct in6_addr *nh, bool srh_present)
{
	int l4_off = 0, l4_hlen = 0;
	struct in6_addr old_da;
	struct ipv6hdr *ip6h;
	__be16 frag_off;
	u8 nexthdr = 0;
	__sum16 *csum;
	int write_len;

	if (!pskb_may_pull(skb, sizeof(*ip6h)))
		return -EINVAL;

	ip6h = ipv6_hdr(skb);
	write_len = sizeof(*ip6h);

	if (!srh_present) {
		nexthdr = ip6h->nexthdr;
		l4_off = ipv6_skip_exthdr(skb, sizeof(*ip6h), &nexthdr,
					  &frag_off);
		if (l4_off < 0)
			return -EINVAL;

		/* Non-first fragments carry no L4 header at @l4_off (the
		 * Fragment header reports a non-zero offset); only the
		 * first fragment, which holds the transport header, is
		 * patched.
		 */
		if (frag_off == 0)
			l4_hlen = seg6_mobile_l4_csum_hlen(nexthdr);
		if (l4_hlen)
			write_len = l4_off + l4_hlen;
	}

	if (skb_ensure_writable(skb, write_len))
		return -ENOMEM;

	/* skb_ensure_writable() may change skb pointers; evaluate ip6h again */
	ip6h = ipv6_hdr(skb);
	old_da = ip6h->daddr;

	csum = l4_hlen ? seg6_mobile_l4_csum(skb, l4_off, nexthdr) : NULL;
	if (csum) {
		inet_proto_csum_replace16(csum, skb, old_da.s6_addr32,
					  nh->s6_addr32, true);
		/* A real UDPv6 checksum of 0x0000 is illegal, replace it
		 * with 0xffff.  inet_proto_csum_replace16() keeps skb->csum
		 * consistent for CHECKSUM_COMPLETE because the IPv6 DA diff
		 * and the L4 csum diff cancel each other.
		 */
		if (nexthdr == IPPROTO_UDP && !*csum)
			*csum = CSUM_MANGLED_0;
	} else if (skb->ip_summed == CHECKSUM_COMPLETE) {
		update_csum_diff16(skb, old_da.s6_addr32, (__be32 *)nh);
	}

	ip6h->daddr = *nh;
	skb_clear_hash(skb);

	return 0;
}

/* seg6_lookup_nexthop() releases the original dst itself, so no
 * skb_dst_drop() is needed before the call.
 */
static int seg6_mobile_forward(struct sk_buff *skb)
{
	seg6_lookup_nexthop(skb, NULL, 0);
	return dst_input(skb);
}

static int input_action_end_map(struct sk_buff *skb,
				struct seg6_mobile_lwt *slwt)
{
	enum seg6_mobile_srh_state srh_state;

	seg6_mobile_get_and_validate_srh(skb, &srh_state);
	if (srh_state == SEG6_MOBILE_SRH_MALFORMED)
		goto drop;

	if (seg6_mobile_advance_da(skb, &slwt->nh6,
				   srh_state == SEG6_MOBILE_SRH_PRESENT))
		goto drop;

	return seg6_mobile_forward(skb);

drop:
	kfree_skb(skb);
	return -EINVAL;
}

static int parse_nla_nh6(struct nlattr **attrs, struct seg6_mobile_lwt *slwt,
			 struct netlink_ext_ack *extack)
{
	memcpy(&slwt->nh6, nla_data(attrs[SEG6_MOBILE_NH6]),
	       sizeof(struct in6_addr));

	return 0;
}

static int put_nla_nh6(struct sk_buff *skb, struct seg6_mobile_lwt *slwt)
{
	if (nla_put_in6_addr(skb, SEG6_MOBILE_NH6, &slwt->nh6))
		return -EMSGSIZE;

	return 0;
}

static int cmp_nla_nh6(struct seg6_mobile_lwt *a, struct seg6_mobile_lwt *b)
{
	return memcmp(&a->nh6, &b->nh6, sizeof(struct in6_addr));
}

static const struct
nla_policy seg6_mobile_counters_policy[SEG6_MOBILE_CNT_MAX + 1] = {
	[SEG6_MOBILE_CNT_PACKETS]	= { .type = NLA_U64 },
	[SEG6_MOBILE_CNT_BYTES]		= { .type = NLA_U64 },
	[SEG6_MOBILE_CNT_ERRORS]	= { .type = NLA_U64 },
};

static int parse_nla_counters(struct nlattr **attrs,
			      struct seg6_mobile_lwt *slwt,
			      struct netlink_ext_ack *extack)
{
	struct pcpu_seg6_mobile_counters __percpu *pcounters;
	struct nlattr *tb[SEG6_MOBILE_CNT_MAX + 1];
	int ret;

	ret = nla_parse_nested_deprecated(tb, SEG6_MOBILE_CNT_MAX,
					  attrs[SEG6_MOBILE_COUNTERS],
					  seg6_mobile_counters_policy, extack);
	if (ret < 0)
		return ret;

	/* basic support for SRv6 Behavior counters requires at least:
	 * packets, bytes and errors.
	 */
	if (!tb[SEG6_MOBILE_CNT_PACKETS] || !tb[SEG6_MOBILE_CNT_BYTES] ||
	    !tb[SEG6_MOBILE_CNT_ERRORS])
		return -EINVAL;

	/* counters are always zero initialized */
	pcounters = seg6_mobile_alloc_pcpu_counters(GFP_KERNEL);
	if (!pcounters)
		return -ENOMEM;

	slwt->pcpu_counters = pcounters;

	return 0;
}

static int seg6_mobile_fill_nla_counters(struct sk_buff *skb,
					 struct seg6_mobile_counters *counters)
{
	if (nla_put_u64_64bit(skb, SEG6_MOBILE_CNT_PACKETS, counters->packets,
			      SEG6_MOBILE_CNT_PAD))
		return -EMSGSIZE;

	if (nla_put_u64_64bit(skb, SEG6_MOBILE_CNT_BYTES, counters->bytes,
			      SEG6_MOBILE_CNT_PAD))
		return -EMSGSIZE;

	if (nla_put_u64_64bit(skb, SEG6_MOBILE_CNT_ERRORS, counters->errors,
			      SEG6_MOBILE_CNT_PAD))
		return -EMSGSIZE;

	return 0;
}

static int put_nla_counters(struct sk_buff *skb, struct seg6_mobile_lwt *slwt)
{
	struct seg6_mobile_counters counters = { 0, 0, 0 };
	struct nlattr *nest;
	int rc, i;

	nest = nla_nest_start(skb, SEG6_MOBILE_COUNTERS);
	if (!nest)
		return -EMSGSIZE;

	for_each_possible_cpu(i) {
		struct pcpu_seg6_mobile_counters *pcounters;
		u64 packets, bytes, errors;
		unsigned int start;

		pcounters = per_cpu_ptr(slwt->pcpu_counters, i);
		do {
			start = u64_stats_fetch_begin(&pcounters->syncp);

			packets = u64_stats_read(&pcounters->packets);
			bytes = u64_stats_read(&pcounters->bytes);
			errors = u64_stats_read(&pcounters->errors);

		} while (u64_stats_fetch_retry(&pcounters->syncp, start));

		counters.packets += packets;
		counters.bytes += bytes;
		counters.errors += errors;
	}

	rc = seg6_mobile_fill_nla_counters(skb, &counters);
	if (rc < 0) {
		nla_nest_cancel(skb, nest);
		return rc;
	}

	return nla_nest_end(skb, nest);
}

static int cmp_nla_counters(struct seg6_mobile_lwt *a,
			    struct seg6_mobile_lwt *b)
{
	/* tunnels with counters enabled and disabled are different. */
	return (!!((unsigned long)a->pcpu_counters)) ^
		(!!((unsigned long)b->pcpu_counters));
}

static void destroy_attr_counters(struct seg6_mobile_lwt *slwt)
{
	free_percpu(slwt->pcpu_counters);
}

static const struct seg6_mobile_action_desc seg6_mobile_action_table[] = {
	{
		.action		= SEG6_MOBILE_ACTION_END_MAP,
		.attrs		= SEG6_MOBILE_F_ATTR(SEG6_MOBILE_NH6),
		.optattrs	= SEG6_F_MOBILE_COUNTERS,
		.input		= input_action_end_map,
	},
};

static const struct seg6_mobile_action_param
seg6_mobile_action_params[SEG6_MOBILE_MAX + 1] = {
	[SEG6_MOBILE_NH6] = {
		.parse	= parse_nla_nh6,
		.put	= put_nla_nh6,
		.cmp	= cmp_nla_nh6,
	},
	[SEG6_MOBILE_COUNTERS] = {
		.parse		= parse_nla_counters,
		.put		= put_nla_counters,
		.cmp		= cmp_nla_counters,
		.destroy	= destroy_attr_counters,
	},
};

static const struct nla_policy
seg6_mobile_policy[SEG6_MOBILE_MAX + 1] = {
	[SEG6_MOBILE_ACTION]	= { .type = NLA_U32 },
	[SEG6_MOBILE_NH6]	= NLA_POLICY_EXACT_LEN(sizeof(struct in6_addr)),
	[SEG6_MOBILE_COUNTERS]	= { .type = NLA_NESTED },
};

static const struct seg6_mobile_action_desc *
seg6_mobile_get_action_desc(int action)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(seg6_mobile_action_table); i++) {
		if (seg6_mobile_action_table[i].action == action)
			return &seg6_mobile_action_table[i];
	}

	return NULL;
}

/* call the destroy() callback (if available) for each set attribute in
 * @parsed_attrs, starting from the first attribute up to the @max_parsed
 * (excluded) attribute.
 */
static void __destroy_attrs(unsigned long parsed_attrs, int max_parsed,
			    struct seg6_mobile_lwt *slwt)
{
	const struct seg6_mobile_action_param *param;
	int i;

	for (i = SEG6_MOBILE_ACTION + 1; i < max_parsed; i++) {
		if (!(parsed_attrs & SEG6_MOBILE_F_ATTR(i)))
			continue;

		param = &seg6_mobile_action_params[i];
		if (param->destroy)
			param->destroy(slwt);
	}
}

static void destroy_attrs(struct seg6_mobile_lwt *slwt)
{
	unsigned long attrs = slwt->desc->attrs | slwt->parsed_optattrs;

	__destroy_attrs(attrs, SEG6_MOBILE_MAX + 1, slwt);
}

static int seg6_mobile_parse_attrs(struct nlattr **attrs,
				   struct seg6_mobile_lwt *slwt,
				   struct netlink_ext_ack *extack)
{
	const struct seg6_mobile_action_param *param;
	const struct seg6_mobile_action_desc *desc;
	unsigned long parsed_optattrs = 0;
	int i, err;

	desc = slwt->desc;

	if (WARN_ON_ONCE(desc->attrs & desc->optattrs))
		return -EINVAL;

	for (i = SEG6_MOBILE_ACTION + 1; i <= SEG6_MOBILE_MAX; i++) {
		bool required = desc->attrs & SEG6_MOBILE_F_ATTR(i);
		bool optional = desc->optattrs & SEG6_MOBILE_F_ATTR(i);

		if (!required && !optional)
			continue;

		if (required && !attrs[i]) {
			NL_SET_ERR_MSG_MOD(extack,
					   "missing required attribute");
			err = -EINVAL;
			goto err;
		}

		if (!attrs[i])
			continue;

		param = &seg6_mobile_action_params[i];
		err = param->parse(attrs, slwt, extack);
		if (err < 0)
			goto err;

		if (optional)
			parsed_optattrs |= SEG6_MOBILE_F_ATTR(i);
	}

	slwt->parsed_optattrs = parsed_optattrs;

	return 0;

err:
	__destroy_attrs(desc->attrs | parsed_optattrs, i, slwt);
	return err;
}

static bool seg6_mobile_counters_enabled(struct seg6_mobile_lwt *slwt)
{
	return slwt->parsed_optattrs & SEG6_F_MOBILE_COUNTERS;
}

static void seg6_mobile_update_counters(struct seg6_mobile_lwt *slwt,
					unsigned int len, int err)
{
	struct pcpu_seg6_mobile_counters *pcounters;

	pcounters = this_cpu_ptr(slwt->pcpu_counters);
	u64_stats_update_begin(&pcounters->syncp);

	if (likely(!err)) {
		u64_stats_inc(&pcounters->packets);
		u64_stats_add(&pcounters->bytes, len);
	} else {
		u64_stats_inc(&pcounters->errors);
	}

	u64_stats_update_end(&pcounters->syncp);
}

static int seg6_mobile_input(struct sk_buff *skb)
{
	struct dst_entry *orig_dst = skb_dst(skb);
	struct seg6_mobile_lwt *slwt;
	unsigned int len = skb->len;
	int rc;

	if (skb->protocol != htons(ETH_P_IPV6)) {
		kfree_skb(skb);
		return -EINVAL;
	}

	slwt = seg6_mobile_lwtunnel(orig_dst->lwtstate);

	rc = slwt->desc->input(skb, slwt);

	if (seg6_mobile_counters_enabled(slwt))
		seg6_mobile_update_counters(slwt, len, rc);

	return rc;
}

static int seg6_mobile_build_state(struct net *net, struct nlattr *nla,
				   unsigned int family, const void *cfg,
				   struct lwtunnel_state **ts,
				   struct netlink_ext_ack *extack)
{
	const struct seg6_mobile_action_desc *desc;
	struct nlattr *tb[SEG6_MOBILE_MAX + 1];
	struct lwtunnel_state *newts;
	struct seg6_mobile_lwt *slwt;
	int err;

	if (family != AF_INET6)
		return -EINVAL;

	err = nla_parse_nested_deprecated(tb, SEG6_MOBILE_MAX, nla,
					  seg6_mobile_policy, extack);
	if (err < 0)
		return err;

	if (!tb[SEG6_MOBILE_ACTION]) {
		NL_SET_ERR_MSG_MOD(extack, "missing SEG6_MOBILE_ACTION");
		return -EINVAL;
	}

	desc = seg6_mobile_get_action_desc(nla_get_u32(tb[SEG6_MOBILE_ACTION]));
	if (!desc) {
		NL_SET_ERR_MSG_MOD(extack, "unknown SRv6 Mobile action");
		return -EOPNOTSUPP;
	}

	newts = lwtunnel_state_alloc(sizeof(*slwt));
	if (!newts)
		return -ENOMEM;

	slwt = seg6_mobile_lwtunnel(newts);
	slwt->action = desc->action;
	slwt->desc = desc;

	err = seg6_mobile_parse_attrs(tb, slwt, extack);
	if (err < 0) {
		kfree(newts);
		return err;
	}

	newts->type = LWTUNNEL_ENCAP_SEG6_MOBILE;
	newts->flags = LWTUNNEL_STATE_INPUT_REDIRECT;

	*ts = newts;

	return 0;
}

static void seg6_mobile_destroy_state(struct lwtunnel_state *lwt)
{
	destroy_attrs(seg6_mobile_lwtunnel(lwt));
}

static int seg6_mobile_fill_encap(struct sk_buff *skb,
				  struct lwtunnel_state *lwt)
{
	struct seg6_mobile_lwt *slwt = seg6_mobile_lwtunnel(lwt);
	const struct seg6_mobile_action_param *param;
	unsigned long attrs;
	int i, err;

	if (nla_put_u32(skb, SEG6_MOBILE_ACTION, slwt->action))
		return -EMSGSIZE;

	attrs = slwt->desc->attrs | slwt->parsed_optattrs;
	for (i = SEG6_MOBILE_ACTION + 1; i <= SEG6_MOBILE_MAX; i++) {
		if (!(attrs & SEG6_MOBILE_F_ATTR(i)))
			continue;

		param = &seg6_mobile_action_params[i];
		err = param->put(skb, slwt);
		if (err < 0)
			return err;
	}

	return 0;
}

static int seg6_mobile_get_encap_size(struct lwtunnel_state *lwt)
{
	struct seg6_mobile_lwt *slwt = seg6_mobile_lwtunnel(lwt);
	unsigned long attrs;
	int nlsize;

	nlsize = nla_total_size(sizeof(u32)); /* SEG6_MOBILE_ACTION */

	attrs = slwt->desc->attrs | slwt->parsed_optattrs;
	if (attrs & SEG6_MOBILE_F_ATTR(SEG6_MOBILE_NH6))
		nlsize += nla_total_size(sizeof(struct in6_addr));

	if (attrs & SEG6_F_MOBILE_COUNTERS)
		nlsize += nla_total_size(0) + /* nest SEG6_MOBILE_COUNTERS */
			  /* SEG6_MOBILE_CNT_PACKETS */
			  nla_total_size_64bit(sizeof(__u64)) +
			  /* SEG6_MOBILE_CNT_BYTES */
			  nla_total_size_64bit(sizeof(__u64)) +
			  /* SEG6_MOBILE_CNT_ERRORS */
			  nla_total_size_64bit(sizeof(__u64));

	return nlsize;
}

static int seg6_mobile_cmp_encap(struct lwtunnel_state *a,
				 struct lwtunnel_state *b)
{
	struct seg6_mobile_lwt *slwt_a = seg6_mobile_lwtunnel(a);
	struct seg6_mobile_lwt *slwt_b = seg6_mobile_lwtunnel(b);
	const struct seg6_mobile_action_param *param;
	unsigned long attrs_a, attrs_b;
	int i;

	if (slwt_a->action != slwt_b->action)
		return 1;

	attrs_a = slwt_a->desc->attrs | slwt_a->parsed_optattrs;
	attrs_b = slwt_b->desc->attrs | slwt_b->parsed_optattrs;

	if (attrs_a != attrs_b)
		return 1;

	for (i = SEG6_MOBILE_ACTION + 1; i <= SEG6_MOBILE_MAX; i++) {
		if (!(attrs_a & SEG6_MOBILE_F_ATTR(i)))
			continue;

		param = &seg6_mobile_action_params[i];
		if (param->cmp(slwt_a, slwt_b))
			return 1;
	}

	return 0;
}

static const struct lwtunnel_encap_ops seg6_mobile_ops = {
	.build_state	= seg6_mobile_build_state,
	.destroy_state	= seg6_mobile_destroy_state,
	.input		= seg6_mobile_input,
	.fill_encap	= seg6_mobile_fill_encap,
	.get_encap_size	= seg6_mobile_get_encap_size,
	.cmp_encap	= seg6_mobile_cmp_encap,
	.owner		= THIS_MODULE,
};

int __init seg6_mobile_init(void)
{
	BUILD_BUG_ON(SEG6_MOBILE_MAX + 1 > BITS_PER_TYPE(unsigned long));

	return lwtunnel_encap_add_ops(&seg6_mobile_ops,
				      LWTUNNEL_ENCAP_SEG6_MOBILE);
}

void seg6_mobile_exit(void)
{
	lwtunnel_encap_del_ops(&seg6_mobile_ops, LWTUNNEL_ENCAP_SEG6_MOBILE);
}
