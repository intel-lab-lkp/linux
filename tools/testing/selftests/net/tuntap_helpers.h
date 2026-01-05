/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef _TUNTAP_HELPERS_H
#define _TUNTAP_HELPERS_H

#include <errno.h>
#include <linux/if_packet.h>
#include <linux/ipv6.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <linux/virtio_net.h>
#include <net/if.h>
#include <netinet/if_ether.h>
#include <netinet/ip.h>
#include <netinet/udp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define MAX_RTNL_PAYLOAD 4096
#define VXLAN_HLEN 8
#define PKT_DATA 0xCB

enum nl_op {
	NL_NEW,
	NL_DEL,
	NL_GET,
	NL_OP_MAX,
};

enum nl_type {
	NL_ADDR,
	NL_NEIGH,
	NL_ROUTE,
	NL_DEV,
};

struct nl_req_msg {
	struct nlmsghdr nh;
	union {
		struct ifaddrmsg addr_info;
		struct ndmsg neigh_info;
		struct rtmsg route_info;
		struct ifinfomsg dev_info;
	};
	unsigned char data[MAX_RTNL_PAYLOAD];
};

struct nl_req_cb {
	int (*fill_rtattr)(struct nlmsghdr *nh, void *data);
	void *cb_data;
};

struct nl_req_entry {
	int type[NL_OP_MAX];
	size_t msgsize;
	int (*fill_req)(int nl_op, void *params, struct nl_req_msg *req,
			struct nl_req_cb *cb);
	int (*check_answer)(void *params, struct nlmsghdr *nh);
};

struct nl_addr {
	const char *intf;
	int family;
	void *addr;
	uint8_t prefix;
};

struct nl_neigh {
	const char *intf;
	int family;
	void *addr;
	unsigned char *lladdr;
};

struct nl_route {
	const char *intf;
	int family;
	int type;
	int table;
	void *dst;
	uint8_t dst_len;
	void *gateway;
};

struct nl_dev {
	const char *intf;
	char *link_type;
	int (*fill_rtattr)(struct nlmsghdr *nh, void *cb_data);
	int (*fill_info_data)(struct nlmsghdr *nh, void *cb_data);
	void *cb_data;
};

static inline size_t ip_addr_len(int family)
{
	return (family == AF_INET) ? sizeof(struct in_addr) :
				     sizeof(struct in6_addr);
}

static inline struct rtattr *rtattr_add(struct nlmsghdr *nh,
					unsigned short type, unsigned short len)
{
	struct rtattr *rta =
		(struct rtattr *)((uint8_t *)nh + RTA_ALIGN(nh->nlmsg_len));

	rta->rta_type = type;
	rta->rta_len = RTA_LENGTH(len);
	nh->nlmsg_len = RTA_ALIGN(nh->nlmsg_len) + RTA_ALIGN(rta->rta_len);
	return rta;
}

static inline struct rtattr *rtattr_begin(struct nlmsghdr *nh,
					  unsigned short type)
{
	return rtattr_add(nh, type, 0);
}

static inline void rtattr_end(struct nlmsghdr *nh, struct rtattr *attr)
{
	uint8_t *end = (uint8_t *)nh + nh->nlmsg_len;

	attr->rta_len = end - (uint8_t *)attr;
}

static inline struct rtattr *rtattr_add_str(struct nlmsghdr *nh,
					    unsigned short type, const char *s)
{
	struct rtattr *rta = rtattr_add(nh, type, strlen(s));

	memcpy(RTA_DATA(rta), s, strlen(s));
	return rta;
}

static inline struct rtattr *
rtattr_add_strsz(struct nlmsghdr *nh, unsigned short type, const char *s)
{
	struct rtattr *rta = rtattr_add(nh, type, strlen(s) + 1);

	strcpy(RTA_DATA(rta), s);
	return rta;
}

static inline struct rtattr *rtattr_add_any(struct nlmsghdr *nh,
					    unsigned short type,
					    const void *arr, size_t len)
{
	struct rtattr *rta = rtattr_add(nh, type, len);

	memcpy(RTA_DATA(rta), arr, len);
	return rta;
}

