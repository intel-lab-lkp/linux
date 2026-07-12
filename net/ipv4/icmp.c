// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *	NET3:	Implementation of the ICMP protocol layer.
 *
 *		Alan Cox, <alan@lxorguk.ukuu.org.uk>
 *
 *	Some of the function names and the icmp unreach table for this
 *	module were derived from [icmp.c 1.0.11 06/02/93] by
 *	Ross Biro, Fred N. van Kempen, Mark Evans, Alan Cox, Gerhard Koerting.
 *	Other than that this module is a complete rewrite.
 *
 *	Fixes:
 *	Clemens Fruhwirth	:	introduce global icmp rate limiting
 *					with icmp type masking ability instead
 *					of broken per type icmp timeouts.
 *		Mike Shaver	:	RFC1122 checks.
 *		Alan Cox	:	Multicast ping reply as self.
 *		Alan Cox	:	Fix atomicity lockup in ip_build_xmit
 *					call.
 *		Alan Cox	:	Added 216,128 byte paths to the MTU
 *					code.
 *		Martin Mares	:	RFC1812 checks.
 *		Martin Mares	:	Can be configured to follow redirects
 *					if acting as a router _without_ a
 *					routing protocol (RFC 1812).
 *		Martin Mares	:	Echo requests may be configured to
 *					be ignored (RFC 1812).
 *		Martin Mares	:	Limitation of ICMP error message
 *					transmit rate (RFC 1812).
 *		Martin Mares	:	TOS and Precedence set correctly
 *					(RFC 1812).
 *		Martin Mares	:	Now copying as much data from the
 *					original packet as we can without
 *					exceeding 576 bytes (RFC 1812).
 *	Willy Konynenberg	:	Transparent proxying support.
 *		Keith Owens	:	RFC1191 correction for 4.2BSD based
 *					path MTU bug.
 *		Thomas Quinot	:	ICMP Dest Unreach codes up to 15 are
 *					valid (RFC 1812).
 *		Andi Kleen	:	Check all packet lengths properly
 *					and moved all kfree_skb() up to
 *					icmp_rcv.
 *		Andi Kleen	:	Move the rate limit bookkeeping
 *					into the dest entry and use a token
 *					bucket filter (thanks to ANK). Make
 *					the rates sysctl configurable.
 *		Yu Tianli	:	Fixed two ugly bugs in icmp_send
 *					- IP option length was accounted wrongly
 *					- ICMP header length was not accounted
 *					  at all.
 *              Tristan Greaves :       Added sysctl option to ignore bogus
 *              			broadcast responses from broken routers.
 *
 * To Fix:
 *
 *	- Should use skb_pull() instead of all the manual checking.
 *	  This would also greatly simply some upper layer error handlers. --AK
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/types.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/fcntl.h>
#include <linux/nospec.h>
#include <linux/socket.h>
#include <linux/in.h>
#include <linux/inet.h>
#include <linux/inetdevice.h>
#include <linux/netdevice.h>
#include <linux/string.h>
#include <linux/netfilter_ipv4.h>
#include <linux/slab.h>
#include <net/flow.h>
#include <net/snmp.h>
#include <net/ip.h>
#include <net/route.h>
#include <net/protocol.h>
#include <net/icmp.h>
#include <net/tcp.h>
#include <net/udp.h>
#include <net/raw.h>
#include <net/ping.h>
#include <linux/skbuff.h>
#include <net/sock.h>
#include <linux/errno.h>
#include <linux/timer.h>
#include <linux/init.h>
#include <linux/uaccess.h>
#include <net/checksum.h>
#include <net/xfrm.h>
#include <net/inet_common.h>
#include <net/ip_fib.h>
#include <net/l3mdev.h>
#include <net/addrconf.h>
#include <net/inet_dscp.h>

/**
 * icmp_global_allow - Are we allowed to send one more ICMP message ?
 * @net: network namespace
 *
 * Uses a token bucket to limit our ICMP messages to ~sysctl_icmp_msgs_per_sec.
 * Returns false if we reached the limit and can not send another packet.
 * Works in tandem with icmp_global_consume().
 */
bool icmp_global_allow(struct net *net)
{
	u32 delta, now, oldstamp;
	int incr, new, old;

	/* Note: many cpus could find this condition true.
	 * Then later icmp_global_consume() could consume more credits,
	 * this is an acceptable race.
	 */
	if (atomic_read(&net->ipv4.icmp_global_credit) > 0)
		return true;

	now = jiffies;
	oldstamp = READ_ONCE(net->ipv4.icmp_global_stamp);
	delta = min_t(u32, now - oldstamp, HZ);
	if (delta < HZ / 50)
		return false;

	incr = READ_ONCE(net->ipv4.sysctl_icmp_msgs_per_sec);
	incr = div_u64((u64)incr * delta, HZ);
	if (!incr)
		return false;

	if (cmpxchg(&net->ipv4.icmp_global_stamp, oldstamp, now) == oldstamp) {
		old = atomic_read(&net->ipv4.icmp_global_credit);
		do {
			new = min(old + incr, READ_ONCE(net->ipv4.sysctl_icmp_msgs_burst));
		} while (!atomic_try_cmpxchg(&net->ipv4.icmp_global_credit, &old, new));
	}
	return true;
}

