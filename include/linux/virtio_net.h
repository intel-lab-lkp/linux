/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_VIRTIO_NET_H
#define _LINUX_VIRTIO_NET_H

#include <linux/if_vlan.h>
#include <linux/ip.h>
#include <linux/ipv6.h>
#include <linux/udp.h>
#include <uapi/linux/tcp.h>
#include <uapi/linux/virtio_net.h>

struct virtio_net_hash {
	u32 value;
	u16 report;
};

struct virtio_net_toeplitz_state {
	u32 hash;
	const u32 *key;
};

#define VIRTIO_NET_SUPPORTED_HASH_TYPES (VIRTIO_NET_RSS_HASH_TYPE_IPv4 | \
					 VIRTIO_NET_RSS_HASH_TYPE_TCPv4 | \
					 VIRTIO_NET_RSS_HASH_TYPE_UDPv4 | \
					 VIRTIO_NET_RSS_HASH_TYPE_IPv6 | \
					 VIRTIO_NET_RSS_HASH_TYPE_TCPv6 | \
					 VIRTIO_NET_RSS_HASH_TYPE_UDPv6)

#define VIRTIO_NET_RSS_MAX_KEY_SIZE 40

static inline void virtio_net_toeplitz_convert_key(u32 *input, size_t len)
{
	while (len >= sizeof(*input)) {
		*input = be32_to_cpu((__force __be32)*input);
		input++;
		len -= sizeof(*input);
	}
}

static inline void virtio_net_toeplitz_calc(struct virtio_net_toeplitz_state *state,
					    const __be32 *input, size_t len)
{
	while (len >= sizeof(*input)) {
		for (u32 map = be32_to_cpu(*input); map; map &= (map - 1)) {
			u32 i = ffs(map);

			state->hash ^= state->key[0] << (32 - i) |
				       (u32)((u64)state->key[1] >> i);
		}

		state->key++;
		input++;
		len -= sizeof(*input);
	}
}

static inline u8 virtio_net_hash_key_length(u32 types)
{
	size_t len = 0;

	if (types & VIRTIO_NET_HASH_REPORT_IPv4)
		len = max(len,
			  sizeof(struct flow_dissector_key_ipv4_addrs));

	if (types &
	    (VIRTIO_NET_HASH_REPORT_TCPv4 | VIRTIO_NET_HASH_REPORT_UDPv4))
		len = max(len,
			  sizeof(struct flow_dissector_key_ipv4_addrs) +
			  sizeof(struct flow_dissector_key_ports));

	if (types & VIRTIO_NET_HASH_REPORT_IPv6)
		len = max(len,
			  sizeof(struct flow_dissector_key_ipv6_addrs));

	if (types &
	    (VIRTIO_NET_HASH_REPORT_TCPv6 | VIRTIO_NET_HASH_REPORT_UDPv6))
		len = max(len,
			  sizeof(struct flow_dissector_key_ipv6_addrs) +
			  sizeof(struct flow_dissector_key_ports));

	return len + sizeof(u32);
}

static inline u32 virtio_net_hash_report(u32 types,
					 const struct flow_keys_basic *keys)
{
	switch (keys->basic.n_proto) {
	case cpu_to_be16(ETH_P_IP):
		if (!(keys->control.flags & FLOW_DIS_IS_FRAGMENT)) {
			if (keys->basic.ip_proto == IPPROTO_TCP &&
			    (types & VIRTIO_NET_RSS_HASH_TYPE_TCPv4))
				return VIRTIO_NET_HASH_REPORT_TCPv4;

			if (keys->basic.ip_proto == IPPROTO_UDP &&
			    (types & VIRTIO_NET_RSS_HASH_TYPE_UDPv4))
				return VIRTIO_NET_HASH_REPORT_UDPv4;
		}

		if (types & VIRTIO_NET_RSS_HASH_TYPE_IPv4)
			return VIRTIO_NET_HASH_REPORT_IPv4;

		return VIRTIO_NET_HASH_REPORT_NONE;

	case cpu_to_be16(ETH_P_IPV6):
		if (!(keys->control.flags & FLOW_DIS_IS_FRAGMENT)) {
			if (keys->basic.ip_proto == IPPROTO_TCP &&
			    (types & VIRTIO_NET_RSS_HASH_TYPE_TCPv6))
				return VIRTIO_NET_HASH_REPORT_TCPv6;

			if (keys->basic.ip_proto == IPPROTO_UDP &&
			    (types & VIRTIO_NET_RSS_HASH_TYPE_UDPv6))
				return VIRTIO_NET_HASH_REPORT_UDPv6;
		}

		if (types & VIRTIO_NET_RSS_HASH_TYPE_IPv6)
			return VIRTIO_NET_HASH_REPORT_IPv6;

		return VIRTIO_NET_HASH_REPORT_NONE;

	default:
		return VIRTIO_NET_HASH_REPORT_NONE;
	}
}