static inline int fill_addr_req(int nl_op, void *params, struct nl_req_msg *req,
				struct nl_req_cb *cb)
{
	struct nl_addr *addrp = params;
	size_t ipalen;

	ipalen = ip_addr_len(addrp->family);
	req->addr_info.ifa_family = addrp->family;
	req->addr_info.ifa_index = if_nametoindex(addrp->intf);
	req->addr_info.ifa_flags = (nl_op == NL_NEW) ? IFA_F_NODAD : 0;
	req->nh.nlmsg_flags |= (nl_op == NL_GET) ? NLM_F_DUMP : 0;

	if (nl_op != NL_GET) {
		req->addr_info.ifa_prefixlen = addrp->prefix;
		rtattr_add_any(&req->nh, IFA_LOCAL, addrp->addr, ipalen);
	}
	return 0;
}

static inline int check_addr_answer(void *params, struct nlmsghdr *nh)
{
	struct rtattr *rta = (struct rtattr *)IFA_RTA(NLMSG_DATA(nh));
	struct nl_addr *addrp = params;
	size_t ipalen;
	int rtlen;

	ipalen = ip_addr_len(addrp->family);
	rtlen = nh->nlmsg_len - NLMSG_LENGTH(sizeof(struct ifaddrmsg));

	while (RTA_OK(rta, rtlen)) {
		if (rta->rta_type == IFA_ADDRESS &&
		    memcmp(RTA_DATA(rta), addrp->addr, ipalen) == 0)
			return 1;
		rta = RTA_NEXT(rta, rtlen);
	}
	return 0;
}

static inline int fill_neigh_req(int nl_op, void *params,
				 struct nl_req_msg *req, struct nl_req_cb *cb)
{
	struct nl_neigh *neighp = params;
	size_t ipalen;

	ipalen = ip_addr_len(neighp->family);
	req->neigh_info.ndm_family = neighp->family;
	req->neigh_info.ndm_ifindex = if_nametoindex(neighp->intf);
	req->neigh_info.ndm_state = (nl_op == NL_NEW) ? NUD_PERMANENT : 0;

	rtattr_add_any(&req->nh, NDA_DST, neighp->addr, ipalen);
	if (nl_op == NL_NEW)
		rtattr_add_any(&req->nh, NDA_LLADDR, neighp->lladdr, ETH_ALEN);
	return 0;
}

static inline int fill_route_req(int nl_op, void *params,
				 struct nl_req_msg *req, struct nl_req_cb *cb)
{
	struct nl_route *routep = params;
	size_t ipalen;
	int oif;

	ipalen = ip_addr_len(routep->family);
	oif = if_nametoindex(routep->intf);
	req->route_info.rtm_family = routep->family;
	req->route_info.rtm_dst_len = routep->dst_len;
	req->route_info.rtm_table = routep->table;
	req->route_info.rtm_type = routep->type;
	req->route_info.rtm_protocol = RTPROT_BOOT;
	req->route_info.rtm_scope = RT_SCOPE_UNIVERSE;

	rtattr_add_any(&req->nh, RTA_DST, routep->dst, ipalen);

	if (nl_op != NL_GET) {
		rtattr_add_any(&req->nh, RTA_GATEWAY, routep->gateway, ipalen);
		rtattr_add_any(&req->nh, RTA_OIF, &oif, sizeof(int));
	}
	return 0;
}

static inline int check_route_answer(void *params, struct nlmsghdr *nh)
{
	struct rtmsg *rtm = NLMSG_DATA(nh);
	struct nl_route *routep = params;

	return (rtm->rtm_type == routep->type) ? 1 : 0;
}

static inline int fill_dev_req(int nl_op, void *params, struct nl_req_msg *req,
			       struct nl_req_cb *cb)
{
	struct rtattr *link_info, *info_data;
	struct nl_dev *devp = params;
	int ret;

	cb->fill_rtattr = devp->fill_rtattr;
	cb->cb_data = devp->cb_data;

	req->dev_info.ifi_family = AF_UNSPEC;
	req->dev_info.ifi_type = 1;
	req->dev_info.ifi_index = 0;
	req->dev_info.ifi_flags = (nl_op == NL_NEW) ? (IFF_BROADCAST | IFF_UP) :
						      0;
	req->dev_info.ifi_change = 0xffffffff;