void icmp_global_consume(struct net *net)
{
	int credits = get_random_u32_below(3);

	/* Note: this might make icmp_global.credit negative. */
	if (credits)
		atomic_sub(credits, &net->ipv4.icmp_global_credit);
}

/*
 *	Maintain the counters used in the SNMP statistics for outgoing ICMP
 */
void icmp_out_count(struct net *net, unsigned char type)
{
	ICMPMSGOUT_INC_STATS(net, type);
	ICMP_INC_STATS(net, ICMP_MIB_OUTMSGS);
}

/*	Helper for icmp_echo and icmpv6_echo_reply.
 *	Searches for net_device that matches PROBE interface identifier
 *		and builds PROBE reply message in icmphdr.
 *
 *	Returns false if PROBE responses are disabled via sysctl
 */

bool icmp_build_probe(struct sk_buff *skb, struct icmphdr *icmphdr)
{
	struct net *net = dev_net_rcu(skb->dev);
	struct icmp_ext_hdr *ext_hdr, _ext_hdr;
	struct icmp_ext_echo_iio *iio, _iio;
	struct inet6_dev *in6_dev;
	struct in_device *in_dev;
	struct net_device *dev;
	char buff[IFNAMSIZ];
	u16 ident_len;
	u8 status;

	if (!READ_ONCE(net->ipv4.sysctl_icmp_echo_enable_probe))
		return false;

	/* We currently only support probing interfaces on the proxy node
	 * Check to ensure L-bit is set
	 */
	if (!(ntohs(icmphdr->un.echo.sequence) & 1))
		return false;
	/* Clear status bits in reply message */
	icmphdr->un.echo.sequence &= htons(0xFF00);
	if (icmphdr->type == ICMP_EXT_ECHO)
		icmphdr->type = ICMP_EXT_ECHOREPLY;
	else
		icmphdr->type = ICMPV6_EXT_ECHO_REPLY;
	ext_hdr = skb_header_pointer(skb, 0, sizeof(_ext_hdr), &_ext_hdr);
	/* Size of iio is class_type dependent.
	 * Only check header here and assign length based on ctype in the switch statement
	 */
	iio = skb_header_pointer(skb, sizeof(_ext_hdr), sizeof(iio->extobj_hdr), &_iio);
	if (!ext_hdr || !iio)
		goto send_mal_query;
	if (ntohs(iio->extobj_hdr.length) <= sizeof(iio->extobj_hdr) ||
	    ntohs(iio->extobj_hdr.length) > sizeof(_iio))
		goto send_mal_query;
	ident_len = ntohs(iio->extobj_hdr.length) - sizeof(iio->extobj_hdr);
	iio = skb_header_pointer(skb, sizeof(_ext_hdr),
				 sizeof(iio->extobj_hdr) + ident_len, &_iio);
	if (!iio)
		goto send_mal_query;

	status = 0;
	dev = NULL;
	switch (iio->extobj_hdr.class_type) {
	case ICMP_EXT_ECHO_CTYPE_NAME:
		if (ident_len >= IFNAMSIZ)
			goto send_mal_query;
		memset(buff, 0, sizeof(buff));
		memcpy(buff, &iio->ident.name, ident_len);
		dev = dev_get_by_name(net, buff);
		break;
	case ICMP_EXT_ECHO_CTYPE_INDEX:
		if (ident_len != sizeof(iio->ident.ifindex))
			goto send_mal_query;
		dev = dev_get_by_index(net, ntohl(iio->ident.ifindex));
		break;
	case ICMP_EXT_ECHO_CTYPE_ADDR:
		if (ident_len < sizeof(iio->ident.addr.ctype3_hdr) ||
		    ident_len != sizeof(iio->ident.addr.ctype3_hdr) +
				 iio->ident.addr.ctype3_hdr.addrlen)
			goto send_mal_query;
		switch (ntohs(iio->ident.addr.ctype3_hdr.afi)) {
#if IS_ENABLED(CONFIG_IPV4)
		case ICMP_AFI_IP:
			if (iio->ident.addr.ctype3_hdr.addrlen != sizeof(struct in_addr))
				goto send_mal_query;
			dev = ip_dev_find(net, iio->ident.addr.ip_addr.ipv4_addr);
			break;
#endif
#if IS_ENABLED(CONFIG_IPV6)
		case ICMP_AFI_IP6:
			if (iio->ident.addr.ctype3_hdr.addrlen != sizeof(struct in6_addr))
				goto send_mal_query;
			dev = ipv6_dev_find(net, &iio->ident.addr.ip_addr.ipv6_addr, dev);
			dev_hold(dev);
			break;
#endif
		default:
			goto send_mal_query;
		}
		break;
	default:
		goto send_mal_query;
	}
	if (!dev) {
		icmphdr->code = ICMP_EXT_CODE_NO_IF;
		return true;
	}
	/* Fill bits in reply message */
	if (dev->flags & IFF_UP)
		status |= ICMP_EXT_ECHOREPLY_ACTIVE;

	in_dev = __in_dev_get_rcu(dev);
	if (in_dev && rcu_access_pointer(in_dev->ifa_list))
		status |= ICMP_EXT_ECHOREPLY_IPV4;

	in6_dev = __in6_dev_get(dev);
	if (in6_dev && !list_empty(&in6_dev->addr_list))
		status |= ICMP_EXT_ECHOREPLY_IPV6;

	dev_put(dev);
	icmphdr->un.echo.sequence |= htons(status);
	return true;
send_mal_query:
	icmphdr->code = ICMP_EXT_CODE_MAL_QUERY;
	return true;
}

