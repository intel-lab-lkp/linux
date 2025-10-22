// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C)2002 USAGI/WIDE Project
 *
 * Authors
 *
 *	Mitsuru KANDA @USAGI       : IPv6 Support
 *	Kazunori MIYAZAWA @USAGI   :
 *	Kunihiro Ishiguro <kunihiro@ipinfusion.com>
 *
 *	This file is derived from net/ipv4/esp.c
 */

#include <crypto/aead.h>
#include <crypto/authenc.h>
#include <net/ip.h>
#include <net/esp.h>
#include <net/ip6_route.h>
#include <linux/icmpv6.h>
#include <net/espintcp.h>
#include <net/inet6_hashtables.h>

#ifdef CONFIG_INET6_ESPINTCP
static struct sock *esp6_find_tcp_sk(struct xfrm_state *x)
{
	struct xfrm_encap_tmpl *encap = x->encap;
	struct net *net = xs_net(x);
	__be16 sport, dport;
	struct sock *sk;

	spin_lock_bh(&x->lock);
	sport = encap->encap_sport;
	dport = encap->encap_dport;
	spin_unlock_bh(&x->lock);

	sk = __inet6_lookup_established(net, &x->id.daddr.in6, dport,
					&x->props.saddr.in6, ntohs(sport), 0, 0);
	if (!sk)
		return ERR_PTR(-ENOENT);

	if (!tcp_is_ulp_esp(sk)) {
		sock_put(sk);
		return ERR_PTR(-EINVAL);
	}

	return sk;
}

#else
static struct sock *esp6_find_tcp_sk(struct xfrm_state *x)
{
	WARN_ON(1);
	return -EOPNOTSUPP;
}
#endif

static void esp6_output_encap_csum(struct sk_buff *skb)
{
	/* UDP encap with IPv6 requires a valid checksum */
	if (*skb_mac_header(skb) == IPPROTO_UDP) {
		struct udphdr *uh = udp_hdr(skb);
		struct ipv6hdr *ip6h = ipv6_hdr(skb);
		int len = ntohs(uh->len);
		unsigned int offset = skb_transport_offset(skb);
		__wsum csum = skb_checksum(skb, offset, skb->len - offset, 0);

		uh->check = csum_ipv6_magic(&ip6h->saddr, &ip6h->daddr,
					    len, IPPROTO_UDP, csum);
		if (uh->check == 0)
			uh->check = CSUM_MANGLED_0;
	}
}

static int esp6_input_encap(struct sk_buff *skb, struct xfrm_state *x)
{
	const struct ipv6hdr *ip6h = ipv6_hdr(skb);
	int offset = skb_network_offset(skb) + sizeof(*ip6h);
	int hdr_len = skb_network_header_len(skb);
	struct xfrm_encap_tmpl *encap = x->encap;
	u8 nexthdr = ip6h->nexthdr;
	__be16 frag_off, source;
	struct udphdr *uh;
	struct tcphdr *th;
	int err = 0;

	offset = ipv6_skip_exthdr(skb, offset, &nexthdr, &frag_off);
	if (offset == -1) {
		err = -EINVAL;
		goto out;
	}

	uh = (void *)(skb->data + offset);
	th = (void *)(skb->data + offset);
	hdr_len += offset;

	switch (x->encap->encap_type) {
	case TCP_ENCAP_ESPINTCP:
		source = th->source;
		break;
	case UDP_ENCAP_ESPINUDP:
		source = uh->source;
		break;
	default:
		WARN_ON_ONCE(1);
		err = -EINVAL;
		goto out;
	}

	/*
	 * 1) if the NAT-T peer's IP or port changed then
	 *    advertise the change to the keying daemon.
	 *    This is an inbound SA, so just compare
	 *    SRC ports.
	 */
	if (!ipv6_addr_equal(&ip6h->saddr, &x->props.saddr.in6) ||
	    source != encap->encap_sport) {
		xfrm_address_t ipaddr;

		memcpy(&ipaddr.a6, &ip6h->saddr.s6_addr, sizeof(ipaddr.a6));
		km_new_mapping(x, &ipaddr, source);

		/* XXX: perhaps add an extra
		 * policy check here, to see
		 * if we should allow or
		 * reject a packet from a
		 * different source
		 * address/port.
		 */
	}

	/*
	 * 2) ignore UDP/TCP checksums in case
	 *    of NAT-T in Transport Mode, or
	 *    perform other post-processing fixes
	 *    as per draft-ietf-ipsec-udp-encaps-06,
	 *    section 3.1.2
	 */
	if (x->props.mode == XFRM_MODE_TRANSPORT)
		skb->ip_summed = CHECKSUM_UNNECESSARY;

out:
	return err;
}

