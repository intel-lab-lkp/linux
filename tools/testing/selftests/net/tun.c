// SPDX-License-Identifier: GPL-2.0

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <linux/if_tun.h>
#include <sys/ioctl.h>
#include <sys/socket.h>

#include "../kselftest_harness.h"
#include "tuntap_helpers.h"

static const char param_dev_vxlan_name[] = "vxlan1";
static unsigned char param_hwaddr_outer_dst[] = { 0x00, 0xfe, 0x98,
						  0x14, 0x22, 0x42 };
static unsigned char param_hwaddr_outer_src[] = { 0x00, 0xfe, 0x98,
						  0x94, 0xd2, 0x43 };
static unsigned char param_hwaddr_inner_dst[] = { 0x00, 0xfe, 0x98,
						  0x94, 0x22, 0xcc };
static unsigned char param_hwaddr_inner_src[] = { 0x00, 0xfe, 0x98,
						  0x94, 0xd2, 0xdd };

static struct in_addr param_ipaddr4_outer_dst = {
	__constant_htonl(0xac100001),
};

static struct in_addr param_ipaddr4_outer_src = {
	__constant_htonl(0xac100002),
};

static struct in_addr param_ipaddr4_inner_dst = {
	__constant_htonl(0xac100101),
};

static struct in_addr param_ipaddr4_inner_src = {
	__constant_htonl(0xac100102),
};

static struct in6_addr param_ipaddr6_outer_dst = {
	{ { 0x20, 0x02, 0x0d, 0xb8, 0x01, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 } },
};

static struct in6_addr param_ipaddr6_outer_src = {
	{ { 0x20, 0x02, 0x0d, 0xb8, 0x01, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2 } },
};

static struct in6_addr param_ipaddr6_inner_dst = {
	{ { 0x20, 0x02, 0x0d, 0xb8, 0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 } },
};

static struct in6_addr param_ipaddr6_inner_src = {
	{ { 0x20, 0x02, 0x0d, 0xb8, 0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2 } },
};

#define VN_ID 1
#define VN_PORT 4789
#define UDP_SRC_PORT 22
#define UDP_DST_PORT 48878
#define IPPREFIX_LEN 24
#define IP6PREFIX_LEN 64
#define TIMEOUT_SEC 3

#define UDP_TUNNEL_VXLAN_4IN4 0x01
#define UDP_TUNNEL_VXLAN_6IN4 0x02
#define UDP_TUNNEL_VXLAN_4IN6 0x04
#define UDP_TUNNEL_VXLAN_6IN6 0x08

#define UDP_TUNNEL_OUTER_IPV4 (UDP_TUNNEL_VXLAN_4IN4 | UDP_TUNNEL_VXLAN_6IN4)
#define UDP_TUNNEL_INNER_IPV4 (UDP_TUNNEL_VXLAN_4IN4 | UDP_TUNNEL_VXLAN_4IN6)

#define TUN_VNET_TNL_SIZE sizeof(struct virtio_net_hdr_v1_hash_tunnel)

#define MAX_VNET_TUNNEL_PACKET_SZ (TUN_VNET_TNL_SIZE + ETH_HLEN + ETH_MAX_MTU)

#define UDP_TUNNEL_VXLAN_4IN4_HDRLEN \
	(ETH_HLEN + 2 * sizeof(struct iphdr) + 8 + 2 * sizeof(struct udphdr))
#define UDP_TUNNEL_VXLAN_6IN6_HDRLEN \
	(ETH_HLEN + 2 * sizeof(struct ipv6hdr) + 8 + 2 * sizeof(struct udphdr))
#define UDP_TUNNEL_VXLAN_4IN6_HDRLEN                                    \
	(ETH_HLEN + sizeof(struct iphdr) + sizeof(struct ipv6hdr) + 8 + \
	 2 * sizeof(struct udphdr))
#define UDP_TUNNEL_VXLAN_6IN4_HDRLEN                                    \
	(ETH_HLEN + sizeof(struct ipv6hdr) + sizeof(struct iphdr) + 8 + \
	 2 * sizeof(struct udphdr))

