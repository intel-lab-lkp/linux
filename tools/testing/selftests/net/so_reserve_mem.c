// SPDX-License-Identifier: GPL-2.0

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/sock_diag.h>
#include <netinet/in.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "kselftest_harness.h"

#ifndef SO_RESERVE_MEM
#define SO_RESERVE_MEM 73
#endif

#ifndef IPPROTO_MPTCP
#define IPPROTO_MPTCP 262
#endif

static int get_reserve_mem(struct __test_metadata *_metadata, int fd)
{
	int val = -1;
	socklen_t len = sizeof(val);

	EXPECT_EQ(getsockopt(fd, SOL_SOCKET, SO_RESERVE_MEM, &val, &len), 0);
	return val;
}

static __u32 get_fwd_alloc(struct __test_metadata *_metadata, int fd)
{
	__u32 meminfo[SK_MEMINFO_VARS] = {};
	socklen_t len = sizeof(meminfo);

	EXPECT_EQ(getsockopt(fd, SOL_SOCKET, SO_MEMINFO, meminfo, &len), 0);
	return meminfo[SK_MEMINFO_FWD_ALLOC];
}

FIXTURE(so_reserve_mem)
{
	char cg_root[64];
	char cg_child[128];
	long page_size;
	bool cg_mounted;
};

FIXTURE_SETUP(so_reserve_mem)
{
	char procs_path[160], ctrl_path[160];
	int fd, ret;

	self->page_size = sysconf(_SC_PAGESIZE);
	ASSERT_GT(self->page_size, 0);

	if (unshare(CLONE_NEWNS))
		SKIP(return, "Failed to unshare mount namespace (need root)");

	mount("none", "/", NULL, MS_REC | MS_PRIVATE, NULL);

	snprintf(self->cg_root, sizeof(self->cg_root),
		 "/tmp/ksft_so_reserve_XXXXXX");
	ASSERT_NE(mkdtemp(self->cg_root), NULL);

	if (mount("none", self->cg_root, "cgroup2", 0, NULL)) {
		rmdir(self->cg_root);
		SKIP(return, "Failed to mount cgroup2 (need root)");
	}

	snprintf(ctrl_path, sizeof(ctrl_path), "%s/cgroup.subtree_control",
		 self->cg_root);
	fd = open(ctrl_path, O_WRONLY);
	if (fd >= 0) {
		ret = write(fd, "+memory", 7);
		(void)ret;
		close(fd);
	}

	snprintf(self->cg_child, sizeof(self->cg_child), "%s/test_%d",
		 self->cg_root, getpid());
	if (mkdir(self->cg_child, 0755)) {
		umount2(self->cg_root, MNT_DETACH);
		rmdir(self->cg_root);
		ASSERT_TRUE(false);
	}

	snprintf(procs_path, sizeof(procs_path), "%s/cgroup.procs",
		 self->cg_child);
	fd = open(procs_path, O_WRONLY);
	if (fd < 0 || dprintf(fd, "%d\n", getpid()) <= 0) {
		if (fd >= 0)
			close(fd);
		rmdir(self->cg_child);
		umount2(self->cg_root, MNT_DETACH);
		rmdir(self->cg_root);
		ASSERT_TRUE(false);
	}
	close(fd);
	self->cg_mounted = true;
}

FIXTURE_TEARDOWN(so_reserve_mem)
{
	char procs_path[160];
	int fd;

	if (!self->cg_mounted)
		return;

	snprintf(procs_path, sizeof(procs_path), "%s/cgroup.procs",
		 self->cg_root);
	fd = open(procs_path, O_WRONLY);
	if (fd >= 0) {
		dprintf(fd, "%d\n", getpid());
		close(fd);
	}
	rmdir(self->cg_child);
	umount2(self->cg_root, MNT_DETACH);
	rmdir(self->cg_root);
}

static void check_non_tcp_rejected(struct __test_metadata *_metadata,
				   int domain, int type, int protocol,
				   int val)
{
	int fd = socket(domain, type, protocol);

	if (fd < 0) {
		EXPECT_TRUE(errno == EAFNOSUPPORT || errno == EPROTONOSUPPORT);
		return;
	}
	EXPECT_EQ(setsockopt(fd, SOL_SOCKET, SO_RESERVE_MEM, &val, sizeof(val)), -1);
	EXPECT_EQ(errno, EOPNOTSUPP);
	close(fd);
}

TEST_F(so_reserve_mem, non_tcp_rejected)
{
	int val = self->page_size * 4;

	check_non_tcp_rejected(_metadata, AF_INET, SOCK_DGRAM, 0, val);
	check_non_tcp_rejected(_metadata, AF_UNIX, SOCK_STREAM, 0, val);
	check_non_tcp_rejected(_metadata, AF_INET, SOCK_RAW, IPPROTO_ICMP, val);
	check_non_tcp_rejected(_metadata, AF_INET, SOCK_STREAM, IPPROTO_MPTCP, val);
}

