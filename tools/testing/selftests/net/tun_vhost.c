// SPDX-License-Identifier: GPL-2.0
/*
 * Feed a tap device with IFF_NAPI from vhost-net, in batches of XDP buffers,
 * as single packets with MSG_MORE, or as zerocopy buffers, with
 * netdev_max_backlog set to 8: excess packets must be dropped and counted,
 * the queue must keep draining, and every buffer must come back.
 *
 * Author: Antonio Quartulli <antonio@mandelbit.com>
 */

#define _GNU_SOURCE
#include <endian.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <linux/if.h>
#include <linux/if_tun.h>
#include <linux/vhost.h>
#include <linux/virtio_config.h>
#include <linux/virtio_net.h>
#include <linux/virtio_ring.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/socket.h>

#include "kselftest_harness.h"

#define RING	256
#define BUFSZ	2048
#define HDRLEN	sizeof(struct virtio_net_hdr_v1)
#define PKTS	(64 * RING)

static long read_long(const char *fmt, const char *arg, long def)
{
	char path[128];
	long v = def;
	FILE *f;

	snprintf(path, sizeof(path), fmt, arg);
	f = fopen(path, "r");
	if (f) {
		if (fscanf(f, "%ld", &v) != 1)
			v = def;
		fclose(f);
	}
	return v;
}

FIXTURE(tun_vhost)
{
	char ifname[IFNAMSIZ];
	char bl[16];		/* saved netdev_max_backlog */
	int bl_fd;
	struct vring vr;
	int vhost;
	int kick;
	int tap;
};

FIXTURE_VARIANT(tun_vhost)
{
	bool limit_sndbuf;	/* no XDP batches */
	bool zerocopy;
	int pktlen;
};