#define UDP_TUNNEL_HDRLEN(type)                                           \
	((type) == UDP_TUNNEL_VXLAN_4IN4 ? UDP_TUNNEL_VXLAN_4IN4_HDRLEN : \
	 (type) == UDP_TUNNEL_VXLAN_6IN4 ? UDP_TUNNEL_VXLAN_6IN4_HDRLEN : \
	 (type) == UDP_TUNNEL_VXLAN_4IN6 ? UDP_TUNNEL_VXLAN_4IN6_HDRLEN : \
	 (type) == UDP_TUNNEL_VXLAN_6IN6 ? UDP_TUNNEL_VXLAN_6IN6_HDRLEN : \
					   0)

#define UDP_TUNNEL_MSS(type) (ETH_DATA_LEN - UDP_TUNNEL_HDRLEN(type))

#define UDP_TUNNEL_MAX(type, is_tap) \
	(ETH_MAX_MTU - UDP_TUNNEL_HDRLEN(type) - ((is_tap) ? ETH_HLEN : 0))

union vxlan_addr {
	struct sockaddr_in sin;
	struct sockaddr_in6 sin6;
};

struct vxlan_setup_config {
	union vxlan_addr local_ip;
	union vxlan_addr remote_ip;
	__be32 vni;
	int remote_ifindex;
	__be16 dst_port;
	unsigned char hwaddr[6];
	uint8_t csum;
};

static int tun_attach(int fd, char *dev)
{
	struct ifreq ifr;

	memset(&ifr, 0, sizeof(ifr));
	strcpy(ifr.ifr_name, dev);
	ifr.ifr_flags = IFF_ATTACH_QUEUE;

	return ioctl(fd, TUNSETQUEUE, (void *)&ifr);
}

static int tun_detach(int fd, char *dev)
{
	struct ifreq ifr;

	memset(&ifr, 0, sizeof(ifr));
	strcpy(ifr.ifr_name, dev);
	ifr.ifr_flags = IFF_DETACH_QUEUE;

	return ioctl(fd, TUNSETQUEUE, (void *)&ifr);
}

static int tun_alloc(char *dev)
{
	struct ifreq ifr;
	int fd, err;

	fd = open("/dev/net/tun", O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "can't open tun: %s\n", strerror(errno));
		return fd;
	}

	memset(&ifr, 0, sizeof(ifr));
	strcpy(ifr.ifr_name, dev);
	ifr.ifr_flags = IFF_TAP | IFF_NAPI | IFF_MULTI_QUEUE;

	err = ioctl(fd, TUNSETIFF, (void *)&ifr);
	if (err < 0) {
		fprintf(stderr, "can't TUNSETIFF: %s\n", strerror(errno));
		close(fd);
		return err;
	}
	strcpy(dev, ifr.ifr_name);
	return fd;
}

static int tun_delete(char *dev)
{
	return dev_delete(dev);
}

static size_t sockaddr_len(int family)
{
	return (family == AF_INET) ? sizeof(struct sockaddr_in) :
				     sizeof(struct sockaddr_in6);
}

static int tun_open(char *dev, const int flags, const int hdrlen,
		    const int features, const unsigned char *mac_addr)
{
	struct ifreq ifr = { 0 };
	int fd, sk = -1;

	fd = open("/dev/net/tun", O_RDWR);
	if (fd < 0) {
		perror("open");
		return -1;
	}

	ifr.ifr_flags = flags;
	if (ioctl(fd, TUNSETIFF, (void *)&ifr) < 0) {
		perror("ioctl(TUNSETIFF)");
		goto err;
	}
	strcpy(dev, ifr.ifr_name);

	if (hdrlen > 0) {
		if (ioctl(fd, TUNSETVNETHDRSZ, &hdrlen) < 0) {
			perror("ioctl(TUNSETVNETHDRSZ)");
			goto err;
		}
	}

	if (features) {
		if (ioctl(fd, TUNSETOFFLOAD, features) < 0) {
			perror("ioctl(TUNSETOFFLOAD)");
			goto err;
		}
	}

	sk = socket(PF_INET, SOCK_DGRAM, 0);
	if (sk < 0) {
		perror("socket");
		goto err;
	}

	if (ioctl(sk, SIOCGIFFLAGS, &ifr) < 0) {
		perror("ioctl(SIOCGIFFLAGS)");
		goto err;
	}

	ifr.ifr_flags |= (IFF_UP | IFF_RUNNING);
	if (ioctl(sk, SIOCSIFFLAGS, &ifr) < 0) {
		perror("ioctl(SIOCSIFFLAGS)");
		goto err;
	}

	if (mac_addr && flags & IFF_TAP) {
		ifr.ifr_hwaddr.sa_family = ARPHRD_ETHER;
		memcpy(ifr.ifr_hwaddr.sa_data, mac_addr, ETH_ALEN);

		if (ioctl(sk, SIOCSIFHWADDR, &ifr) < 0) {
			perror("ioctl(SIOCSIFHWADDR)");
			goto err;
		}
	}

out:
	if (sk >= 0)
		close(sk);
	return fd;

err:
	close(fd);
	fd = -1;
	goto out;
}

