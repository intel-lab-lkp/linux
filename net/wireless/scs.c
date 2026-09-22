// SPDX-License-Identifier: GPL-2.0-only
/*
 * Stream classification service (SCS) and mirrored SCS (MSCS)
 *
 * Copyright (C) 2026 Felix Fietkau <nbd@nbd.name>
 */
#include <kunit/visibility.h>
#include <linux/ieee80211.h>
#include <linux/if_vlan.h>
#include <linux/ip.h>
#include <linux/ipv6.h>
#include <linux/unaligned.h>
#include <net/cfg80211.h>
#include <net/dsfield.h>
#include <net/ip.h>
#include <net/ipv6.h>
#include "core.h"

enum cfg80211_tclas_type {
	CFG80211_TCLAS_ETH		= 0,
	CFG80211_TCLAS_IP		= 4,
	CFG80211_TCLAS_VLAN		= 5,
	CFG80211_TCLAS_FILTER		= 10,
};

struct tclas_fc {
	u8 type;
	u8 mask;
} __packed;

struct tclas_fc_eth {
	struct tclas_fc hdr;
	u8 sa[ETH_ALEN];
	u8 da[ETH_ALEN];
	__be16 ethertype;
} __packed;

struct tclas_fc_ip4 {
	struct tclas_fc hdr;
	u8 version;
	__be32 src;
	__be32 dst;
	__be16 src_port;
	__be16 dst_port;
	u8 dscp;
	u8 proto;
	u8 reserved;
} __packed;

struct tclas_fc_ip6 {
	struct tclas_fc hdr;
	u8 version;
	struct in6_addr src;
	struct in6_addr dst;
	__be16 src_port;
	__be16 dst_port;
	u8 dscp;
	u8 proto;
	u8 flow_label[3];
} __packed;

struct tclas_fc_vlan {
	struct tclas_fc hdr;
	u8 pcp;
	u8 dei;
	__be16 vid;
} __packed;

/* Hashed and compared as a byte string, so it must contain no padding */
static_assert(sizeof(struct cfg80211_flow_key) ==
	      2 * sizeof(struct in6_addr) + sizeof(__be32) + 2 * ETH_ALEN +
	      4 * sizeof(__be16) + 4);

#define FLOW_F(name)	BIT(CFG80211_FLOW_F_##name)

#define FLOW_F_VLAN	(FLOW_F(VLAN_PCP) | FLOW_F(VLAN_DEI) | FLOW_F(VLAN_VID))

/* Reserved classifier mask bits above the array length are ignored */
static const u8 tclas_map_eth[] = {
	CFG80211_FLOW_F_ETH_SA,
	CFG80211_FLOW_F_ETH_DA,
	CFG80211_FLOW_F_ETH_TYPE,
};

/* Shared by both IP versions, only IPv6 has the flow label */
static const u8 tclas_map_ip[] = {
	CFG80211_FLOW_F_IP_VERSION,
	CFG80211_FLOW_F_IP_SRC,
	CFG80211_FLOW_F_IP_DST,
	CFG80211_FLOW_F_SRC_PORT,
	CFG80211_FLOW_F_DST_PORT,
	CFG80211_FLOW_F_DSCP,
	CFG80211_FLOW_F_PROTO,
	CFG80211_FLOW_F_FLOW_LABEL,
};

static const u8 tclas_map_vlan[] = {
	CFG80211_FLOW_F_VLAN_PCP,
	CFG80211_FLOW_F_VLAN_DEI,
	CFG80211_FLOW_F_VLAN_VID,
};

static u32 tclas_mask_fields(u8 type, u8 version, u8 mask)
{
	size_t n = ARRAY_SIZE(tclas_map_ip);
	const u8 *map = tclas_map_ip;
	unsigned int i;
	u32 fields = 0;

	switch (type) {
	case CFG80211_TCLAS_ETH:
		map = tclas_map_eth;
		n = ARRAY_SIZE(tclas_map_eth);
		break;
	case CFG80211_TCLAS_VLAN:
		map = tclas_map_vlan;
		n = ARRAY_SIZE(tclas_map_vlan);
		break;
	default:
		n -= version == 4;
		break;
	}

	for (i = 0; i < n; i++)
		if (mask & BIT(i))
			fields |= BIT(map[i]);

	return fields;
}