static inline void virtio_net_hash_rss(const struct sk_buff *skb,
				       u32 types, const u32 *key,
				       struct virtio_net_hash *hash)
{
	struct virtio_net_toeplitz_state toeplitz_state = { .key = key };
	struct flow_keys flow;
	struct flow_keys_basic flow_basic;
	u16 report;

	if (!skb_flow_dissect_flow_keys(skb, &flow, 0)) {
		hash->report = VIRTIO_NET_HASH_REPORT_NONE;
		return;
	}

	flow_basic = (struct flow_keys_basic) {
		.control = flow.control,
		.basic = flow.basic
	};

	report = virtio_net_hash_report(types, &flow_basic);

	switch (report) {
	case VIRTIO_NET_HASH_REPORT_IPv4:
		virtio_net_toeplitz_calc(&toeplitz_state,
					 (__be32 *)&flow.addrs.v4addrs,
					 sizeof(flow.addrs.v4addrs));
		break;

	case VIRTIO_NET_HASH_REPORT_TCPv4:
		virtio_net_toeplitz_calc(&toeplitz_state,
					 (__be32 *)&flow.addrs.v4addrs,
					 sizeof(flow.addrs.v4addrs));
		virtio_net_toeplitz_calc(&toeplitz_state, &flow.ports.ports,
					 sizeof(flow.ports.ports));
		break;

	case VIRTIO_NET_HASH_REPORT_UDPv4:
		virtio_net_toeplitz_calc(&toeplitz_state,
					 (__be32 *)&flow.addrs.v4addrs,
					 sizeof(flow.addrs.v4addrs));
		virtio_net_toeplitz_calc(&toeplitz_state, &flow.ports.ports,
					 sizeof(flow.ports.ports));
		break;

	case VIRTIO_NET_HASH_REPORT_IPv6:
		virtio_net_toeplitz_calc(&toeplitz_state,
					 (__be32 *)&flow.addrs.v6addrs,
					 sizeof(flow.addrs.v6addrs));
		break;

	case VIRTIO_NET_HASH_REPORT_TCPv6:
		virtio_net_toeplitz_calc(&toeplitz_state,
					 (__be32 *)&flow.addrs.v6addrs,
					 sizeof(flow.addrs.v6addrs));
		virtio_net_toeplitz_calc(&toeplitz_state, &flow.ports.ports,
					 sizeof(flow.ports.ports));
		break;

	case VIRTIO_NET_HASH_REPORT_UDPv6:
		virtio_net_toeplitz_calc(&toeplitz_state,
					 (__be32 *)&flow.addrs.v6addrs,
					 sizeof(flow.addrs.v6addrs));
		virtio_net_toeplitz_calc(&toeplitz_state, &flow.ports.ports,
					 sizeof(flow.ports.ports));
		break;

	default:
		hash->report = VIRTIO_NET_HASH_REPORT_NONE;
		return;
	}

	hash->value = toeplitz_state.hash;
	hash->report = report;
}

static inline bool virtio_net_hdr_match_proto(__be16 protocol, __u8 gso_type)
{
	switch (gso_type & ~VIRTIO_NET_HDR_GSO_ECN) {
	case VIRTIO_NET_HDR_GSO_TCPV4:
		return protocol == cpu_to_be16(ETH_P_IP);
	case VIRTIO_NET_HDR_GSO_TCPV6:
		return protocol == cpu_to_be16(ETH_P_IPV6);
	case VIRTIO_NET_HDR_GSO_UDP:
	case VIRTIO_NET_HDR_GSO_UDP_L4:
		return protocol == cpu_to_be16(ETH_P_IP) ||
		       protocol == cpu_to_be16(ETH_P_IPV6);
	default:
		return false;
	}
}