static int vxlan_fill_rtattr(struct nlmsghdr *nh, void *data)
{
	struct vxlan_setup_config *vxlan = data;

	rtattr_add_any(nh, IFLA_ADDRESS, vxlan->hwaddr, ETH_ALEN);
	return 0;
}

static int vxlan_fill_info_data(struct nlmsghdr *nh, void *data)
{
	struct vxlan_setup_config *vxlan = data;

	rtattr_add_any(nh, IFLA_VXLAN_LINK, &vxlan->remote_ifindex,
		       sizeof(vxlan->remote_ifindex));
	rtattr_add_any(nh, IFLA_VXLAN_ID, &vxlan->vni, sizeof(vxlan->vni));
	rtattr_add_any(nh, IFLA_VXLAN_PORT, &vxlan->dst_port,
		       sizeof(vxlan->dst_port));
	rtattr_add_any(nh, IFLA_VXLAN_UDP_CSUM, &vxlan->csum,
		       sizeof(vxlan->csum));

	if (vxlan->remote_ip.sin.sin_family == AF_INET) {
		rtattr_add_any(nh, IFLA_VXLAN_GROUP,
			       &vxlan->remote_ip.sin.sin_addr,
			       sizeof(struct in_addr));
		rtattr_add_any(nh, IFLA_VXLAN_LOCAL,
			       &vxlan->local_ip.sin.sin_addr,
			       sizeof(struct in_addr));
	} else {
		rtattr_add_any(nh, IFLA_VXLAN_GROUP6,
			       &vxlan->remote_ip.sin6.sin6_addr,
			       sizeof(struct in6_addr));
		rtattr_add_any(nh, IFLA_VXLAN_LOCAL6,
			       &vxlan->local_ip.sin6.sin6_addr,
			       sizeof(struct in6_addr));
	}

	return 0;
}

static int set_pmtu_discover(int fd, bool is_ipv4)
{
	int level, name, val;

	if (is_ipv4) {
		level = SOL_IP;
		name = IP_MTU_DISCOVER;
		val = IP_PMTUDISC_DO;
	} else {
		level = SOL_IPV6;
		name = IPV6_MTU_DISCOVER;
		val = IPV6_PMTUDISC_DO;
	}

	return setsockopt(fd, level, name, &val, sizeof(val));
}

static int udp_socket_open(struct sockaddr_storage *sockaddr, bool can_frag)
{
	struct timeval timeout = { .tv_sec = TIMEOUT_SEC };
	int family = sockaddr->ss_family;
	int sockfd, alen;

	sockfd = socket(family, SOCK_DGRAM, 0);
	if (sockfd < 0) {
		perror("socket");
		return -1;
	}

	alen = sockaddr_len(family);
	if (bind(sockfd, (struct sockaddr *)sockaddr, alen) < 0) {
		perror("bind");
		goto err;
	}

	if (setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &timeout,
		       sizeof(timeout)) < 0) {
		perror("setsockopt(SO_RCVTIMEO)");
		goto err;
	}

	if (!can_frag) {
		if (set_pmtu_discover(sockfd, family == AF_INET) < 0) {
			perror("set_pmtu_discover");
			goto err;
		}
	}

	return sockfd;

err:
	close(sockfd);
	return -1;
}

FIXTURE(tun)
{
	char ifname[IFNAMSIZ];
	int fd, fd2;
};