static bool ip_icmp_error_rfc4884_validate(const struct sk_buff *skb, int off)
{
	struct icmp_extobj_hdr *objh, _objh;
	struct icmp_ext_hdr *exth, _exth;
	u16 olen;

	exth = skb_header_pointer(skb, off, sizeof(_exth), &_exth);
	if (!exth)
		return false;
	if (exth->version != 2)
		return true;

	if (exth->checksum &&
	    csum_fold(skb_checksum(skb, off, skb->len - off, 0)))
		return false;

	off += sizeof(_exth);
	while (off < skb->len) {
		objh = skb_header_pointer(skb, off, sizeof(_objh), &_objh);
		if (!objh)
			return false;

		olen = ntohs(objh->length);
		if (olen < sizeof(_objh))
			return false;

		off += olen;
		if (off > skb->len)
			return false;
	}

	return true;
}

void ip_icmp_error_rfc4884(const struct sk_buff *skb,
			   struct sock_ee_data_rfc4884 *out,
			   int thlen, int off)
{
	int hlen;

	/* original datagram headers: end of icmph to payload (skb->data) */
	hlen = -skb_transport_offset(skb) - thlen;

	/* per rfc 4884: minimal datagram length of 128 bytes */
	if (off < 128 || off < hlen)
		return;

	/* kernel has stripped headers: return payload offset in bytes */
	off -= hlen;
	if (off + sizeof(struct icmp_ext_hdr) > skb->len)
		return;

	out->len = off;

	if (!ip_icmp_error_rfc4884_validate(skb, off))
		out->flags |= SO_EE_RFC4884_FLAG_INVALID;
}

static int __net_init icmp_sk_init(struct net *net)
{
	/* Control parameters for ECHO replies. */
	net->ipv4.sysctl_icmp_echo_ignore_all = 0;
	net->ipv4.sysctl_icmp_echo_enable_probe = 0;
	net->ipv4.sysctl_icmp_echo_ignore_broadcasts = 1;

	/* Control parameter - ignore bogus broadcast responses? */
	net->ipv4.sysctl_icmp_ignore_bogus_error_responses = 1;

	/*
	 * 	Configurable global rate limit.
	 *
	 *	ratelimit defines tokens/packet consumed for dst->rate_token
	 *	bucket ratemask defines which icmp types are ratelimited by
	 *	setting	it's bit position.
	 *
	 *	default:
	 *	dest unreachable (3), source quench (4),
	 *	time exceeded (11), parameter problem (12)
	 */

	net->ipv4.sysctl_icmp_ratelimit = 1 * HZ;
	net->ipv4.sysctl_icmp_ratemask = 0x1818;
	net->ipv4.sysctl_icmp_errors_use_inbound_ifaddr = 0;
	net->ipv4.sysctl_icmp_errors_extension_mask = 0;
	net->ipv4.sysctl_icmp_msgs_per_sec = 10000;
	net->ipv4.sysctl_icmp_msgs_burst = 10000;

	return 0;
}

static struct pernet_operations __net_initdata icmp_sk_ops = {
       .init = icmp_sk_init,
};

int __init icmp_init(void)
{
#if IS_ENABLED(CONFIG_IPV4)
	int err;

	err = icmp_init_ipv4();
	if (err)
		return err;
#endif
	return register_pernet_subsys(&icmp_sk_ops);
}