static inline int virtio_net_hdr_set_proto(struct sk_buff *skb,
					   const struct virtio_net_hdr *hdr)
{
	if (skb->protocol)
		return 0;

	switch (hdr->gso_type & ~VIRTIO_NET_HDR_GSO_ECN) {
	case VIRTIO_NET_HDR_GSO_TCPV4:
	case VIRTIO_NET_HDR_GSO_UDP:
	case VIRTIO_NET_HDR_GSO_UDP_L4:
		skb->protocol = cpu_to_be16(ETH_P_IP);
		break;
	case VIRTIO_NET_HDR_GSO_TCPV6:
		skb->protocol = cpu_to_be16(ETH_P_IPV6);
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static inline int virtio_net_hdr_to_skb(struct sk_buff *skb,
					const struct virtio_net_hdr *hdr,
					bool little_endian)
{
	unsigned int nh_min_len = sizeof(struct iphdr);
	unsigned int gso_type = 0;
	unsigned int thlen = 0;
	unsigned int p_off = 0;
	unsigned int ip_proto;

	if (hdr->gso_type != VIRTIO_NET_HDR_GSO_NONE) {
		switch (hdr->gso_type & ~VIRTIO_NET_HDR_GSO_ECN) {
		case VIRTIO_NET_HDR_GSO_TCPV4:
			gso_type = SKB_GSO_TCPV4;
			ip_proto = IPPROTO_TCP;
			thlen = sizeof(struct tcphdr);
			break;
		case VIRTIO_NET_HDR_GSO_TCPV6:
			gso_type = SKB_GSO_TCPV6;
			ip_proto = IPPROTO_TCP;
			thlen = sizeof(struct tcphdr);
			nh_min_len = sizeof(struct ipv6hdr);
			break;
		case VIRTIO_NET_HDR_GSO_UDP:
			gso_type = SKB_GSO_UDP;
			ip_proto = IPPROTO_UDP;
			thlen = sizeof(struct udphdr);
			break;
		case VIRTIO_NET_HDR_GSO_UDP_L4:
			gso_type = SKB_GSO_UDP_L4;
			ip_proto = IPPROTO_UDP;
			thlen = sizeof(struct udphdr);
			break;
		default:
			return -EINVAL;
		}

		if (hdr->gso_type & VIRTIO_NET_HDR_GSO_ECN)
			gso_type |= SKB_GSO_TCP_ECN;

		if (hdr->gso_size == 0)
			return -EINVAL;
	}

	skb_reset_mac_header(skb);

	if (hdr->flags & VIRTIO_NET_HDR_F_NEEDS_CSUM) {
		u32 start = __virtio16_to_cpu(little_endian, hdr->csum_start);
		u32 off = __virtio16_to_cpu(little_endian, hdr->csum_offset);
		u32 needed = start + max_t(u32, thlen, off + sizeof(__sum16));

		if (!pskb_may_pull(skb, needed))
			return -EINVAL;

		if (!skb_partial_csum_set(skb, start, off))
			return -EINVAL;
		if (skb_transport_offset(skb) < nh_min_len)
			return -EINVAL;

		nh_min_len = skb_transport_offset(skb);
		p_off = nh_min_len + thlen;
		if (!pskb_may_pull(skb, p_off))
			return -EINVAL;
	} else {
		/* gso packets without NEEDS_CSUM do not set transport_offset.
		 * probe and drop if does not match one of the above types.
		 */
		if (gso_type && skb->network_header) {
			struct flow_keys_basic keys;

			if (!skb->protocol) {
				__be16 protocol = dev_parse_header_protocol(skb);

				if (!protocol)
					virtio_net_hdr_set_proto(skb, hdr);
				else if (!virtio_net_hdr_match_proto(protocol, hdr->gso_type))
					return -EINVAL;
				else
					skb->protocol = protocol;
			}
retry:
			if (!skb_flow_dissect_flow_keys_basic(NULL, skb, &keys,
							      NULL, 0, 0, 0,
							      0)) {
				/* UFO does not specify ipv4 or 6: try both */
				if (gso_type & SKB_GSO_UDP &&
				    skb->protocol == htons(ETH_P_IP)) {
					skb->protocol = htons(ETH_P_IPV6);
					goto retry;
				}
				return -EINVAL;
			}

			p_off = keys.control.thoff + thlen;
			if (!pskb_may_pull(skb, p_off) ||
			    keys.basic.ip_proto != ip_proto)
				return -EINVAL;

			skb_set_transport_header(skb, keys.control.thoff);
		} else if (gso_type) {
			p_off = nh_min_len + thlen;
			if (!pskb_may_pull(skb, p_off))
				return -EINVAL;
		}
	}

	if (hdr->gso_type != VIRTIO_NET_HDR_GSO_NONE) {
		u16 gso_size = __virtio16_to_cpu(little_endian, hdr->gso_size);
		unsigned int nh_off = p_off;
		struct skb_shared_info *shinfo = skb_shinfo(skb);

		switch (gso_type & ~SKB_GSO_TCP_ECN) {
		case SKB_GSO_UDP:
			/* UFO may not include transport header in gso_size. */
			nh_off -= thlen;
			break;
		case SKB_GSO_UDP_L4:
			if (!(hdr->flags & VIRTIO_NET_HDR_F_NEEDS_CSUM))
				return -EINVAL;
			if (skb->csum_offset != offsetof(struct udphdr, check))
				return -EINVAL;
			if (skb->len - p_off > gso_size * UDP_MAX_SEGMENTS)
				return -EINVAL;
			if (gso_type != SKB_GSO_UDP_L4)
				return -EINVAL;
			break;
		case SKB_GSO_TCPV4:
		case SKB_GSO_TCPV6:
			if (skb->ip_summed == CHECKSUM_PARTIAL &&
			    skb->csum_offset != offsetof(struct tcphdr, check))
				return -EINVAL;
			break;
		}

		/* Kernel has a special handling for GSO_BY_FRAGS. */
		if (gso_size == GSO_BY_FRAGS)
			return -EINVAL;

		/* Too small packets are not really GSO ones. */
		if (skb->len - nh_off > gso_size) {
			shinfo->gso_size = gso_size;
			shinfo->gso_type = gso_type;

			/* Header must be checked, and gso_segs computed. */
			shinfo->gso_type |= SKB_GSO_DODGY;
			shinfo->gso_segs = 0;
		}
	}

	return 0;
}

static inline int virtio_net_hdr_from_skb(const struct sk_buff *skb,
					  struct virtio_net_hdr *hdr,
					  bool little_endian,
					  bool has_data_valid,
					  int vlan_hlen)
{
	memset(hdr, 0, sizeof(*hdr));   /* no info leak */

	if (skb_is_gso(skb)) {
		struct skb_shared_info *sinfo = skb_shinfo(skb);

		/* This is a hint as to how much should be linear. */
		hdr->hdr_len = __cpu_to_virtio16(little_endian,
						 skb_headlen(skb));
		hdr->gso_size = __cpu_to_virtio16(little_endian,
						  sinfo->gso_size);
		if (sinfo->gso_type & SKB_GSO_TCPV4)
			hdr->gso_type = VIRTIO_NET_HDR_GSO_TCPV4;
		else if (sinfo->gso_type & SKB_GSO_TCPV6)
			hdr->gso_type = VIRTIO_NET_HDR_GSO_TCPV6;
		else if (sinfo->gso_type & SKB_GSO_UDP_L4)
			hdr->gso_type = VIRTIO_NET_HDR_GSO_UDP_L4;
		else
			return -EINVAL;
		if (sinfo->gso_type & SKB_GSO_TCP_ECN)
			hdr->gso_type |= VIRTIO_NET_HDR_GSO_ECN;
	} else
		hdr->gso_type = VIRTIO_NET_HDR_GSO_NONE;

	if (skb->ip_summed == CHECKSUM_PARTIAL) {
		hdr->flags = VIRTIO_NET_HDR_F_NEEDS_CSUM;
		hdr->csum_start = __cpu_to_virtio16(little_endian,
			skb_checksum_start_offset(skb) + vlan_hlen);
		hdr->csum_offset = __cpu_to_virtio16(little_endian,
				skb->csum_offset);
	} else if (has_data_valid &&
		   skb->ip_summed == CHECKSUM_UNNECESSARY) {
		hdr->flags = VIRTIO_NET_HDR_F_DATA_VALID;
	} /* else everything is zero */

	return 0;
}

#endif /* _LINUX_VIRTIO_NET_H */
