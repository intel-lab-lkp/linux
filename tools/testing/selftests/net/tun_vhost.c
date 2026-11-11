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
 *    only with vhost_net experimental_zcopytx set (else skipped)
 *
 * Tun schedules NAPI after a batch, after the last of a series of MSG_MORE
 * packets, once more than NAPI_POLL_WEIGHT (64) packets are queued, and
 * when it drops a packet because the queue is full. With netdev_max_backlog
 * below 64, the queue always overflows and only the last of these keeps it
 * draining while the sender keeps sending.
 *
 * netdev_max_backlog is global: the test lowers it for the whole system
 * while it runs, so it should not run in parallel with other tests.
 *
 * Author: Antonio Quartulli <antonio@mandelbit.com>
 */

#define _GNU_SOURCE
#include <endian.h>
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

/* TX virtqueue, little-endian as negotiated with VIRTIO_F_VERSION_1 */
struct tv_ring {
	void *mem;
	struct vring vr;
	char *bufs;
	int kick;
	int call;
	unsigned short avail_idx;
	unsigned short used_idx;
	/* descriptors not owned by vhost: buffers may come back in any order */
	unsigned short free[TV_RING_NUM];
	int nfree;
};

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
	struct tv_ring ring;
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

/* vhost-net uses zerocopy for any non-zero value */
static bool tv_zerocopy_enabled(void)
{
	char buf[16] = {};
	int fd;

	fd = open("/sys/module/vhost_net/parameters/experimental_zcopytx",
		  O_RDONLY);
	if (fd < 0)
		return false;
	if (read(fd, buf, sizeof(buf) - 1) <= 0)
		buf[0] = '\0';
	close(fd);
	return atoi(buf) != 0;
}

static int tv_vhost_setup(int vhost, int tap, struct tv_ring *r)
{
	struct vhost_vring_state state = { .index = TV_TX_VQ };
	struct vhost_vring_file file = { .index = TV_TX_VQ };
	struct vhost_vring_addr addr = { .index = TV_TX_VQ };
	size_t memsz = sizeof(struct vhost_memory) +
		       sizeof(struct vhost_memory_region);
	unsigned long long features;
	struct vhost_memory *mem;
	int ret;

	if (ioctl(vhost, VHOST_SET_OWNER, NULL) ||
	    ioctl(vhost, VHOST_GET_FEATURES, &features) ||
	    !(features & (1ULL << VIRTIO_F_VERSION_1)))
		return -1;
	features = 1ULL << VIRTIO_F_VERSION_1;
	if (ioctl(vhost, VHOST_SET_FEATURES, &features))
		return -1;

	/* buffer addresses are passed as is: map them 1:1 */
	mem = calloc(1, memsz);
	if (!mem)
		return -1;
	mem->nregions = 1;
	mem->regions[0].guest_phys_addr = (unsigned long)r->bufs;
	mem->regions[0].userspace_addr = (unsigned long)r->bufs;
	mem->regions[0].memory_size = TV_RING_NUM * TV_BUF_SIZE;
	ret = ioctl(vhost, VHOST_SET_MEM_TABLE, mem);
	free(mem);
	if (ret)
		return -1;

	state.num = TV_RING_NUM;
	if (ioctl(vhost, VHOST_SET_VRING_NUM, &state))
		return -1;
	state.num = 0;
	if (ioctl(vhost, VHOST_SET_VRING_BASE, &state))
		return -1;
	addr.desc_user_addr = (unsigned long)r->vr.desc;
	addr.avail_user_addr = (unsigned long)r->vr.avail;
	addr.used_user_addr = (unsigned long)r->vr.used;
	if (ioctl(vhost, VHOST_SET_VRING_ADDR, &addr))
		return -1;
	file.fd = r->kick;
	if (ioctl(vhost, VHOST_SET_VRING_KICK, &file))
		return -1;
	file.fd = r->call;
	if (ioctl(vhost, VHOST_SET_VRING_CALL, &file))
		return -1;
	file.fd = tap;
	return ioctl(vhost, VHOST_NET_SET_BACKEND, &file);
}

static int tv_ring_init(struct tv_ring *r)
{
	int i;

	if (posix_memalign((void **)&r->bufs, 4096, TV_RING_NUM * TV_BUF_SIZE))
		return -1;
	memset(r->bufs, 0, TV_RING_NUM * TV_BUF_SIZE);

	/* every buffer: zeroed vnet header + IPv4 frame for another host,
	 * which the stack drops without counting it in rx_dropped
	 */
	for (i = 0; i < TV_RING_NUM; i++) {
		unsigned char *eth = (unsigned char *)r->bufs +
				     i * TV_BUF_SIZE + TV_HDR_LEN;

		memcpy(eth, tv_mac_dst, ETH_ALEN);
		memcpy(eth + ETH_ALEN, tv_mac_src, ETH_ALEN);
		eth[12] = ETH_P_IP >> 8;
		eth[13] = ETH_P_IP & 0xff;
		eth[14] = 0x45;
		r->free[i] = i;
	}
	r->nfree = TV_RING_NUM;

	if (posix_memalign(&r->mem, 4096, vring_size(TV_RING_NUM, 4096)))
		return -1;
	memset(r->mem, 0, vring_size(TV_RING_NUM, 4096));
	vring_init(&r->vr, TV_RING_NUM, r->mem, 4096);

	r->kick = eventfd(0, EFD_NONBLOCK);
	r->call = eventfd(0, EFD_NONBLOCK);
	return r->kick < 0 || r->call < 0 ? -1 : 0;
}

