// SPDX-License-Identifier: GPL-2.0

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <net/if.h>
#include <linux/if_tun.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <linux/virtio_net.h>
#include <netinet/ip.h>
#include <netinet/udp.h>
#include "kselftest_harness.h"
#include "tuntap_helpers.h"

static const char param_dev_tap_name[] = "xmacvtap0";
static const char param_dev_dummy_name[] = "xdummy0";
static unsigned char param_hwaddr_src[] = { 0x00, 0xfe, 0x98, 0x14, 0x22, 0x42 };
static unsigned char param_hwaddr_dest[] = {
	0x00, 0xfe, 0x98, 0x94, 0xd2, 0x43
};

static struct in_addr param_ipaddr_src = {
	__constant_htonl(0xac110002),
};

static struct in_addr param_ipaddr_dst = {
	__constant_htonl(0xac110001),
};

#define UDP_SRC_PORT 22
#define UDP_DST_PORT 58822
#define TEST_PACKET_SZ (sizeof(struct virtio_net_hdr) + ETH_HLEN + ETH_MAX_MTU)

struct mactap_setup_config {
	char name[IFNAMSIZ];
	unsigned char hwaddr[ETH_ALEN];
};

static int macvtap_fill_rtattr(struct nlmsghdr *nh, void *data)
{
	struct mactap_setup_config *mactap = data;
	int ifindex;

	ifindex = if_nametoindex(mactap->name);
	if (ifindex == 0) {
		fprintf(stderr, "%s: ifindex  %s\n", __func__, strerror(errno));
		return -errno;
	}

	rtattr_add_any(nh, IFLA_LINK, &ifindex, sizeof(ifindex));
	rtattr_add_any(nh, IFLA_ADDRESS, mactap->hwaddr, ETH_ALEN);
	return 0;
}

static int opentap(const char *devname)
{
	int ifindex;
	char buf[256];
	int fd;
	struct ifreq ifr;

	ifindex = if_nametoindex(devname);
	if (ifindex == 0) {
		fprintf(stderr, "%s: ifindex %s\n", __func__, strerror(errno));
		return -errno;
	}

	sprintf(buf, "/dev/tap%d", ifindex);
	fd = open(buf, O_RDWR | O_NONBLOCK);
	if (fd < 0) {
		fprintf(stderr, "%s: open %s\n", __func__, strerror(errno));
		return -errno;
	}

	memset(&ifr, 0, sizeof(ifr));
	strcpy(ifr.ifr_name, devname);
	ifr.ifr_flags = IFF_TAP | IFF_NO_PI | IFF_VNET_HDR | IFF_MULTI_QUEUE;
	if (ioctl(fd, TUNSETIFF, &ifr, sizeof(ifr)) < 0)
		return -errno;
	return fd;
}

size_t build_test_packet_valid_udp_gso(uint8_t *buf, size_t payload_len)
{
	uint8_t *cur = buf;
	struct virtio_net_hdr *vh = (struct virtio_net_hdr *)buf;

	vh->hdr_len = ETH_HLEN + sizeof(struct iphdr) + sizeof(struct udphdr);
	vh->flags = VIRTIO_NET_HDR_F_NEEDS_CSUM;
	vh->csum_start = ETH_HLEN + sizeof(struct iphdr);
	vh->csum_offset = __builtin_offsetof(struct udphdr, check);
	vh->gso_type = VIRTIO_NET_HDR_GSO_UDP;
	vh->gso_size = ETH_DATA_LEN - sizeof(struct iphdr);
	cur += sizeof(*vh);

	cur += build_eth(cur, ETH_P_IP, param_hwaddr_src, param_hwaddr_dest);
	cur += build_ipv4_header(cur, IPPROTO_UDP,
				 payload_len + sizeof(struct udphdr),
				 &param_ipaddr_src, &param_ipaddr_dst);
	cur += build_udp_packet(cur, UDP_SRC_PORT, UDP_DST_PORT, payload_len,
				AF_INET, true);

	return cur - buf;
}