	rtattr_add_str(&req->nh, IFLA_IFNAME, devp->intf);

	if (nl_op == NL_NEW) {
		link_info = rtattr_begin(&req->nh, IFLA_LINKINFO);
		rtattr_add_strsz(&req->nh, IFLA_INFO_KIND, devp->link_type);

		if (devp->fill_info_data) {
			info_data = rtattr_begin(&req->nh, IFLA_INFO_DATA);
			ret = devp->fill_info_data(&req->nh, devp->cb_data);
			if (ret < 0)
				return ret;
			rtattr_end(&req->nh, info_data);
		}
		rtattr_end(&req->nh, link_info);
	}
	return 0;
}

static const struct nl_req_entry nl_req_tbl[] = {
	[NL_ADDR] = { { RTM_NEWADDR, RTM_DELADDR, RTM_GETADDR },
		      sizeof(struct ifaddrmsg),
		      fill_addr_req,
		      check_addr_answer },
	[NL_NEIGH] = { { RTM_NEWNEIGH, RTM_DELNEIGH, RTM_GETNEIGH },
		       sizeof(struct ndmsg),
		       fill_neigh_req,
		       NULL },
	[NL_ROUTE] = { { RTM_NEWROUTE, RTM_DELROUTE, RTM_GETROUTE },
		       sizeof(struct rtmsg),
		       fill_route_req,
		       check_route_answer },
	[NL_DEV] = { { RTM_NEWLINK, RTM_DELLINK, RTM_GETLINK },
		     sizeof(struct ifinfomsg),
		     fill_dev_req,
		     NULL },
};

static inline int fill_nl_req_msg(int nl_op, int nl_type, void *params,
				  struct nl_req_msg *req, struct nl_req_cb *cb)
{
	int ret;

	req->nh.nlmsg_type = nl_req_tbl[nl_type].type[nl_op];
	req->nh.nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
	req->nh.nlmsg_flags |= nl_op == NL_NEW ? (NLM_F_CREATE | NLM_F_EXCL) :
						 0;
	req->nh.nlmsg_len = NLMSG_LENGTH(nl_req_tbl[nl_type].msgsize);

	ret = nl_req_tbl[nl_type].fill_req(nl_op, params, req, cb);
	if (ret < 0)
		return ret;

	if (cb->fill_rtattr) {
		ret = cb->fill_rtattr(&req->nh, cb->cb_data);
		if (ret < 0)
			return ret;
	}

	return 0;
}

static inline int check_nl_resp_msg(int rtnl, int nl_op, int nl_type,
				    void *params, struct nl_req_msg *req)
{
	int (*check_answer)(void *, struct nlmsghdr *) =
		nl_req_tbl[nl_type].check_answer;
	char answer[MAX_RTNL_PAYLOAD];
	int len, done, ret = 0;
	struct nlmsgerr *err;
	struct nlmsghdr *nh;

	do {
		len = recv(rtnl, answer, sizeof(answer), 0);
		if (len < 0)
			return -1;

		for (nh = (struct nlmsghdr *)answer; NLMSG_OK(nh, len);
		     nh = NLMSG_NEXT(nh, len)) {
			done = !(nh->nlmsg_flags & NLM_F_MULTI);

			if (nh->nlmsg_type == NLMSG_DONE)
				goto out;

			if (nh->nlmsg_type == NLMSG_ERROR) {
				err = (struct nlmsgerr *)NLMSG_DATA(nh);
				ret = err->error;
				goto out;
			}

			if (nl_op != NL_GET || !check_answer) {
				/* treat new/del non-error msg as error */
				ret = -1;
				goto out;
			}

			if (check_answer(params, nh)) {
				ret = 1;
				goto out;
			}
		}
	} while (!done);

out:
	return ret;
}