TEST_F(so_reserve_mem, grow_shrink_and_rounding)
{
	int ps = self->page_size;
	int fd, val;

	fd = socket(AF_INET, SOCK_STREAM, 0);
	ASSERT_GE(fd, 0);

	EXPECT_EQ(get_reserve_mem(_metadata, fd), 0);
	EXPECT_EQ(get_fwd_alloc(_metadata, fd), 0U);

	/* Negative or > INT_MAX >> 1 value -> EINVAL */
	val = -1;
	EXPECT_EQ(setsockopt(fd, SOL_SOCKET, SO_RESERVE_MEM, &val, sizeof(val)), -1);
	EXPECT_EQ(errno, EINVAL);

	val = (INT_MAX >> 1) + 1;
	EXPECT_EQ(setsockopt(fd, SOL_SOCKET, SO_RESERVE_MEM, &val, sizeof(val)), -1);
	EXPECT_EQ(errno, EINVAL);

	val = INT_MAX;
	EXPECT_EQ(setsockopt(fd, SOL_SOCKET, SO_RESERVE_MEM, &val, sizeof(val)), -1);
	EXPECT_EQ(errno, EINVAL);

	/* 1 byte rounds up to 1 page */
	val = 1;
	ASSERT_EQ(setsockopt(fd, SOL_SOCKET, SO_RESERVE_MEM, &val, sizeof(val)), 0);
	EXPECT_EQ(get_reserve_mem(_metadata, fd), ps);
	EXPECT_EQ(get_fwd_alloc(_metadata, fd), (__u32)ps);

	/* Grow to 16 pages */
	val = 16 * ps;
	ASSERT_EQ(setsockopt(fd, SOL_SOCKET, SO_RESERVE_MEM, &val, sizeof(val)), 0);
	EXPECT_EQ(get_reserve_mem(_metadata, fd), 16 * ps);
	EXPECT_EQ(get_fwd_alloc(_metadata, fd), (__u32)(16 * ps));

	/* Shrink by 1 byte (rounds delta down to 0 -> stays 16 pages) */
	val = 16 * ps - 1;
	ASSERT_EQ(setsockopt(fd, SOL_SOCKET, SO_RESERVE_MEM, &val, sizeof(val)), 0);
	EXPECT_EQ(get_reserve_mem(_metadata, fd), 16 * ps);
	EXPECT_EQ(get_fwd_alloc(_metadata, fd), (__u32)(16 * ps));

	/* Shrink to 4 pages */
	val = 4 * ps;
	ASSERT_EQ(setsockopt(fd, SOL_SOCKET, SO_RESERVE_MEM, &val, sizeof(val)), 0);
	EXPECT_EQ(get_reserve_mem(_metadata, fd), 4 * ps);
	EXPECT_EQ(get_fwd_alloc(_metadata, fd), (__u32)(4 * ps));

	/* Release all */
	val = 0;
	ASSERT_EQ(setsockopt(fd, SOL_SOCKET, SO_RESERVE_MEM, &val, sizeof(val)), 0);
	EXPECT_EQ(get_reserve_mem(_metadata, fd), 0);
	EXPECT_EQ(get_fwd_alloc(_metadata, fd), 0U);

	close(fd);
}

TEST_F(so_reserve_mem, cgroup_memory_max)
{
	char max_path[160];
	int ps = self->page_size;
	int fd, max_fd, val;

	snprintf(max_path, sizeof(max_path), "%s/memory.max", self->cg_child);
	max_fd = open(max_path, O_WRONLY);
	if (max_fd < 0)
		SKIP(return, "cgroup memory controller not delegated");

	fd = socket(AF_INET, SOCK_STREAM, 0);
	ASSERT_GE(fd, 0);

	/* Limit cgroup memory to 8 pages and try to reserve 64 pages */
	ASSERT_GT(dprintf(max_fd, "%ld\n", 8L * ps), 0);

	val = 64 * ps;
	EXPECT_EQ(setsockopt(fd, SOL_SOCKET, SO_RESERVE_MEM, &val, sizeof(val)), -1);
	EXPECT_EQ(errno, ENOMEM);
	EXPECT_EQ(get_reserve_mem(_metadata, fd), 0);

	/* Restore unlimited memory.max */
	ASSERT_GT(dprintf(max_fd, "max\n"), 0);
	close(max_fd);

	val = 4 * ps;
	ASSERT_EQ(setsockopt(fd, SOL_SOCKET, SO_RESERVE_MEM, &val, sizeof(val)), 0);
	EXPECT_EQ(get_reserve_mem(_metadata, fd), 4 * ps);

	close(fd);
}

