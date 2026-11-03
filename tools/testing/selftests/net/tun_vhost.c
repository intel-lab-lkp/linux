// SPDX-License-Identifier: GPL-2.0
/*
 * Send packets from vhost-net into a tap device with IFF_NAPI and check
 * that the NAPI queue is bounded by netdev_max_backlog: excess packets
 * are dropped and counted, and every buffer is returned to the driver,
 * zerocopy ones included.
 *
 * vhost-net passes packets to tun in three ways:
 *  - batch: batches of XDP buffers (tun_xdp_one()), the default when the
 *    socket send buffer is unlimited and zerocopy is disabled
 *  - copy: one packet per sendmsg(), when the send buffer is limited
 *  - zerocopy: one packet per sendmsg() with a ubuf_info to complete,
 *    only with vhost_net experimental_zcopytx=1 (else skipped)
 *
 * Tun schedules NAPI only after a batch, or after the last of a series of
 * MSG_MORE packets, so with a small netdev_max_backlog the queue always
 * overflows. It must keep draining while the sender keeps sending.
 *
 * Author: Antonio Quartulli <antonio@mandelbit.com>
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <linux/if.h>
#include <linux/if_arp.h>
#include <linux/if_ether.h>
#include <linux/if_tun.h>
#include <linux/vhost.h>
#include <linux/virtio_config.h>
#include <linux/virtio_net.h>
#include <linux/virtio_ring.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/socket.h>

#include "kselftest_harness.h"

#define TV_IFNAME	"tvh0"
#define TV_RING_NUM	256
#define TV_BUF_SIZE	2048
#define TV_HDR_LEN	sizeof(struct virtio_net_hdr_v1)
#define TV_PKTS		(64 * TV_RING_NUM)
#define TV_BACKLOG	"8"
#define TV_TX_VQ	1

static const unsigned char tv_mac_tap[ETH_ALEN] = { 2, 0, 0, 0, 0, 1 };
static const unsigned char tv_mac_dst[ETH_ALEN] = { 2, 0, 0, 0, 0, 2 };
static const unsigned char tv_mac_src[ETH_ALEN] = { 2, 0, 0, 0, 0, 3 };

struct tv_stats {
	unsigned long long rx_packets;
	unsigned long long rx_dropped;
};

static int tv_read_stats(const char *ifname, struct tv_stats *st)
{
	char line[512], *p;
	FILE *f;
	int ret = -1;

	f = fopen("/proc/net/dev", "r");
	if (!f)
		return -1;
	while (fgets(line, sizeof(line), f)) {
		p = strchr(line, ':');
		if (!p)
			continue;
		*p = '\0';
		if (strcmp(line + strspn(line, " "), ifname))
			continue;
		if (sscanf(p + 1, "%*u %llu %*u %llu", &st->rx_packets,
			   &st->rx_dropped) == 2)
			ret = 0;
		break;
	}
	fclose(f);
	return ret;
}

FIXTURE(tun_vhost)
{
	int backlog_fd;
	char backlog[32];
	int tap;
	int vhost;
	int kick;
	int call;
	void *ring;
	struct vring vr;
	char *bufs;
	unsigned short avail_idx;
	unsigned short used_idx;
};

FIXTURE_VARIANT(tun_vhost)
{
	bool batch;
	bool limit_sndbuf;
	bool zerocopy;
	int pktlen;
};

FIXTURE_VARIANT_ADD(tun_vhost, batch)
{
	.batch = true,
	.pktlen = 64,
};

FIXTURE_VARIANT_ADD(tun_vhost, copy)
{
	.limit_sndbuf = true,
	.pktlen = 64,
};

FIXTURE_VARIANT_ADD(tun_vhost, zerocopy)
{
	.limit_sndbuf = true,
	.zerocopy = true,
	.pktlen = 1000,
};

static bool tv_zerocopy_enabled(void)
{
	char c = '0';
	int fd;

	fd = open("/sys/module/vhost_net/parameters/experimental_zcopytx",
		  O_RDONLY);
	if (fd < 0)
		return false;
	if (read(fd, &c, 1) != 1)
		c = '0';
	close(fd);
	return c == '1';
}

static void tv_vhost_setup(struct __test_metadata *_metadata,
			   FIXTURE_DATA(tun_vhost) *self)
{
	struct vhost_vring_state state = { .index = TV_TX_VQ };
	struct vhost_vring_file file = { .index = TV_TX_VQ };
	struct vhost_vring_addr addr = { .index = TV_TX_VQ };
	size_t memsz = sizeof(struct vhost_memory) +
		       sizeof(struct vhost_memory_region);
	unsigned long long features;
	struct vhost_memory *mem;

	ASSERT_EQ(ioctl(self->vhost, VHOST_SET_OWNER, NULL), 0);
	ASSERT_EQ(ioctl(self->vhost, VHOST_GET_FEATURES, &features), 0);
	ASSERT_TRUE(features & (1ULL << VIRTIO_F_VERSION_1));
	features = 1ULL << VIRTIO_F_VERSION_1;
	ASSERT_EQ(ioctl(self->vhost, VHOST_SET_FEATURES, &features), 0);

	/* buffer addresses are passed as is: map them 1:1 */
	mem = calloc(1, memsz);
	ASSERT_NE(mem, NULL);
	mem->nregions = 1;
	mem->regions[0].guest_phys_addr = (unsigned long)self->bufs;
	mem->regions[0].userspace_addr = (unsigned long)self->bufs;
	mem->regions[0].memory_size = TV_RING_NUM * TV_BUF_SIZE;
	ASSERT_EQ(ioctl(self->vhost, VHOST_SET_MEM_TABLE, mem), 0);
	free(mem);

	state.num = TV_RING_NUM;
	ASSERT_EQ(ioctl(self->vhost, VHOST_SET_VRING_NUM, &state), 0);
	state.num = 0;
	ASSERT_EQ(ioctl(self->vhost, VHOST_SET_VRING_BASE, &state), 0);
	addr.desc_user_addr = (unsigned long)self->vr.desc;
	addr.avail_user_addr = (unsigned long)self->vr.avail;
	addr.used_user_addr = (unsigned long)self->vr.used;
	ASSERT_EQ(ioctl(self->vhost, VHOST_SET_VRING_ADDR, &addr), 0);
	file.fd = self->kick;
	ASSERT_EQ(ioctl(self->vhost, VHOST_SET_VRING_KICK, &file), 0);
	file.fd = self->call;
	ASSERT_EQ(ioctl(self->vhost, VHOST_SET_VRING_CALL, &file), 0);
	file.fd = self->tap;
	ASSERT_EQ(ioctl(self->vhost, VHOST_NET_SET_BACKEND, &file), 0);
}