static inline int netlink_op(int nl_op, int nl_type, void *params)
{
	struct nl_req_cb cb = { 0 };
	struct nl_req_msg req;
	int rtnl, ret;

	rtnl = socket(AF_NETLINK, SOCK_DGRAM, NETLINK_ROUTE);
	if (rtnl < 0)
		return -1;

	ret = fill_nl_req_msg(nl_op, nl_type, params, &req, &cb);
	if (ret < 0)
		goto out;

	ret = send(rtnl, &req, req.nh.nlmsg_len, 0);
	if (ret < 0)
		goto out;

	/* return 1 if found, 0 if request msg successful, < 0 if error */
	ret = check_nl_resp_msg(rtnl, nl_op, nl_type, params, &req);
out:
	close(rtnl);
	return ret;
}

static inline int ip_addr_add(const char *intf, int family, void *addr,
			      uint8_t prefix)
{
	struct nl_addr param = { intf, family, addr, prefix };

	return netlink_op(NL_NEW, NL_ADDR, &param);
}

static inline int ip_neigh_add(const char *intf, int family, void *addr,
			       unsigned char *lladdr)
{
	struct nl_neigh param = { intf, family, addr, lladdr };

	return netlink_op(NL_NEW, NL_NEIGH, &param);
}

static inline int ip_route_get(const char *intf, int family, void *dst,
			       uint8_t dst_len, int type, int table)
{
	struct nl_route param = {
		intf, family, type, table, dst, dst_len, NULL
	};

	return netlink_op(NL_GET, NL_ROUTE, &param);
}

static inline int
dev_create(const char *dev, char *link_type,
	   int (*fill_rtattr)(struct nlmsghdr *nh, void *cb_data),
	   int (*fill_info_data)(struct nlmsghdr *nh, void *cb_data),
	   void *cb_data)
{
	struct nl_dev param = { dev, link_type, fill_rtattr, fill_info_data,
				cb_data };

	return netlink_op(NL_NEW, NL_DEV, &param);
}

static inline int dev_delete(const char *dev)
{
	struct nl_dev param = { dev, NULL, NULL, NULL, NULL };

	return netlink_op(NL_DEL, NL_DEV, &param);
}

static inline size_t build_eth(uint8_t *buf, uint16_t proto, unsigned char *src,
			       unsigned char *dest)
{
	struct ethhdr *eth = (struct ethhdr *)buf;

	eth->h_proto = htons(proto);
	memcpy(eth->h_source, src, ETH_ALEN);
	memcpy(eth->h_dest, dest, ETH_ALEN);

	return ETH_HLEN;
}

static inline uint32_t add_csum(const uint8_t *buf, int len)
{
	uint16_t *sbuf = (uint16_t *)buf;
	uint32_t sum = 0;

	while (len > 1) {
		sum += *sbuf++;
		len -= 2;
	}

	if (len)
		sum += *(uint8_t *)sbuf;

	return sum;
}

static inline uint16_t finish_ip_csum(uint32_t sum)
{
	while (sum >> 16)
		sum = (sum & 0xffff) + (sum >> 16);
	return ~((uint16_t)sum);
}

static inline uint16_t build_ip_csum(const uint8_t *buf, int len, uint32_t sum)
{
	sum += add_csum(buf, len);
	return finish_ip_csum(sum);
}

static inline int build_ipv4_header(uint8_t *buf, uint8_t proto,
				    int payload_len, struct in_addr *src,
				    struct in_addr *dst)
{
	struct iphdr *iph = (struct iphdr *)buf;

	iph->ihl = 5;
	iph->version = 4;
	iph->ttl = 8;
	iph->tot_len = htons(sizeof(*iph) + payload_len);
	iph->id = htons(1337);
	iph->protocol = proto;
	iph->saddr = src->s_addr;
	iph->daddr = dst->s_addr;
	iph->check = build_ip_csum(buf, iph->ihl << 2, 0);

	return iph->ihl << 2;
}

static inline void ipv6_set_dsfield(struct ipv6hdr *ip6h, uint8_t dsfield)
{
	uint16_t val, *ptr = (uint16_t *)ip6h;

	val = ntohs(*ptr);
	val &= 0xF00F;
	val |= ((uint16_t)dsfield) << 4;
	*ptr = htons(val);
}

static inline int build_ipv6_header(uint8_t *buf, uint8_t proto,
				    uint8_t dsfield, int payload_len,
				    struct in6_addr *src, struct in6_addr *dst)
{
	struct ipv6hdr *ip6h = (struct ipv6hdr *)buf;