static void flow_key_select(const struct cfg80211_flow_key *src, u32 fields,
			    struct cfg80211_flow_key *dst)
{
	memset(dst, 0, sizeof(*dst));

	if (fields & FLOW_F(ETH_SA))
		memcpy(dst->sa, src->sa, ETH_ALEN);
	if (fields & FLOW_F(ETH_DA))
		memcpy(dst->da, src->da, ETH_ALEN);
	if (fields & FLOW_F(ETH_TYPE))
		dst->eth_type = src->eth_type;

	if (fields & FLOW_F_VLAN) {
		u16 tci = 0;

		if (fields & FLOW_F(VLAN_PCP))
			tci |= VLAN_PRIO_MASK;
		if (fields & FLOW_F(VLAN_DEI))
			tci |= VLAN_CFI_MASK;
		if (fields & FLOW_F(VLAN_VID))
			tci |= VLAN_VID_MASK;

		dst->vlan_tci = src->vlan_tci & htons(tci);
	}

	if (fields & FLOW_F(IP_VERSION))
		dst->ip_version = src->ip_version;
	if (fields & FLOW_F(IP_SRC))
		dst->src = src->src;
	if (fields & FLOW_F(IP_DST))
		dst->dst = src->dst;
	if (fields & FLOW_F(SRC_PORT))
		dst->src_port = src->src_port;
	if (fields & FLOW_F(DST_PORT))
		dst->dst_port = src->dst_port;

	if (fields & FLOW_F(DSCP))
		dst->dscp = src->dscp;
	if (fields & FLOW_F(PROTO))
		dst->proto = src->proto;
	if (fields & FLOW_F(FLOW_LABEL))
		dst->flow_label = src->flow_label;
}

static int tclas_parse_eth(struct cfg80211_flow_key *key, const u8 *data,
			   size_t len)
{
	const struct tclas_fc_eth *fc = (const void *)data;

	if (len != sizeof(*fc))
		return -EINVAL;

	memcpy(key->sa, fc->sa, ETH_ALEN);
	memcpy(key->da, fc->da, ETH_ALEN);
	key->eth_type = fc->ethertype;

	return 0;
}

static int tclas_parse_ip4(struct cfg80211_flow_key *key,
			   const struct tclas_fc_ip4 *fc, bool values)
{
	if (values && fc->version != 4)
		return -EINVAL;

	key->ip_version = 4;
	memcpy(&key->src, &fc->src, sizeof(fc->src));
	memcpy(&key->dst, &fc->dst, sizeof(fc->dst));
	key->src_port = fc->src_port;
	key->dst_port = fc->dst_port;
	key->dscp = fc->dscp & 0x3f;
	key->proto = fc->proto;

	return 0;
}

static int tclas_parse_ip6(struct cfg80211_flow_key *key,
			   const struct tclas_fc_ip6 *fc, bool values)
{
	if (values && fc->version != 6)
		return -EINVAL;

	key->ip_version = 6;
	memcpy(&key->src, &fc->src, sizeof(fc->src));
	memcpy(&key->dst, &fc->dst, sizeof(fc->dst));
	key->src_port = fc->src_port;
	key->dst_port = fc->dst_port;
	key->dscp = fc->dscp & 0x3f;
	key->proto = fc->proto;
	/* Same encoding as ip6_flowlabel(), so the two compare equal */
	key->flow_label =
		cpu_to_be32(get_unaligned_be24(fc->flow_label) & 0xfffff);

	return 0;
}

