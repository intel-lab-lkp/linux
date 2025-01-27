// SPDX-License-Identifier: GPL-2.0 OR Linux-OpenIB
/*
 * Copyright (c) 2016 Mellanox Technologies Ltd. All rights reserved.
 * Copyright (c) 2015 System Fabric Works, Inc. All rights reserved.
 */

#include <linux/crc32.h>

#include "rxe.h"
#include "rxe_loc.h"

/**
 * rxe_icrc() - Compute the ICRC of a packet
 * @skb: packet buffer
 * @pkt: packet information
 *
 * Return: the ICRC
 */
static u32 rxe_icrc(struct sk_buff *skb, struct rxe_pkt_info *pkt)
{
	unsigned int bth_offset = 0;
	struct iphdr *ip4h = NULL;
	struct ipv6hdr *ip6h = NULL;
	struct udphdr *udph;
	struct rxe_bth *bth;
	u32 crc;
	int hdr_size = sizeof(struct udphdr) +
		(skb->protocol == htons(ETH_P_IP) ?
		sizeof(struct iphdr) : sizeof(struct ipv6hdr));
	/* pseudo header buffer size is calculate using ipv6 header size since
	 * it is bigger than ipv4
	 */
	u8 pshdr[sizeof(struct udphdr) +
		sizeof(struct ipv6hdr) +
		RXE_BTH_BYTES];

	/* This seed is the result of computing a CRC with a seed of
	 * 0xfffffff and 8 bytes of 0xff representing a masked LRH.
	 */
	crc = 0xdebb20e3;

	if (skb->protocol == htons(ETH_P_IP)) { /* IPv4 */
		memcpy(pshdr, ip_hdr(skb), hdr_size);
		ip4h = (struct iphdr *)pshdr;
		udph = (struct udphdr *)(ip4h + 1);

		ip4h->ttl = 0xff;
		ip4h->check = CSUM_MANGLED_0;
		ip4h->tos = 0xff;
	} else {				/* IPv6 */
		memcpy(pshdr, ipv6_hdr(skb), hdr_size);
		ip6h = (struct ipv6hdr *)pshdr;
		udph = (struct udphdr *)(ip6h + 1);

		memset(ip6h->flow_lbl, 0xff, sizeof(ip6h->flow_lbl));
		ip6h->priority = 0xf;
		ip6h->hop_limit = 0xff;
	}
	udph->check = CSUM_MANGLED_0;

	bth_offset += hdr_size;

	memcpy(&pshdr[bth_offset], pkt->hdr, RXE_BTH_BYTES);
	bth = (struct rxe_bth *)&pshdr[bth_offset];

	/* exclude bth.resv8a */
	bth->qpn |= cpu_to_be32(~BTH_QPN_MASK);

	/* Update the CRC with the first part of the headers. */
	crc = crc32(crc, pshdr, hdr_size + RXE_BTH_BYTES);

	/* Update the CRC with the remainder of the headers. */
	crc = crc32(crc, pkt->hdr + RXE_BTH_BYTES,
		    rxe_opcode[pkt->opcode].length - RXE_BTH_BYTES);

	/* Update the CRC with the payload. */
	crc = crc32(crc, payload_addr(pkt), payload_size(pkt) + bth_pad(pkt));

	/* Invert the CRC and return it. */
	return ~crc;
}

/**
 * rxe_icrc_check() - Compute ICRC for a packet and compare to the ICRC
 *		      delivered in the packet.
 * @skb: packet buffer
 * @pkt: packet information
 *
 * Return: 0 if the values match else an error
 */
int rxe_icrc_check(struct sk_buff *skb, struct rxe_pkt_info *pkt)
{
	/*
	 * The usual big endian convention of InfiniBand does not apply to the
	 * ICRC field, whose transmission is specified in terms of the CRC32
	 * polynomial coefficients.  The coefficients are transmitted in
	 * descending order from the x^31 coefficient to the x^0 one.  When the
	 * result is interpreted as a 32-bit integer using the required reverse
	 * mapping between bits and polynomial coefficients, it's a __le32.
	 */
	__le32 *icrcp = (__le32 *)(pkt->hdr + pkt->paylen - RXE_ICRC_SIZE);

	if (unlikely(rxe_icrc(skb, pkt) != le32_to_cpu(*icrcp)))
		return -EINVAL;

	return 0;
}

/**
 * rxe_icrc_generate() - compute ICRC for a packet.
 * @skb: packet buffer
 * @pkt: packet information
 */
void rxe_icrc_generate(struct sk_buff *skb, struct rxe_pkt_info *pkt)
{
	__le32 *icrcp = (__le32 *)(pkt->hdr + pkt->paylen - RXE_ICRC_SIZE);

	*icrcp = cpu_to_le32(rxe_icrc(skb, pkt));
}