	ip6h->version = 6;
	ip6h->payload_len = htons(payload_len);
	ip6h->nexthdr = proto;
	ip6h->hop_limit = 8;
	ipv6_set_dsfield(ip6h, dsfield);
	memcpy(&ip6h->saddr, src, sizeof(ip6h->saddr));
	memcpy(&ip6h->daddr, dst, sizeof(ip6h->daddr));

	return sizeof(struct ipv6hdr);
}

static inline int build_vxlan_header(uint8_t *buf, uint32_t vni)
{
	uint32_t vx_vni = htonl((vni << 8) & 0xffffff00);
	uint32_t vx_flags = htonl(0x08000000);

	memcpy(buf, &vx_flags, 4);
	memcpy(buf + 4, &vx_vni, 4);
	return VXLAN_HLEN;
}

static inline int build_udp_header(uint8_t *buf, uint16_t sport, uint16_t dport,
				   int payload_len)
{
	struct udphdr *udph = (struct udphdr *)buf;

	udph->source = htons(sport);
	udph->dest = htons(dport);
	udph->len = htons(sizeof(*udph) + payload_len);
	return sizeof(*udph);
}

static inline void build_udp_packet_csum(uint8_t *buf, int family,
					 bool csum_off)
{
	struct udphdr *udph = (struct udphdr *)buf;
	size_t ipalen = ip_addr_len(family);
	uint32_t sum;

	/* No extension IPv4 and IPv6 headers addresses are the last fields */
	sum = add_csum(buf - 2 * ipalen, 2 * ipalen);
	sum += htons(IPPROTO_UDP) + udph->len;

	if (!csum_off)
		sum += add_csum(buf, udph->len);

	udph->check = finish_ip_csum(sum);
}

static inline int build_udp_packet(uint8_t *buf, uint16_t sport, uint16_t dport,
				   int payload_len, int family, bool csum_off)
{
	struct udphdr *udph = (struct udphdr *)buf;

	build_udp_header(buf, sport, dport, payload_len);
	memset(buf + sizeof(*udph), PKT_DATA, payload_len);
	build_udp_packet_csum(buf, family, csum_off);

	return sizeof(*udph) + payload_len;
}

static inline int build_virtio_net_hdr_v1_hash_tunnel(uint8_t *buf, bool is_tap,
						      int hdr_len, int gso_size,
						      int outer_family,
						      int inner_family)
{
	struct virtio_net_hdr_v1_hash_tunnel *vh_tunnel = (void *)buf;
	struct virtio_net_hdr_v1 *vh = &vh_tunnel->hash_hdr.hdr;
	int outer_iphlen, inner_iphlen, eth_hlen, gso_type;

	eth_hlen = is_tap ? ETH_HLEN : 0;
	outer_iphlen = (outer_family == AF_INET) ? sizeof(struct iphdr) :
						   sizeof(struct ipv6hdr);
	inner_iphlen = (inner_family == AF_INET) ? sizeof(struct iphdr) :
						   sizeof(struct ipv6hdr);

	vh_tunnel->outer_th_offset = eth_hlen + outer_iphlen;
	vh_tunnel->inner_nh_offset = vh_tunnel->outer_th_offset + ETH_HLEN +
				     VXLAN_HLEN + sizeof(struct udphdr);

	vh->csum_start = vh_tunnel->inner_nh_offset + inner_iphlen;
	vh->csum_offset = __builtin_offsetof(struct udphdr, check);
	vh->flags = VIRTIO_NET_HDR_F_NEEDS_CSUM;
	vh->hdr_len = hdr_len;
	vh->gso_size = gso_size;

	if (gso_size) {
		gso_type = outer_family == AF_INET ?
				   VIRTIO_NET_HDR_GSO_UDP_TUNNEL_IPV4 :
				   VIRTIO_NET_HDR_GSO_UDP_TUNNEL_IPV6;
		vh->gso_type = VIRTIO_NET_HDR_GSO_UDP_L4 | gso_type;
	}

	return sizeof(struct virtio_net_hdr_v1_hash_tunnel);
}

#endif /* _TUNTAP_HELPERS_H */
