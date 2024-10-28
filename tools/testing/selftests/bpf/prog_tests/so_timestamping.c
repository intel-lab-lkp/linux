// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2024 Tencent */

#define _GNU_SOURCE
#include <sched.h>
#include <linux/socket.h>
#include <linux/tls.h>
#include <net/if.h>

#include "test_progs.h"
#include "cgroup_helpers.h"
#include "network_helpers.h"

#include "so_timestamping.skel.h"

#define CG_NAME "/so-timestamping-test"

static const char addr4_str[] = "127.0.0.1";
static const char addr6_str[] = "::1";
static struct so_timestamping *skel;
static int cg_fd;

static int create_netns(void)
{
	if (!ASSERT_OK(unshare(CLONE_NEWNET), "create netns"))
		return -1;

	if (!ASSERT_OK(system("ip link set dev lo up"), "set lo up"))
		return -1;

	return 0;
}

static void test_tcp(int family)
{
	struct so_timestamping__bss *bss = skel->bss;
	char buf[] = "testing testing";
	int sfd = -1, cfd = -1;
	int n;

	memset(bss, 0, sizeof(*bss));

	sfd = start_server(family, SOCK_STREAM,
			   family == AF_INET6 ? addr6_str : addr4_str, 0, 0);
	if (!ASSERT_GE(sfd, 0, "start_server"))
		goto out;

	cfd = connect_to_fd(sfd, 0);
	if (!ASSERT_GE(cfd, 0, "connect_to_fd_server")) {
		close(sfd);
		goto out;
	}

	n = write(cfd, buf, sizeof(buf));
	if (!ASSERT_EQ(n, sizeof(buf), "send to server"))
		goto out;

	ASSERT_EQ(bss->nr_active, 1, "nr_active");
	ASSERT_EQ(bss->nr_passive, 1, "nr_passive");
	ASSERT_EQ(bss->nr_sched, 1, "nr_sched");
	ASSERT_EQ(bss->nr_txsw, 1, "nr_txsw");
	ASSERT_EQ(bss->nr_ack, 1, "nr_ack");

out:
	if (sfd >= 0)
		close(sfd);
	if (cfd >= 0)
		close(cfd);
}

void test_so_timestamping(void)
{
	cg_fd = test__join_cgroup(CG_NAME);
	if (cg_fd < 0)
		return;

	if (create_netns())
		goto done;

	skel = so_timestamping__open();
	if (!ASSERT_OK_PTR(skel, "open skel"))
		goto done;

	if (!ASSERT_OK(so_timestamping__load(skel), "load skel"))
		goto done;

	skel->links.skops_sockopt =
		bpf_program__attach_cgroup(skel->progs.skops_sockopt, cg_fd);
	if (!ASSERT_OK_PTR(skel->links.skops_sockopt, "attach cgroup"))
		goto done;

	test_tcp(AF_INET6);
	test_tcp(AF_INET);

done:
	so_timestamping__destroy(skel);
	close(cg_fd);
}