FIXTURE_SETUP(tun_vhost)
{
	struct ifreq ifr = {};
	int hdrlen = TV_HDR_LEN;
	int sndbuf = 1 << 20;
	ssize_t n;
	int i, fd;

	self->tap = -1;
	self->vhost = open("/dev/vhost-net", O_RDWR);
	if (self->vhost < 0)
		SKIP(return, "no /dev/vhost-net");
	if (variant->zerocopy && !tv_zerocopy_enabled())
		SKIP(return, "vhost_net experimental_zcopytx not enabled");
	if (variant->batch && tv_zerocopy_enabled())
		SKIP(return, "vhost_net does not batch with zerocopy");

	/* global, and only visible from init_net: keep it open to restore */
	self->backlog_fd = open("/proc/sys/net/core/netdev_max_backlog",
				O_RDWR);
	ASSERT_GE(self->backlog_fd, 0);
	n = read(self->backlog_fd, self->backlog, sizeof(self->backlog) - 1);
	ASSERT_GT(n, 0);
	self->backlog[n] = '\0';
	ASSERT_EQ(pwrite(self->backlog_fd, TV_BACKLOG, strlen(TV_BACKLOG), 0),
		  strlen(TV_BACKLOG));

	ASSERT_EQ(unshare(CLONE_NEWNET), 0);

	self->tap = open("/dev/net/tun", O_RDWR);
	ASSERT_GE(self->tap, 0);
	ifr.ifr_flags = IFF_TAP | IFF_NO_PI | IFF_VNET_HDR | IFF_NAPI;
	memcpy(ifr.ifr_name, TV_IFNAME, sizeof(TV_IFNAME));
	ASSERT_EQ(ioctl(self->tap, TUNSETIFF, &ifr), 0);
	ASSERT_EQ(ioctl(self->tap, TUNSETVNETHDRSZ, &hdrlen), 0);
	if (variant->limit_sndbuf)
		ASSERT_EQ(ioctl(self->tap, TUNSETSNDBUF, &sndbuf), 0);

	fd = socket(AF_INET, SOCK_DGRAM, 0);
	ASSERT_GE(fd, 0);
	ifr.ifr_hwaddr.sa_family = ARPHRD_ETHER;
	memcpy(ifr.ifr_hwaddr.sa_data, tv_mac_tap, ETH_ALEN);
	ASSERT_EQ(ioctl(fd, SIOCSIFHWADDR, &ifr), 0);
	ASSERT_EQ(ioctl(fd, SIOCGIFFLAGS, &ifr), 0);
	ifr.ifr_flags |= IFF_UP;
	ASSERT_EQ(ioctl(fd, SIOCSIFFLAGS, &ifr), 0);
	close(fd);

	/* every buffer: zeroed vnet header + IPv4 frame for another host,
	 * which the stack drops without counting it in rx_dropped
	 */
	ASSERT_EQ(posix_memalign((void **)&self->bufs, 4096,
				 TV_RING_NUM * TV_BUF_SIZE), 0);
	memset(self->bufs, 0, TV_RING_NUM * TV_BUF_SIZE);
	for (i = 0; i < TV_RING_NUM; i++) {
		unsigned char *eth = (unsigned char *)self->bufs +
				     i * TV_BUF_SIZE + TV_HDR_LEN;

		memcpy(eth, tv_mac_dst, ETH_ALEN);
		memcpy(eth + ETH_ALEN, tv_mac_src, ETH_ALEN);
		eth[12] = ETH_P_IP >> 8;
		eth[13] = ETH_P_IP & 0xff;
		eth[14] = 0x45;
	}

	ASSERT_EQ(posix_memalign(&self->ring, 4096,
				 vring_size(TV_RING_NUM, 4096)), 0);
	memset(self->ring, 0, vring_size(TV_RING_NUM, 4096));
	vring_init(&self->vr, TV_RING_NUM, self->ring, 4096);

	self->kick = eventfd(0, EFD_NONBLOCK);
	self->call = eventfd(0, EFD_NONBLOCK);
	ASSERT_GE(self->kick, 0);
	ASSERT_GE(self->call, 0);

	tv_vhost_setup(_metadata, self);
}

