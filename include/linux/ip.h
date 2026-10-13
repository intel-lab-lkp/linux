/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * INET		An implementation of the TCP/IP protocol suite for the LINUX
 *		operating system.  INET is implemented using the  BSD Socket
 *		interface as the means of communication with the user level.
 *
 *		Definitions for the IP protocol.
 *
 * Version:	@(#)ip.h	1.0.2	04/28/93
 *
 * Authors:	Fred N. van Kempen, <waltje@uWalt.NL.Mugnet.ORG>
 */
#ifndef _LINUX_IP_H
#define _LINUX_IP_H

#include <linux/skbuff.h>
#include <uapi/linux/ip.h>

static inline struct iphdr *ip_hdr(const struct sk_buff *skb)
{
	return (struct iphdr *)skb_network_header(skb);
}

static inline struct iphdr *inner_ip_hdr(const struct sk_buff *skb)
{
	return (struct iphdr *)skb_inner_network_header(skb);
}

static inline struct iphdr *ipip_hdr(const struct sk_buff *skb)
{
	return (struct iphdr *)skb_transport_header(skb);
}

static inline unsigned int ip_transport_len(const struct sk_buff *skb)
{
	return ntohs(ip_hdr(skb)->tot_len) - skb_network_header_len(skb);
}

static inline unsigned int iph_totlen(const struct sk_buff *skb, const struct iphdr *iph)
{
	u32 len = ntohs(iph->tot_len);

	return (len || !skb_is_gso(skb) || !skb_is_gso_tcp(skb)) ?
	       len : skb->len - skb_network_offset(skb);
}

static inline unsigned int skb_ip_totlen(const struct sk_buff *skb)
{
	return iph_totlen(skb, ip_hdr(skb));
}

/* IPv4 datagram length is stored into 16bit field (tot_len) */
#define IP_MAX_MTU	0xFFFFU

static inline void iph_set_totlen(struct iphdr *iph, unsigned int len)
{
	iph->tot_len = len <= IP_MAX_MTU ? htons(len) : 0;
}

/*
 * Pull and minimally validate the IPv4 header at @nhoff.
 * Returns the header and stores the transport header offset in
 * @thoff, or NULL if truncated or malformed.  Only the fixed header
 * is guaranteed linear; callers that need the options or the
 * transport header must pull further.
 */
static inline struct iphdr *ip4_parse(struct sk_buff *skb,
				      unsigned int nhoff,
				      unsigned int *thoff)
{
	struct iphdr *iph;

	if (!pskb_may_pull(skb, nhoff + sizeof(struct iphdr)))
		return NULL;

	iph = (struct iphdr *)(skb->data + nhoff);
	if (iph->version != 4 || iph->ihl < sizeof(struct iphdr) / sizeof(u32))
		return NULL;

	*thoff = nhoff + iph->ihl * sizeof(u32);
	return iph;
}
#endif	/* _LINUX_IP_H */
