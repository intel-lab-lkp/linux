/* SPDX-License-Identifier: GPL-2.0 */
/*  IPXLAT - Stateless IP/ICMP Translation (SIIT) virtual device driver
 *
 *  Copyright (C) 2024- Alberto Leiva Popper <ydahhrk@gmail.com>
 *  Copyright (C) 2026- Mandelbit SRL
 *  Copyright (C) 2026- Daniel Gröber <dxld@darkboxed.org>
 *
 *  Author:	Alberto Leiva Popper <ydahhrk@gmail.com>
 *		Antonio Quartulli <antonio@mandelbit.com>
 *		Daniel Gröber <dxld@darkboxed.org>
 *		Ralf Lici <ralf@mandelbit.com>
 */

#ifndef _NET_IPXLAT_PACKET_H_
#define _NET_IPXLAT_PACKET_H_

#include <net/ip.h>

#include "ipxlpriv.h"

/**
 * struct ipxlat_cb - per-skb parser and control metadata stored in skb->cb
 * @l4_off: outer L4 header offset
 * @payload_off: outer payload offset
 * @fragh_off: outer IPv6 Fragment Header offset, or 0 if absent
 * @inner_l3_offset: quoted inner L3 offset for ICMP errors
 * @inner_l4_offset: quoted inner L4 offset for ICMP errors
 * @inner_fragh_off: quoted inner IPv6 Fragment Header offset, or 0
 * @udp_zero_csum_len: outer UDP length used for 4->6 checksum synthesis
 * @frag_max_size: pre-fragment payload cap for ip_do_fragment
 * @l4_proto: outer L4 protocol (or nexthdr for IPv6)
 * @inner_l4_proto: quoted inner L4 protocol
 * @l3_hdr_len: outer L3 header length including extension headers
 * @inner_l3_hdr_len: quoted inner L3 header length
 * @is_icmp_err: packet is ICMP error and carries quoted inner packet
 * @emit_icmp_err: datapath must emit translator-generated ICMP on drop
 * @icmp_err: ICMP type/code/info cached for deferred emission
 * @icmp_err.type: ICMP type to emit
 * @icmp_err.code: ICMP code to emit
 * @icmp_err.info: ICMP auxiliary info (e.g. pointer/MTU)
 */
struct ipxlat_cb {
	u16 l4_off;
	u16 payload_off;
	u16 fragh_off;
	u16 inner_l3_offset;
	u16 inner_l4_offset;
	u16 inner_fragh_off;
	/* L4 span length (UDP header + payload) for outer IPv4 UDP packets
	 * arriving with checksum 0.
	 */
	u16 udp_zero_csum_len;
	u16 frag_max_size;
	u8 l4_proto;
	u8 inner_l4_proto;
	u8 l3_hdr_len;
	u8 inner_l3_hdr_len;
	bool is_icmp_err;
	bool emit_icmp_err;
	struct {
		u8 type;
		u8 code;
		u32 info;
	} icmp_err;
};

/**
 * ipxlat_skb_cb - return ipxlat private control block in skb->cb
 * @skb: skb carrying ipxlat metadata
 *
 * Return: pointer to &struct ipxlat_cb stored in the control buffer of @skb.
 */
static inline struct ipxlat_cb *ipxlat_skb_cb(const struct sk_buff *skb)
{
	BUILD_BUG_ON(sizeof(struct ipxlat_cb) > sizeof(skb->cb));
	return (struct ipxlat_cb *)(skb->cb);
}

static inline unsigned int ipxlat_skb_datagram_len(const struct sk_buff *skb)
{
	return skb->len - skb_transport_offset(skb);
}

static inline u8 ipxlat_get_ipv6_tclass(const struct ipv6hdr *hdr)
{
	return (hdr->priority << 4) | (hdr->flow_lbl[0] >> 4);
}

static inline u16 ipxlat_get_frag6_offset(const struct frag_hdr *hdr)
{
	return be16_to_cpu(hdr->frag_off) & 0xFFF8U;
}

static inline u16 ipxlat_get_frag4_offset(const struct iphdr *hdr)
{
	return (be16_to_cpu(hdr->frag_off) & IP_OFFSET) << 3;
}

static inline bool ipxlat_is_first_frag6(const struct frag_hdr *hdr)
{
	return hdr ? (ipxlat_get_frag6_offset(hdr) == 0) : true;
}

static inline bool ipxlat_is_first_frag4(const struct iphdr *hdr)
{
	return !(hdr->frag_off & htons(IP_OFFSET));
}

static inline __be16 ipxlat_build_frag6_offset(u16 frag_offset, bool mf)
{
	return cpu_to_be16((frag_offset & 0xFFF8U) | mf);
}

static inline __be16
ipxlat_build_frag4_offset(bool df, bool mf, u16 frag_offset)
{
	return cpu_to_be16((df ? (1U << 14) : 0) | (mf ? (1U << 13) : 0) |
			   (frag_offset >> 3));
}

/**
 * ipxlat_cb_rebase_offsets - shift cached cb offsets after skb relayout
 * @cb: parsed packet metadata
 * @delta: signed byte delta applied to cached offsets
 *
 * Return: 0 on success, negative errno if rebased offsets would underflow.
 */
int ipxlat_cb_rebase_offsets(struct ipxlat_cb *cb, int delta);
#ifdef CONFIG_DEBUG_NET
/**
 * ipxlat_cb_offsets_valid - validate monotonicity and bounds of cb offsets
 * @cb: parsed packet metadata
 *
 * Return: true if cached offsets are internally consistent.
 */
bool ipxlat_cb_offsets_valid(const struct ipxlat_cb *cb);
#else
static inline bool ipxlat_cb_offsets_valid(const struct ipxlat_cb *cb)
{
	return true;
}
#endif

/**
 * ipxlat_v4_validate_skb - validate and summarize IPv4 packet into skb->cb
 * @ipxlat: translator private context
 * @skb: packet to validate
 *
 * Populates &struct ipxlat_cb and may mark translator-generated ICMP action on
 * failure paths.
 *
 * Return: 0 on success, negative errno on validation failure.
 */
int ipxlat_v4_validate_skb(struct ipxlat_priv *ipxlat, struct sk_buff *skb);

/**
 * ipxlat_v6_validate_skb - validate and summarize IPv6 packet into skb->cb
 * @skb: packet to validate
 *
 * Populates &struct ipxlat_cb for subsequent 6->4 translation.
 *
 * Return: 0 on success, negative errno on validation failure.
 */
int ipxlat_v6_validate_skb(struct sk_buff *skb);

#endif /* _NET_IPXLAT_PACKET_H_ */