/* The Version subfield is reserved in a mask, so the length selects v4/v6 */
static int tclas_parse_ip(struct cfg80211_flow_key *key, const u8 *data,
			  size_t len, bool values)
{
	if (len == sizeof(struct tclas_fc_ip4))
		return tclas_parse_ip4(key, (const void *)data, values);

	if (len == sizeof(struct tclas_fc_ip6))
		return tclas_parse_ip6(key, (const void *)data, values);

	return -EINVAL;
}

static int tclas_parse_vlan(struct cfg80211_flow_key *key, const u8 *data,
			    size_t len)
{
	const struct tclas_fc_vlan *fc = (const void *)data;
	u16 tci;

	if (len != sizeof(*fc))
		return -EINVAL;

	if (fc->pcp > 7 || fc->dei > 1 || be16_to_cpu(fc->vid) > 0xfff)
		return -EINVAL;

	tci = fc->pcp << 13;
	tci |= fc->dei << 12;
	tci |= be16_to_cpu(fc->vid);
	key->vlan_tci = cpu_to_be16(tci);

	return 0;
}

/* The octets after the header are the value and the mask, of equal length */
static int tclas_parse_filter(struct cfg80211_tclas *t, const u8 *data,
			      size_t len)
{
	struct cfg80211_tclas_filter *f = &t->filter;

	if (len < 3 || (len - 3) % 2)
		return -EINVAL;

	f->len = (len - 3) / 2;
	if (!f->len || f->len > CFG80211_TCLAS_FILTER_LEN)
		return -EOPNOTSUPP;

	f->instance = data[1];
	f->protocol = data[2];
	memcpy(f->value, data + 3, f->len);
	memcpy(f->mask, data + 3 + f->len, f->len);

	return 0;
}

/* @values is false for a TCLAS Mask element, which carries no values */
static int cfg80211_parse_frame_classifier(struct cfg80211_tclas *t,
					   const u8 *data, size_t len,
					   bool values)
{
	const struct tclas_fc *fc = (const void *)data;
	struct cfg80211_flow_key raw = {};
	int ret;

	if (len < sizeof(*fc))
		return -EINVAL;

	memset(t, 0, sizeof(*t));
	t->type = fc->type;

	switch (t->type) {
	case CFG80211_TCLAS_ETH:
		ret = tclas_parse_eth(&raw, data, len);
		break;
	case CFG80211_TCLAS_IP:
		ret = tclas_parse_ip(&raw, data, len, values);
		break;
	case CFG80211_TCLAS_VLAN:
		ret = tclas_parse_vlan(&raw, data, len);
		break;
	case CFG80211_TCLAS_FILTER:
		/* A mask element selects fields, which a filter does not */
		if (!values)
			return -EOPNOTSUPP;

		return tclas_parse_filter(t, data, len);
	default:
		return -EOPNOTSUPP;
	}

	if (ret)
		return ret;

	t->fields = tclas_mask_fields(t->type, raw.ip_version, fc->mask);

	/* A mask that selects nothing would match every MSDU */
	if (!t->fields)
		return -EINVAL;

	/* Always match on the IP version, which the element length selects */
	if (t->type == CFG80211_TCLAS_IP)
		t->fields |= FLOW_F(IP_VERSION);

	flow_key_select(&raw, t->fields, &t->key);

	return 0;
}

/**
 * cfg80211_tclas_count - count the TCLAS elements of a chain
 *
 * @elems: element chain, as it arrived over the air
 * @len: length of @elems
 *
 * Sizes the array for cfg80211_parse_tclas(). Does not validate the
 * elements.
 *
 * Return: the number of TCLAS elements, or -ENOSPC for more than a descriptor
 *	can hold.
 */
int cfg80211_tclas_count(const u8 *elems, size_t len)
{
	const struct element *elem;
	unsigned int n = 0;

	for_each_element_id(elem, WLAN_EID_TCLAS, elems, len)
		n++;

	if (n > U8_MAX)
		return -ENOSPC;

	return n;
}
EXPORT_SYMBOL_IF_CFG80211_KUNIT(cfg80211_tclas_count);