FIXTURE_VARIANT_ADD(tun_vhost, batch)
{
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

FIXTURE_SETUP(tun_vhost)
{
	struct ifreq ifr = { .ifr_flags = IFF_TAP | IFF_NO_PI | IFF_VNET_HDR |
					  IFF_NAPI };
	unsigned long long features = 1ULL << VIRTIO_F_VERSION_1;
	struct vhost_vring_state state = { .index = 1, .num = RING };
	struct vhost_vring_file file = { .index = 1 };
	struct vhost_vring_addr addr = { .index = 1 };
	int hdrlen = HDRLEN, sndbuf = 1 << 20, i, fd;
	struct vhost_memory *mem;
	unsigned char *bufs;
	void *ring;
	bool zc;

	self->bl_fd = -1;
	self->vhost = open("/dev/vhost-net", O_RDWR);
	if (self->vhost < 0)
		SKIP(return, "no /dev/vhost-net");
	zc = read_long("/sys/module/vhost_net/parameters/%s",
		       "experimental_zcopytx", 0);
	if (variant->zerocopy && !zc)
		SKIP(return, "vhost_net experimental_zcopytx not set");
	if (!variant->limit_sndbuf && zc)
		SKIP(return, "vhost_net does not batch with zerocopy");

	self->tap = open("/dev/net/tun", O_RDWR);
	ASSERT_GE(self->tap, 0);
	strcpy(ifr.ifr_name, "tvh%d");
	ASSERT_EQ(ioctl(self->tap, TUNSETIFF, &ifr), 0);
	memcpy(self->ifname, ifr.ifr_name, IFNAMSIZ);
	ASSERT_EQ(ioctl(self->tap, TUNSETVNETHDRSZ, &hdrlen), 0);
	if (variant->limit_sndbuf)
		ASSERT_EQ(ioctl(self->tap, TUNSETSNDBUF, &sndbuf), 0);
	fd = socket(AF_INET, SOCK_DGRAM, 0);
	ASSERT_EQ(ioctl(fd, SIOCGIFFLAGS, &ifr), 0);
	ifr.ifr_flags |= IFF_UP;
	ASSERT_EQ(ioctl(fd, SIOCSIFFLAGS, &ifr), 0);
	close(fd);

	/* each buffer: vnet header and an IPv4 frame for another host, which
	 * the stack drops without counting it
	 */
	ASSERT_EQ(posix_memalign((void **)&bufs, 4096, RING * BUFSZ), 0);
	memset(bufs, 0, RING * BUFSZ);
	ASSERT_EQ(posix_memalign(&ring, 4096, vring_size(RING, 4096)), 0);
	memset(ring, 0, vring_size(RING, 4096));
	vring_init(&self->vr, RING, ring, 4096);
	for (i = 0; i < RING; i++) {
		unsigned char *eth = bufs + i * BUFSZ + HDRLEN;

		eth[0] = 0x02;
		eth[5] = 0x02;
		eth[12] = 0x08;
		eth[14] = 0x45;
		self->vr.desc[i].addr = htole64((unsigned long)eth - HDRLEN);
		self->vr.desc[i].len = htole32(HDRLEN + variant->pktlen);
		self->vr.avail->ring[i] = htole16(i);
	}

	/* buffer addresses are passed as is: map them 1:1 */
	mem = calloc(1, sizeof(*mem) + sizeof(mem->regions[0]));
	ASSERT_NE(mem, NULL);
	mem->nregions = 1;
	mem->regions[0].guest_phys_addr = (unsigned long)bufs;
	mem->regions[0].userspace_addr = (unsigned long)bufs;
	mem->regions[0].memory_size = RING * BUFSZ;

	self->kick = eventfd(0, 0);
	ASSERT_GE(self->kick, 0);
	addr.desc_user_addr = (unsigned long)self->vr.desc;
	addr.avail_user_addr = (unsigned long)self->vr.avail;
	addr.used_user_addr = (unsigned long)self->vr.used;
	ASSERT_EQ(ioctl(self->vhost, VHOST_SET_OWNER, NULL), 0);
	ASSERT_EQ(ioctl(self->vhost, VHOST_SET_FEATURES, &features), 0);
	ASSERT_EQ(ioctl(self->vhost, VHOST_SET_MEM_TABLE, mem), 0);
	ASSERT_EQ(ioctl(self->vhost, VHOST_SET_VRING_NUM, &state), 0);
	state.num = 0;
	ASSERT_EQ(ioctl(self->vhost, VHOST_SET_VRING_BASE, &state), 0);
	ASSERT_EQ(ioctl(self->vhost, VHOST_SET_VRING_ADDR, &addr), 0);
	file.fd = self->kick;
	ASSERT_EQ(ioctl(self->vhost, VHOST_SET_VRING_KICK, &file), 0);
	file.fd = self->tap;
	ASSERT_EQ(ioctl(self->vhost, VHOST_NET_SET_BACKEND, &file), 0);
	free(mem);
}

FIXTURE_TEARDOWN(tun_vhost)
{
	/* restore first: closing vhost blocks if buffers are not returned */
	if (self->bl_fd >= 0 &&
	    pwrite(self->bl_fd, self->bl, strlen(self->bl), 0) < 0)
		TH_LOG("cannot restore netdev_max_backlog to %s", self->bl);
	close(self->vhost);
	close(self->tap);
}

TEST_F(tun_vhost, napi_queue_bounded)
{
	const char *stat = "/sys/class/net/%s/statistics/rx_packets";
	const char *drop = "/sys/class/net/%s/statistics/rx_dropped";
	long rx = read_long(stat, self->ifname, -1);
	long dropped = read_long(drop, self->ifname, -1);
	unsigned short idx = 0;
	int n, ms;

	/* lowered only here, so that a failed setup leaves it alone */
	self->bl_fd = open("/proc/sys/net/core/netdev_max_backlog", O_RDWR);
	ASSERT_GE(self->bl_fd, 0);
	n = pread(self->bl_fd, self->bl, sizeof(self->bl) - 1, 0);
	ASSERT_GT(n, 0);
	self->bl[n] = '\0';
	ASSERT_EQ(pwrite(self->bl_fd, "8", 1, 0), 1);

	/* post the whole ring, wait for every buffer to come back, repeat */
	for (n = 0; n < PKTS; n += RING) {
		idx += RING;
		__atomic_store_n(&self->vr.avail->idx, htole16(idx),
				 __ATOMIC_RELEASE);
		ASSERT_EQ(eventfd_write(self->kick, 1), 0);
		for (ms = 0; le16toh(__atomic_load_n(&self->vr.used->idx,
						     __ATOMIC_ACQUIRE)) != idx;
		     ms++) {
			ASSERT_LT(ms, 5000) TH_LOG("buffers not returned");
			usleep(1000);
		}
	}

	rx = read_long(stat, self->ifname, -1) - rx;
	dropped = read_long(drop, self->ifname, -1) - dropped;
	TH_LOG("sent %d, delivered %ld, dropped %ld", PKTS, rx, dropped);
	EXPECT_GT(dropped, 0);		/* bounded */
	EXPECT_GE(rx, PKTS / 16);	/* draining while the sender sends */
	EXPECT_EQ(rx + dropped, PKTS);
}

TEST_HARNESS_MAIN