/* save the global netdev_max_backlog, to restore it in teardown */
static int tv_backlog_save(int *fd, char *old, size_t size)
{
	ssize_t n;

	/* only visible from init_net: keep it open across unshare() */
	*fd = open("/proc/sys/net/core/netdev_max_backlog", O_RDWR);
	if (*fd < 0)
		return -1;
	n = read(*fd, old, size - 1);
	if (n <= 0)
		return -1;
	old[n] = '\0';
	return 0;
}

FIXTURE_SETUP(tun_vhost)
{
	struct ifreq ifr = {};
	int hdrlen = TV_HDR_LEN;
	int sndbuf = 1 << 20;
	int fd;

	self->backlog_fd = -1;
	self->vhost = open("/dev/vhost-net", O_RDWR);
	if (self->vhost < 0)
		SKIP(return, "no /dev/vhost-net");
	if (variant->zerocopy && !tv_zerocopy_enabled())
		SKIP(return, "vhost_net experimental_zcopytx not enabled");
	if (variant->batch && tv_zerocopy_enabled())
		SKIP(return, "vhost_net does not batch with zerocopy");

	ASSERT_EQ(tv_backlog_save(&self->backlog_fd, self->backlog,
				  sizeof(self->backlog)), 0);
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

	ASSERT_EQ(tv_ring_init(&self->ring), 0);
	ASSERT_EQ(tv_vhost_setup(self->vhost, self->tap, &self->ring), 0);
}

FIXTURE_TEARDOWN(tun_vhost)
{
	/* restore first: closing vhost may block if buffers are not returned */
	if (self->backlog_fd >= 0) {
		if (pwrite(self->backlog_fd, self->backlog,
			   strlen(self->backlog), 0) < 0)
			TH_LOG("cannot restore netdev_max_backlog to %s",
			       self->backlog);
		close(self->backlog_fd);
	}
	close(self->vhost);
	close(self->tap);
}

/* queue up to @n buffers, return how many */
static int tv_post(struct tv_ring *r, int pktlen, int n)
{
	unsigned long long one = 1;
	int i;

	for (i = 0; i < n && r->nfree; i++) {
		unsigned short id = r->free[--r->nfree];

		r->vr.desc[id].addr = htole64((unsigned long)r->bufs +
					      id * TV_BUF_SIZE);
		r->vr.desc[id].len = htole32(TV_HDR_LEN + pktlen);
		r->vr.desc[id].flags = 0;
		r->vr.avail->ring[r->avail_idx % TV_RING_NUM] = htole16(id);
		r->avail_idx++;
	}
	__atomic_store_n(&r->vr.avail->idx, htole16(r->avail_idx),
			 __ATOMIC_RELEASE);
	if (i && write(r->kick, &one, sizeof(one)) != sizeof(one))
		return -1;
	return i;
}

/* reap returned buffers, waiting up to @ms for at least one */
static int tv_reap(struct tv_ring *r, int ms)
{
	struct pollfd pfd = { .fd = r->call, .events = POLLIN };
	unsigned short used;
	unsigned long long v;
	int n = 0;

	for (;;) {
		used = le16toh(__atomic_load_n(&r->vr.used->idx,
					       __ATOMIC_ACQUIRE));
		while (r->used_idx != used) {
			struct vring_used_elem *e;

			e = &r->vr.used->ring[r->used_idx % TV_RING_NUM];
			r->free[r->nfree++] = le32toh(e->id);
			r->used_idx++;
			n++;
		}
		if (n)
			return n;
		if (ms <= 0 || poll(&pfd, 1, 100) < 0)
			return 0;
		if (pfd.revents & POLLIN && read(r->call, &v, sizeof(v)) < 0)
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
	/* lowered only now, so that a failed setup leaves it alone */
	ASSERT_EQ(pwrite(self->backlog_fd, TV_BACKLOG, strlen(TV_BACKLOG), 0),
		  strlen(TV_BACKLOG));

	while (reaped < TV_PKTS) {
		n = tv_post(&self->ring, variant->pktlen, TV_PKTS - posted);
		ASSERT_GE(n, 0);
		posted += n;
		n = tv_reap(&self->ring, 5000);
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