/**
 * cfg80211_parse_tclas - parse a chain of TCLAS elements
 *
 * @elems: element chain, as it arrived over the air
 * @len: length of @elems
 * @out: array that receives the parsed elements
 * @n_tclas: number of entries in @out, from cfg80211_tclas_count()
 * @processing: receives how the elements relate, or
 *	%CFG80211_TCLAS_PROCESSING_ABSENT when the chain carries no TCLAS
 *	Processing element
 *
 * The chain holds TCLAS elements and at most one TCLAS Processing element.
 *
 * Return: 0 on success, -EINVAL for a malformed chain, -EOPNOTSUPP for a
 *	classifier the kernel cannot evaluate, -ENOSPC for a chain that no
 *	longer fits @out.
 */
int cfg80211_parse_tclas(const u8 *elems, size_t len,
			 struct cfg80211_tclas *out, u8 n_tclas,
			 enum cfg80211_tclas_processing *processing)
{
	const struct element *elem;
	unsigned int n = 0;
	int ret;

	*processing = CFG80211_TCLAS_PROCESSING_ABSENT;

	for_each_element(elem, elems, len) {
		switch (elem->id) {
		case WLAN_EID_TCLAS:
			if (n == n_tclas)
				return -ENOSPC;

			if (elem->datalen < 1)
				return -EINVAL;

			/*
			 * Any other value matches on the UP of the MSDU,
			 * which is not known yet at classification time.
			 */
			if (elem->data[0] != 255)
				return -EOPNOTSUPP;

			ret = cfg80211_parse_frame_classifier(&out[n],
							      elem->data + 1,
							      elem->datalen - 1,
							      true);
			if (ret)
				return ret;

			n++;
			break;
		case WLAN_EID_TCLAS_PROCESSING:
			if (elem->datalen != 1 ||
			    *processing != CFG80211_TCLAS_PROCESSING_ABSENT ||
			    elem->data[0] > CFG80211_TCLAS_PROCESSING_DEFAULT)
				return -EINVAL;

			*processing = elem->data[0];
			break;
		default:
			return -EINVAL;
		}
	}

	if (!for_each_element_completed(elem, elems, len))
		return -EINVAL;

	return 0;
}
EXPORT_SYMBOL_IF_CFG80211_KUNIT(cfg80211_parse_tclas);

/**
 * cfg80211_parse_tclas_mask - parse a chain of TCLAS Mask elements
 *
 * @elems: element chain, as it arrived over the air
 * @len: length of @elems
 * @fields: receives the union of the selected classifier parameters, as a
 *	bitmap of &enum cfg80211_flow_field
 *
 * A TCLAS Mask element carries no values, only the field selection.
 *
 * Return: 0 on success, -EINVAL for a malformed chain, -EOPNOTSUPP for a
 *	classifier type the kernel cannot evaluate.
 */
int cfg80211_parse_tclas_mask(const u8 *elems, size_t len, u32 *fields)
{
	const struct element *elem;
	struct cfg80211_tclas t;
	int ret;

	*fields = 0;

	for_each_element(elem, elems, len) {
		if (elem->id != WLAN_EID_EXTENSION || elem->datalen < 1 ||
		    elem->data[0] != WLAN_EID_EXT_TCLAS_MASK)
			return -EINVAL;

		ret = cfg80211_parse_frame_classifier(&t, elem->data + 1,
						      elem->datalen - 1, false);
		if (ret)
			return ret;

		*fields |= t.fields;
	}

	if (!for_each_element_completed(elem, elems, len))
		return -EINVAL;

	return 0;
}
EXPORT_SYMBOL_IF_CFG80211_KUNIT(cfg80211_parse_tclas_mask);

#define FLOW_F_PORTS	(FLOW_F(SRC_PORT) | FLOW_F(DST_PORT))