TEST_F(so_reserve_mem, accept_child_zero_reserve)
{
	struct sockaddr_in addr = {
		.sin_family = AF_INET,
		.sin_addr.s_addr = htonl(INADDR_LOOPBACK),
	};
	socklen_t alen = sizeof(addr);
	int ps = self->page_size;
	int lfd, cfd, sfd, val;

	lfd = socket(AF_INET, SOCK_STREAM, 0);
	ASSERT_GE(lfd, 0);

	/* Set SO_RESERVE_MEM on listener before listen() */
	val = 4 * ps;
	ASSERT_EQ(setsockopt(lfd, SOL_SOCKET, SO_RESERVE_MEM, &val, sizeof(val)), 0);
	ASSERT_EQ(bind(lfd, (struct sockaddr *)&addr, sizeof(addr)), 0);
	ASSERT_EQ(listen(lfd, 2), 0);
	ASSERT_EQ(getsockname(lfd, (struct sockaddr *)&addr, &alen), 0);

	/* Grow SO_RESERVE_MEM on listener after listen() */
	val = 8 * ps;
	ASSERT_EQ(setsockopt(lfd, SOL_SOCKET, SO_RESERVE_MEM, &val, sizeof(val)), 0);
	EXPECT_EQ(get_reserve_mem(_metadata, lfd), 8 * ps);
	EXPECT_EQ(get_fwd_alloc(_metadata, lfd), (__u32)(8 * ps));

	cfd = socket(AF_INET, SOCK_STREAM, 0);
	ASSERT_GE(cfd, 0);
	ASSERT_EQ(connect(cfd, (struct sockaddr *)&addr, sizeof(addr)), 0);

	sfd = accept(lfd, NULL, NULL);
	ASSERT_GE(sfd, 0);

	/* Child after accept() must have 0 reserve while listener keeps 8 pages */
	EXPECT_EQ(get_reserve_mem(_metadata, sfd), 0);
	EXPECT_EQ(get_fwd_alloc(_metadata, sfd), 0U);
	EXPECT_EQ(get_reserve_mem(_metadata, lfd), 8 * ps);
	EXPECT_EQ(get_fwd_alloc(_metadata, lfd), (__u32)(8 * ps));

	/* Child can still independently set its own SO_RESERVE_MEM */
	val = 6 * ps;
	ASSERT_EQ(setsockopt(sfd, SOL_SOCKET, SO_RESERVE_MEM, &val, sizeof(val)), 0);
	EXPECT_EQ(get_reserve_mem(_metadata, sfd), 6 * ps);
	EXPECT_EQ(get_fwd_alloc(_metadata, sfd), (__u32)(6 * ps));

	close(sfd);
	close(cfd);
	close(lfd);
}

TEST_F(so_reserve_mem, preserved_after_traffic)
{
	struct sockaddr_in addr = {
		.sin_family = AF_INET,
		.sin_addr.s_addr = htonl(INADDR_LOOPBACK),
	};
	socklen_t alen = sizeof(addr);
	int ps = self->page_size;
	int lfd, cfd, sfd, val;
	char buf[8192] = {};

	lfd = socket(AF_INET, SOCK_STREAM, 0);
	ASSERT_GE(lfd, 0);
	ASSERT_EQ(bind(lfd, (struct sockaddr *)&addr, sizeof(addr)), 0);
	ASSERT_EQ(listen(lfd, 1), 0);
	ASSERT_EQ(getsockname(lfd, (struct sockaddr *)&addr, &alen), 0);

	cfd = socket(AF_INET, SOCK_STREAM, 0);
	ASSERT_GE(cfd, 0);
	val = 16 * ps;
	ASSERT_EQ(setsockopt(cfd, SOL_SOCKET, SO_RESERVE_MEM, &val, sizeof(val)), 0);
	ASSERT_EQ(connect(cfd, (struct sockaddr *)&addr, sizeof(addr)), 0);

	sfd = accept(lfd, NULL, NULL);
	ASSERT_GE(sfd, 0);

	/* Send & drain traffic; cfd must retain its 16-page forward alloc,
	 * while sfd (0 reserve) reclaims its forward alloc back to 0.
	 */
	ASSERT_EQ(send(cfd, buf, sizeof(buf), 0), (ssize_t)sizeof(buf));
	ASSERT_EQ(recv(sfd, buf, sizeof(buf), MSG_WAITALL), (ssize_t)sizeof(buf));

	EXPECT_GE(get_fwd_alloc(_metadata, cfd), (__u32)(16 * ps));
	EXPECT_EQ(get_fwd_alloc(_metadata, sfd), 0U);

	close(sfd);
	close(cfd);
	close(lfd);
}

TEST_HARNESS_MAIN