FIXTURE_SETUP(tun)
{
	memset(self->ifname, 0, sizeof(self->ifname));

	self->fd = tun_alloc(self->ifname);
	ASSERT_GE(self->fd, 0);

	self->fd2 = tun_alloc(self->ifname);
	ASSERT_GE(self->fd2, 0);
}

FIXTURE_TEARDOWN(tun)
{
	if (self->fd >= 0)
		close(self->fd);
	if (self->fd2 >= 0)
		close(self->fd2);
}

TEST_F(tun, delete_detach_close)
{
	EXPECT_EQ(tun_delete(self->ifname), 0);
	EXPECT_EQ(tun_detach(self->fd, self->ifname), -1);
	EXPECT_EQ(errno, 22);
}

TEST_F(tun, detach_delete_close)
{
	EXPECT_EQ(tun_detach(self->fd, self->ifname), 0);
	EXPECT_EQ(tun_delete(self->ifname), 0);
}

TEST_F(tun, detach_close_delete)
{
	EXPECT_EQ(tun_detach(self->fd, self->ifname), 0);
	close(self->fd);
	self->fd = -1;
	EXPECT_EQ(tun_delete(self->ifname), 0);
}

TEST_F(tun, reattach_delete_close)
{
	EXPECT_EQ(tun_detach(self->fd, self->ifname), 0);
	EXPECT_EQ(tun_attach(self->fd, self->ifname), 0);
	EXPECT_EQ(tun_delete(self->ifname), 0);
}

TEST_F(tun, reattach_close_delete)
{
	EXPECT_EQ(tun_detach(self->fd, self->ifname), 0);
	EXPECT_EQ(tun_attach(self->fd, self->ifname), 0);
	close(self->fd);
	self->fd = -1;
	EXPECT_EQ(tun_delete(self->ifname), 0);
}

FIXTURE(tun_vnet_udptnl)
{
	char ifname[IFNAMSIZ];
	int fd, sock;
};

FIXTURE_VARIANT(tun_vnet_udptnl)
{
	int tunnel_type;
	int gso_size;
	int data_size;
	int r_num_mss;
	bool is_tap, no_gso;
};

/* clang-format off */
#define TUN_VNET_UDPTNL_VARIANT_ADD(type, desc)                              \
	FIXTURE_VARIANT_ADD(tun_vnet_udptnl, desc##_1mss) {                  \
		/* send a single MSS: fall back to no GSO */                 \
		.tunnel_type = type,                                         \
		.gso_size = UDP_TUNNEL_MSS(type),                            \
		.data_size = UDP_TUNNEL_MSS(type),                           \
		.r_num_mss = 1,                                              \
		.is_tap = true,                                              \
		.no_gso = true,                                              \
	};
/* clang-format on */

TUN_VNET_UDPTNL_VARIANT_ADD(UDP_TUNNEL_VXLAN_4IN4, 4in4);
TUN_VNET_UDPTNL_VARIANT_ADD(UDP_TUNNEL_VXLAN_6IN4, 6in4);
TUN_VNET_UDPTNL_VARIANT_ADD(UDP_TUNNEL_VXLAN_4IN6, 4in6);
TUN_VNET_UDPTNL_VARIANT_ADD(UDP_TUNNEL_VXLAN_6IN6, 6in6);