static void cfg80211_flow_parse_ports(struct sk_buff *skb,
				      unsigned int offset, int proto,
				      struct cfg80211_flow_info *info)
{
	__be16 ports[2], *p;

	switch (proto) {
	case IPPROTO_TCP:
	case IPPROTO_UDP:
	case IPPROTO_SCTP:
		break;
	default:
		return;
	}

	p = skb_header_pointer(skb, offset, sizeof(ports), ports);
	if (!p)
		return;

	info->key.src_port = p[0];
	info->key.dst_port = p[1];
	info->present |= FLOW_F_PORTS;
}

static void cfg80211_flow_parse_ipv4(struct sk_buff *skb, unsigned int offset,
				     struct cfg80211_flow_info *info)
{
	struct iphdr iph_buf, *iph;

	iph = skb_header_pointer(skb, offset, sizeof(*iph), &iph_buf);
	if (!iph || iph->version != 4 || iph->ihl < 5)
		return;

	info->key.ip_version = 4;
	info->key.dscp = ipv4_get_dsfield(iph) >> 2;
	info->key.proto = iph->protocol;
	memcpy(&info->key.src, &iph->saddr, sizeof(iph->saddr));
	memcpy(&info->key.dst, &iph->daddr, sizeof(iph->daddr));
	info->present |= FLOW_F(IP_VERSION) | FLOW_F(IP_SRC) | FLOW_F(IP_DST) |
			 FLOW_F(DSCP) | FLOW_F(PROTO);

	if (iph->frag_off & htons(IP_OFFSET))
		return;

	cfg80211_flow_parse_ports(skb, offset + iph->ihl * 4, iph->protocol,
				  info);
}

static void cfg80211_flow_parse_ipv6(struct sk_buff *skb, unsigned int offset,
				     struct cfg80211_flow_info *info)
{
	struct ipv6hdr ip6h_buf, *ip6h;
	__be16 frag_off = 0;
	int l4_off;
	u8 nexthdr;

	ip6h = skb_header_pointer(skb, offset, sizeof(*ip6h), &ip6h_buf);
	if (!ip6h || ip6h->version != 6)
		return;

	info->key.ip_version = 6;
	info->key.dscp = ipv6_get_dsfield(ip6h) >> 2;
	info->key.flow_label = ip6_flowlabel(ip6h);
	info->key.src = ip6h->saddr;
	info->key.dst = ip6h->daddr;

	/* The classifier matches Next Header, not the layer 4 protocol */
	info->key.proto = ip6h->nexthdr;
	info->present |= FLOW_F(IP_VERSION) | FLOW_F(IP_SRC) | FLOW_F(IP_DST) |
			 FLOW_F(DSCP) | FLOW_F(FLOW_LABEL) | FLOW_F(PROTO);

	nexthdr = ip6h->nexthdr;
	l4_off = ipv6_skip_exthdr(skb, offset + sizeof(*ip6h), &nexthdr,
				  &frag_off);
	if (l4_off < 0 || frag_off & htons(IP6_OFFSET))
		return;

	cfg80211_flow_parse_ports(skb, l4_off, nexthdr, info);
}

/**
 * cfg80211_flow_parse - read the classifier parameters of an MSDU
 *
 * @skb: the frame, in IEEE 802.3 format
 * @info: receives the parameters
 *
 * Parses from skb->data, since a frame forwarded between two stations
 * arrives with skb->protocol set to ETH_P_802_3 and no network header.
 *
 * Return: %true when @info describes the frame.
 */