size_t build_test_packet_valid_udp_csum(uint8_t *buf, size_t payload_len)
{
	uint8_t *cur = buf;
	struct virtio_net_hdr *vh = (struct virtio_net_hdr *)buf;

	vh->flags = VIRTIO_NET_HDR_F_DATA_VALID;
	vh->gso_type = VIRTIO_NET_HDR_GSO_NONE;
	cur += sizeof(*vh);

	cur += build_eth(cur, ETH_P_IP, param_hwaddr_src, param_hwaddr_dest);
	cur += build_ipv4_header(cur, IPPROTO_UDP,
				 payload_len + sizeof(struct udphdr),
				 &param_ipaddr_src, &param_ipaddr_dst);
	cur += build_udp_packet(cur, UDP_SRC_PORT, UDP_DST_PORT, payload_len,
				AF_INET, false);

	return cur - buf;
}

size_t build_test_packet_crash_tap_invalid_eth_proto(uint8_t *buf,
						     size_t payload_len)
{
	uint8_t *cur = buf;
	struct virtio_net_hdr *vh = (struct virtio_net_hdr *)buf;

	vh->hdr_len = ETH_HLEN + sizeof(struct iphdr) + sizeof(struct udphdr);
	vh->flags = 0;
	vh->gso_type = VIRTIO_NET_HDR_GSO_UDP;
	vh->gso_size = ETH_DATA_LEN - sizeof(struct iphdr);
	cur += sizeof(*vh);

	cur += build_eth(cur, ETH_P_IP, param_hwaddr_src, param_hwaddr_dest);
	cur += sizeof(struct iphdr) + sizeof(struct udphdr);
	cur += build_ipv4_header(cur, IPPROTO_UDP,
				 payload_len + sizeof(struct udphdr),
				 &param_ipaddr_src, &param_ipaddr_dst);
	cur += build_udp_packet(cur, UDP_SRC_PORT, UDP_DST_PORT, payload_len,
				AF_INET, true);
	cur += payload_len;

	return cur - buf;
}

FIXTURE(tap)
{
	int fd;
};

FIXTURE_SETUP(tap)
{
	int ret;
	struct mactap_setup_config mactap_config;

	ret = dev_create(param_dev_dummy_name, "dummy", NULL, NULL, NULL);
	EXPECT_EQ(ret, 0);

	strcpy(mactap_config.name, param_dev_dummy_name);
	memcpy(mactap_config.hwaddr, param_hwaddr_src, ETH_ALEN);
	ret = dev_create(param_dev_tap_name, "macvtap", macvtap_fill_rtattr,
			 NULL, &mactap_config);
	EXPECT_EQ(ret, 0);

	self->fd = opentap(param_dev_tap_name);
	ASSERT_GE(self->fd, 0);
}

FIXTURE_TEARDOWN(tap)
{
	int ret;

	if (self->fd != -1)
		close(self->fd);

	ret = dev_delete(param_dev_tap_name);
	EXPECT_EQ(ret, 0);

	ret = dev_delete(param_dev_dummy_name);
	EXPECT_EQ(ret, 0);
}

TEST_F(tap, test_packet_valid_udp_gso)
{
	uint8_t pkt[TEST_PACKET_SZ];
	size_t off;
	int ret;

	memset(pkt, 0, sizeof(pkt));
	off = build_test_packet_valid_udp_gso(pkt, 1021);
	ret = write(self->fd, pkt, off);
	ASSERT_EQ(ret, off);
}

TEST_F(tap, test_packet_valid_udp_csum)
{
	uint8_t pkt[TEST_PACKET_SZ];
	size_t off;
	int ret;

	memset(pkt, 0, sizeof(pkt));
	off = build_test_packet_valid_udp_csum(pkt, 1024);
	ret = write(self->fd, pkt, off);
	ASSERT_EQ(ret, off);
}

TEST_F(tap, test_packet_crash_tap_invalid_eth_proto)
{
	uint8_t pkt[TEST_PACKET_SZ];
	size_t off;
	int ret;

	memset(pkt, 0, sizeof(pkt));
	off = build_test_packet_crash_tap_invalid_eth_proto(pkt, 1024);
	ret = write(self->fd, pkt, off);
	ASSERT_EQ(ret, -1);
	ASSERT_EQ(errno, EINVAL);
}

TEST_HARNESS_MAIN