FIXTURE_SETUP(tun_vnet_udptnl)
{
	const int flags = (variant->is_tap ? IFF_TAP : IFF_TUN) | IFF_VNET_HDR |
			  IFF_MULTI_QUEUE | IFF_NO_PI;
	const int features = TUN_F_CSUM | TUN_F_UDP_TUNNEL_GSO |
			     TUN_F_UDP_TUNNEL_GSO_CSUM | TUN_F_USO4 |
			     TUN_F_USO6;
	const int hdrlen = TUN_VNET_TNL_SIZE;
	int tunnel_type = variant->tunnel_type;
	struct sockaddr_storage sockaddr;
	struct vxlan_setup_config vxlan;
	int ret, family;

	self->fd = tun_open(self->ifname, flags, hdrlen, features,
			    param_hwaddr_outer_src);
	ASSERT_GE(self->fd, 0);

	family = (tunnel_type & UDP_TUNNEL_OUTER_IPV4) ? AF_INET : AF_INET6;
	if (family == AF_INET) {
		ret = ip_addr_add(self->ifname, AF_INET,
				  (void *)&param_ipaddr4_outer_src,
				  IPPREFIX_LEN);
		ret += ip_neigh_add(self->ifname, AF_INET,
				    (void *)&param_ipaddr4_outer_dst,
				    param_hwaddr_outer_dst);
	} else {
		ret = ip_addr_add(self->ifname, AF_INET6,
				  (void *)&param_ipaddr6_outer_src,
				  IP6PREFIX_LEN);
		ret += ip_neigh_add(self->ifname, AF_INET6,
				    (void *)&param_ipaddr6_outer_dst,
				    param_hwaddr_outer_dst);
	}
	ASSERT_EQ(ret, 0);

	memset(&vxlan, 0, sizeof(vxlan));
	vxlan.vni = VN_ID;
	vxlan.dst_port = htons(VN_PORT);
	vxlan.csum = 1;
	vxlan.remote_ifindex = if_nametoindex(self->ifname);
	memcpy(vxlan.hwaddr, param_hwaddr_inner_src, ETH_ALEN);

	if (tunnel_type & UDP_TUNNEL_OUTER_IPV4) {
		vxlan.remote_ip.sin.sin_family = AF_INET;
		vxlan.remote_ip.sin.sin_addr = param_ipaddr4_outer_dst;
		vxlan.local_ip.sin.sin_family = AF_INET;
		vxlan.local_ip.sin.sin_addr = param_ipaddr4_outer_src;
	} else {
		vxlan.remote_ip.sin6.sin6_family = AF_INET6;
		vxlan.remote_ip.sin6.sin6_addr = param_ipaddr6_outer_dst;
		vxlan.local_ip.sin6.sin6_family = AF_INET6;
		vxlan.local_ip.sin6.sin6_addr = param_ipaddr6_outer_src;
	}

	ret = dev_create(param_dev_vxlan_name, "vxlan", vxlan_fill_rtattr,
			 vxlan_fill_info_data, (void *)&vxlan);
	ASSERT_EQ(ret, 0);

	family = (tunnel_type & UDP_TUNNEL_INNER_IPV4) ? AF_INET : AF_INET6;
	if (family == AF_INET) {
		ret = ip_addr_add(param_dev_vxlan_name, AF_INET,
				  (void *)&param_ipaddr4_inner_src,
				  IPPREFIX_LEN);
		ret += ip_neigh_add(param_dev_vxlan_name, AF_INET,
				    (void *)&param_ipaddr4_inner_dst,
				    param_hwaddr_inner_dst);
	} else {
		ret = ip_addr_add(param_dev_vxlan_name, AF_INET6,
				  (void *)&param_ipaddr6_inner_src,
				  IP6PREFIX_LEN);
		ret += ip_neigh_add(param_dev_vxlan_name, AF_INET6,
				    (void *)&param_ipaddr6_inner_dst,
				    param_hwaddr_inner_dst);
	}
	ASSERT_EQ(ret, 0);

	memset(&sockaddr, 0, sizeof(sockaddr));
	sockaddr.ss_family = family;
	if (family == AF_INET) {
		struct sockaddr_in *addr4 = (struct sockaddr_in *)&sockaddr;

		addr4->sin_addr = param_ipaddr4_inner_src;
		addr4->sin_port = htons(UDP_SRC_PORT);
	} else {
		struct sockaddr_in6 *addr6 = (struct sockaddr_in6 *)&sockaddr;

		addr6->sin6_addr = param_ipaddr6_inner_src;
		addr6->sin6_port = htons(UDP_SRC_PORT);
	}
	self->sock = udp_socket_open(&sockaddr, false);
	ASSERT_GE(self->sock, 0);

	/* give 1000us delay to ensure the interface address is ready */
	usleep(1000);
}

FIXTURE_TEARDOWN(tun_vnet_udptnl)
{
	int ret;

	if (self->sock != -1)
		close(self->sock);

	ret = dev_delete(param_dev_vxlan_name);
	EXPECT_EQ(ret, 0);

	ret = tun_delete(self->ifname);
	EXPECT_EQ(ret, 0);
}