static int esp6_err(struct sk_buff *skb, struct inet6_skb_parm *opt,
		    u8 type, u8 code, int offset, __be32 info)
{
	struct net *net = dev_net(skb->dev);
	const struct ipv6hdr *iph = (const struct ipv6hdr *)skb->data;
	struct ip_esp_hdr *esph = (struct ip_esp_hdr *)(skb->data + offset);
	struct xfrm_state *x;

	if (type != ICMPV6_PKT_TOOBIG &&
	    type != NDISC_REDIRECT)
		return 0;

	x = xfrm_state_lookup(net, skb->mark, (const xfrm_address_t *)&iph->daddr,
			      esph->spi, IPPROTO_ESP, AF_INET6);
	if (!x)
		return 0;

	if (type == NDISC_REDIRECT)
		ip6_redirect(skb, net, skb->dev->ifindex, 0,
			     sock_net_uid(net, NULL));
	else
		ip6_update_pmtu(skb, net, info, 0, 0, sock_net_uid(net, NULL));
	xfrm_state_put(x);

	return 0;
}

static int esp6_init_state(struct xfrm_state *x, struct netlink_ext_ack *extack)
{
	struct crypto_aead *aead;
	u32 align;
	int err;

	x->data = NULL;

	if (x->aead) {
		err = esp_init_aead(x, extack);
	} else if (x->ealg) {
		err = esp_init_authenc(x, extack);
	} else {
		NL_SET_ERR_MSG(extack, "ESP: AEAD or CRYPT must be provided");
		err = -EINVAL;
	}

	if (err)
		goto error;

	aead = x->data;

	x->props.header_len = sizeof(struct ip_esp_hdr) +
			      crypto_aead_ivsize(aead);
	switch (x->props.mode) {
	case XFRM_MODE_BEET:
		if (x->sel.family != AF_INET6)
			x->props.header_len += IPV4_BEET_PHMAXLEN +
					       (sizeof(struct ipv6hdr) - sizeof(struct iphdr));
		break;
	default:
	case XFRM_MODE_TRANSPORT:
		break;
	case XFRM_MODE_TUNNEL:
		x->props.header_len += sizeof(struct ipv6hdr);
		break;
	}

	if (x->encap) {
		struct xfrm_encap_tmpl *encap = x->encap;

		switch (encap->encap_type) {
		default:
			NL_SET_ERR_MSG(extack, "Unsupported encapsulation type for ESP");
			err = -EINVAL;
			goto error;
		case UDP_ENCAP_ESPINUDP:
			x->props.header_len += sizeof(struct udphdr);
			break;
#ifdef CONFIG_INET6_ESPINTCP
		case TCP_ENCAP_ESPINTCP:
			/* only the length field, TCP encap is done by
			 * the socket
			 */
			x->props.header_len += 2;
			break;
#endif
		}
	}

	align = ALIGN(crypto_aead_blocksize(aead), 4);
	x->props.trailer_len = align + 1 + crypto_aead_authsize(aead);

error:
	return err;
}

static int esp6_rcv_cb(struct sk_buff *skb, int err)
{
	return 0;
}

static const struct xfrm_type esp6_type = {
	.owner			= THIS_MODULE,
	.proto			= IPPROTO_ESP,
	.flags			= XFRM_TYPE_REPLAY_PROT,
	.init_state		= esp6_init_state,
	.destructor		= esp_destroy,
	.input			= esp_input,
	.input_encap		= esp6_input_encap,
	.output			= esp_output,
	.find_tcp_sk		= esp6_find_tcp_sk,
	.output_encap_csum	= esp6_output_encap_csum,
};

static struct xfrm6_protocol esp6_protocol = {
	.handler	=	xfrm6_rcv,
	.input_handler	=	xfrm_input,
	.cb_handler	=	esp6_rcv_cb,
	.err_handler	=	esp6_err,
	.priority	=	0,
};

static int __init esp6_init(void)
{
	if (xfrm_register_type(&esp6_type, AF_INET6) < 0) {
		pr_info("%s: can't add xfrm type\n", __func__);
		return -EAGAIN;
	}
	if (xfrm6_protocol_register(&esp6_protocol, IPPROTO_ESP) < 0) {
		pr_info("%s: can't add protocol\n", __func__);
		xfrm_unregister_type(&esp6_type, AF_INET6);
		return -EAGAIN;
	}

	return 0;
}

static void __exit esp6_fini(void)
{
	if (xfrm6_protocol_deregister(&esp6_protocol, IPPROTO_ESP) < 0)
		pr_info("%s: can't remove protocol\n", __func__);
	xfrm_unregister_type(&esp6_type, AF_INET6);
}

module_init(esp6_init);
module_exit(esp6_fini);

MODULE_DESCRIPTION("IPv6 ESP transformation helpers");
MODULE_LICENSE("GPL");
MODULE_ALIAS_XFRM_TYPE(AF_INET6, XFRM_PROTO_ESP);