bool cfg80211_flow_parse(struct sk_buff *skb, struct cfg80211_flow_info *info)
{
	struct ethhdr eth_buf, *eth;
	unsigned int offset = ETH_HLEN;
	__be16 proto;

	memset(info, 0, sizeof(*info));

	eth = skb_header_pointer(skb, 0, sizeof(*eth), &eth_buf);
	if (!eth)
		return false;

	ether_addr_copy(info->key.sa, eth->h_source);
	ether_addr_copy(info->key.da, eth->h_dest);
	info->present = FLOW_F(ETH_SA) | FLOW_F(ETH_DA);
	proto = eth->h_proto;

	if (skb_vlan_tag_present(skb)) {
		info->key.vlan_tci = htons(skb_vlan_tag_get(skb));
		info->present |= FLOW_F_VLAN;
	} else if (eth_type_vlan(proto)) {
		struct vlan_hdr vhdr_buf, *vhdr;

		vhdr = skb_header_pointer(skb, offset, sizeof(*vhdr),
					  &vhdr_buf);
		if (!vhdr)
			return false;

		info->key.vlan_tci = vhdr->h_vlan_TCI;
		info->present |= FLOW_F_VLAN;
		proto = vhdr->h_vlan_encapsulated_proto;
		offset += sizeof(*vhdr);
	}

	/* Use the type behind the VLAN tag, whether it was stripped or not */
	info->key.eth_type = proto;
	info->present |= FLOW_F(ETH_TYPE);

	info->skb = skb;
	info->l3_off = offset;

	switch (proto) {
	case htons(ETH_P_IP):
		cfg80211_flow_parse_ipv4(skb, offset, info);
		break;
	case htons(ETH_P_IPV6):
		cfg80211_flow_parse_ipv6(skb, offset, info);
		break;
	}

	return true;
}
EXPORT_SYMBOL(cfg80211_flow_parse);

static void flow_key_swap(struct cfg80211_flow_key *k)
{
	u8 addr[ETH_ALEN];

	memcpy(addr, k->sa, ETH_ALEN);
	memcpy(k->sa, k->da, ETH_ALEN);
	memcpy(k->da, addr, ETH_ALEN);

	swap(k->src, k->dst);
	swap(k->src_port, k->dst_port);
}

static u32 cfg80211_flow_fields_mirror(u32 fields)
{
	static const u8 pairs[][2] = {
		{ CFG80211_FLOW_F_ETH_SA, CFG80211_FLOW_F_ETH_DA },
		{ CFG80211_FLOW_F_IP_SRC, CFG80211_FLOW_F_IP_DST },
		{ CFG80211_FLOW_F_SRC_PORT, CFG80211_FLOW_F_DST_PORT },
	};
	unsigned int i;
	u32 out = fields;

	for (i = 0; i < ARRAY_SIZE(pairs); i++) {
		u32 lo = BIT(pairs[i][0]), hi = BIT(pairs[i][1]);

		out &= ~(lo | hi);
		if (fields & lo)
			out |= hi;
		if (fields & hi)
			out |= lo;
	}

	return out;
}

/**
 * cfg80211_flow_key_build - build an MSCS lookup key
 *
 * @info: parsed frame, from cfg80211_flow_parse()
 * @fields: classifier parameters of the MSCS, a bitmap of
 *	&enum cfg80211_flow_field
 * @dir: %CFG80211_FLOW_MIRRORED when @info describes an uplink frame and the
 *	key must describe the downlink direction
 * @key: receives the key
 *
 * Everything that @fields does not select stays zero, so the key is hashed and
 * compared as a byte string.
 *
 * Return: %false when @info lacks a parameter that @fields selects, in which
 *	case the MSDU is not classified at all.
 */
bool cfg80211_flow_key_build(const struct cfg80211_flow_info *info, u32 fields,
			     enum cfg80211_flow_dir dir,
			     struct cfg80211_flow_key *key)
{
	struct cfg80211_flow_key mirrored;

	if (dir == CFG80211_FLOW_AS_IS) {
		if (fields & ~info->present)
			return false;

		flow_key_select(&info->key, fields, key);

		return true;
	}

	if (cfg80211_flow_fields_mirror(fields) & ~info->present)
		return false;

	mirrored = info->key;
	flow_key_swap(&mirrored);
	flow_key_select(&mirrored, fields, key);

	return true;
}
EXPORT_SYMBOL(cfg80211_flow_key_build);