FIXTURE_TEARDOWN(tun_vhost)
{
	if (self->tap < 0)
		return;
	/* waits for outstanding zerocopy buffers */
	close(self->vhost);
	close(self->tap);
	if (pwrite(self->backlog_fd, self->backlog, strlen(self->backlog),
		   0) < 0)
		TH_LOG("cannot restore netdev_max_backlog to %s",
		       self->backlog);
	close(self->backlog_fd);
}

/* queue up to @n buffers, return how many */
static int tv_post(FIXTURE_DATA(tun_vhost) *self, int pktlen, int n)
{
	int inflight = (unsigned short)(self->avail_idx - self->used_idx);
	unsigned long long one = 1;
	int i;

	for (i = 0; i < n && inflight < TV_RING_NUM; i++, inflight++) {
		unsigned int id = self->avail_idx % TV_RING_NUM;

		self->vr.desc[id].addr = (unsigned long)self->bufs +
					 id * TV_BUF_SIZE;
		self->vr.desc[id].len = TV_HDR_LEN + pktlen;
		self->vr.desc[id].flags = 0;
		self->vr.avail->ring[id] = id;
		self->avail_idx++;
	}
	__atomic_store_n(&self->vr.avail->idx, self->avail_idx,
			 __ATOMIC_RELEASE);
	if (i && write(self->kick, &one, sizeof(one)) != sizeof(one))
		return -1;
	return i;
}

/* reap returned buffers, waiting up to @ms for at least one */
static int tv_reap(FIXTURE_DATA(tun_vhost) *self, int ms)
{
	struct pollfd pfd = { .fd = self->call, .events = POLLIN };
	unsigned short used;
	unsigned long long v;

	for (;;) {
		used = __atomic_load_n(&self->vr.used->idx, __ATOMIC_ACQUIRE);
		if (used != self->used_idx) {
			int n = (unsigned short)(used - self->used_idx);

			self->used_idx = used;
			return n;
		}
		if (ms <= 0 || poll(&pfd, 1, 100) < 0)
			return 0;
		if (pfd.revents & POLLIN && read(self->call, &v, sizeof(v)) < 0)
			return -1;
		ms -= 100;
	}
}

TEST_F(tun_vhost, napi_queue_bounded)
{
	struct tv_stats before, after;
	unsigned long long delivered, dropped;
	int posted = 0, reaped = 0, n;

	ASSERT_EQ(tv_read_stats(TV_IFNAME, &before), 0);

	while (reaped < TV_PKTS) {
		n = tv_post(self, variant->pktlen, TV_PKTS - posted);
		ASSERT_GE(n, 0);
		posted += n;
		n = tv_reap(self, 5000);
		ASSERT_GE(n, 0);
		if (!n) {
			TH_LOG("%d of %d buffers not returned",
			       posted - reaped, posted);
			ASSERT_GT(n, 0);
		}
		reaped += n;
	}

	ASSERT_EQ(tv_read_stats(TV_IFNAME, &after), 0);
	delivered = after.rx_packets - before.rx_packets;
	dropped = after.rx_dropped - before.rx_dropped;
	TH_LOG("sent %d, delivered %llu, dropped %llu", TV_PKTS, delivered,
	       dropped);
	/* bounded, */
	EXPECT_GT(dropped, 0);
	/* still draining, */
	EXPECT_GE(delivered, TV_PKTS / 64);
	/* and every packet accounted for */
	EXPECT_EQ(delivered + dropped, TV_PKTS);
}

TEST_HARNESS_MAIN