static int build_gso_packet_into_tun(const FIXTURE_VARIANT(tun_vnet_udptnl) *
					     variant,
				     uint8_t *buf)
{
	int tunnel_type = variant->tunnel_type;
	int payload_len = variant->data_size;
	int gso_size = variant->gso_size;
	int inner_family, outer_family;
	bool is_tap = variant->is_tap;
	uint8_t *outer_udph = NULL;
	uint8_t *cur = buf;
	int len, proto;

	len = (is_tap ? ETH_HLEN : 0) + UDP_TUNNEL_HDRLEN(tunnel_type);
	inner_family = (tunnel_type & UDP_TUNNEL_INNER_IPV4) ? AF_INET :
							       AF_INET6;
	outer_family = (tunnel_type & UDP_TUNNEL_OUTER_IPV4) ? AF_INET :
							       AF_INET6;

	cur += build_virtio_net_hdr_v1_hash_tunnel(cur, is_tap, len, gso_size,
						   outer_family, inner_family);

	if (is_tap) {
		proto = outer_family == AF_INET ? ETH_P_IP : ETH_P_IPV6;
		cur += build_eth(cur, proto, param_hwaddr_outer_dst,
				 param_hwaddr_outer_src);
		len -= ETH_HLEN;
	}

	if (outer_family == AF_INET) {
		len = len - sizeof(struct iphdr) + payload_len;
		cur += build_ipv4_header(cur, IPPROTO_UDP, len,
					 &param_ipaddr4_outer_dst,
					 &param_ipaddr4_outer_src);
	} else {
		len = len - sizeof(struct ipv6hdr) + payload_len;
		cur += build_ipv6_header(cur, IPPROTO_UDP, 0, len,
					 &param_ipaddr6_outer_dst,
					 &param_ipaddr6_outer_src);
	}

	outer_udph = cur;
	len -= sizeof(struct udphdr);
	proto = inner_family == AF_INET ? ETH_P_IP : ETH_P_IPV6;
	cur += build_udp_header(cur, UDP_SRC_PORT, VN_PORT, len);
	cur += build_vxlan_header(cur, VN_ID);
	cur += build_eth(cur, proto, param_hwaddr_inner_dst,
			 param_hwaddr_inner_src);

	len = sizeof(struct udphdr) + payload_len;
	if (inner_family == AF_INET) {
		cur += build_ipv4_header(cur, IPPROTO_UDP, len,
					 &param_ipaddr4_inner_dst,
					 &param_ipaddr4_inner_src);
	} else {
		cur += build_ipv6_header(cur, IPPROTO_UDP, 0, len,
					 &param_ipaddr6_inner_dst,
					 &param_ipaddr6_inner_src);
	}

	cur += build_udp_packet(cur, UDP_DST_PORT, UDP_SRC_PORT, payload_len,
				inner_family, false);

	build_udp_packet_csum(outer_udph, outer_family, false);

	return cur - buf;
}

static int
recieve_gso_packet_from_tunnel(FIXTURE_DATA(tun_vnet_udptnl) * self,
			       const FIXTURE_VARIANT(tun_vnet_udptnl) * variant,
			       int *r_num_mss)
{
	uint8_t packet_buf[MAX_VNET_TUNNEL_PACKET_SZ];
	int len, total_len = 0, socket = self->sock;
	int payload_len = variant->data_size;

	while (total_len < payload_len) {
		len = recv(socket, packet_buf, sizeof(packet_buf), 0);
		if (len < 0) {
			if (errno != EAGAIN && errno != EWOULDBLOCK)
				perror("recv");
			return total_len;
		}

		(*r_num_mss)++;
		total_len += len;
	}

	return total_len;
}

TEST_F(tun_vnet_udptnl, send_gso_packet)
{
	uint8_t pkt[MAX_VNET_TUNNEL_PACKET_SZ];
	int r_num_mss = 0;
	int ret, off;

	memset(pkt, 0, sizeof(pkt));
	off = build_gso_packet_into_tun(variant, pkt);
	ret = write(self->fd, pkt, off);
	ASSERT_EQ(ret, off);

	ret = recieve_gso_packet_from_tunnel(self, variant, &r_num_mss);
	ASSERT_EQ(ret, variant->data_size);
	ASSERT_EQ(r_num_mss, variant->r_num_mss);
}

TEST_HARNESS_MAIN
