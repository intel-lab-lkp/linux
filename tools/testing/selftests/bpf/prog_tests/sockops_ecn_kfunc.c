// SPDX-License-Identifier: GPL-2.0
#include <errno.h>

#include "test_progs.h"
#include "cgroup_helpers.h"
#include "network_helpers.h"

#include "sockops_ecn_kfunc.skel.h"

/* From include/net/tcp_ecn.h */
#define TCP_ECN_IN_ACCECN_OUT_ACCECN	3
#define TCP_ECN_IN_ACCECN_OUT_ECN	4
#define TCP_ECN_MODE_UNSPEC		255
#define TCP_ACCECN_OPTION_MINIMUM	1
#define TCP_ACCECN_OPTION_FULL		2
#define TCP_ACCECN_OPTION_PERSIST	3

void test_sockops_ecn_kfunc(void)
{
	struct sockops_ecn_kfunc__bss *bss;
	struct sockops_ecn_kfunc *skel;
	int cg_fd, sfd = -1, cfd = -1, afd = -1;

	cg_fd = test__join_cgroup("/sockops_ecn_kfunc");
	if (!ASSERT_OK_FD(cg_fd, "join_cgroup"))
		return;

	skel = sockops_ecn_kfunc__open_and_load();
	if (!ASSERT_OK_PTR(skel, "open_and_load"))
		goto out;
	bss = skel->bss;

	skel->links.skops_ecn =
		bpf_program__attach_cgroup(skel->progs.skops_ecn, cg_fd);
	if (!ASSERT_OK_PTR(skel->links.skops_ecn, "attach_cgroup"))
		goto out;

	sfd = start_server(AF_INET, SOCK_STREAM, "127.0.0.1", 0, 0);
	if (!ASSERT_OK_FD(sfd, "start_server"))
		goto out;

	cfd = connect_to_fd(sfd, 0);
	if (!ASSERT_OK_FD(cfd, "connect_to_fd"))
		goto out;

	/* accept() returning guarantees PASSIVE_ESTABLISHED_CB has run. */
	afd = accept(sfd, NULL, NULL);
	if (!ASSERT_OK_FD(afd, "accept"))
		goto out;

	/* BPF_SOCK_OPS_TCP_CONNECT_CB */
	ASSERT_EQ(bss->connect_bad_mode_ret, -EINVAL, "connect_bad_mode_ret");
	ASSERT_EQ(bss->connect_bad_opt_ret, -EINVAL, "connect_bad_opt_ret");
	ASSERT_EQ(bss->connect_set_mode_ret, 0, "connect_set_mode_ret");
	ASSERT_EQ(bss->connect_mode_read, TCP_ECN_IN_ACCECN_OUT_ACCECN,
		  "connect_mode_read");
	ASSERT_EQ(bss->connect_set_opt_ret, 0, "connect_set_opt_ret");
	ASSERT_EQ(bss->connect_opt_read, TCP_ACCECN_OPTION_FULL,
		  "connect_opt_read");
	ASSERT_EQ(bss->connect_unspec_mode_ret, 0, "connect_unspec_mode_ret");
	ASSERT_EQ(bss->connect_unspec_mode_read, TCP_ECN_MODE_UNSPEC,
		  "connect_unspec_mode_read");

	/* BPF_SOCK_OPS_ACTIVE_ESTABLISHED_CB */
	ASSERT_EQ(bss->estab_set_mode_ret, -EOPNOTSUPP, "estab_set_mode_ret");
	ASSERT_EQ(bss->estab_set_opt_ret, 0, "estab_set_opt_ret");
	ASSERT_EQ(bss->estab_opt_read, TCP_ACCECN_OPTION_MINIMUM,
		  "estab_opt_read");

	/*
	 * BPF_SOCK_OPS_TCP_LISTEN_CB -> BPF_SOCK_OPS_PASSIVE_ESTABLISHED_CB
	 * The listener's values are consulted for SYN negotiation and, like
	 * other per-socket settings, inherited by the accepted child.
	 */
	ASSERT_EQ(bss->listen_set_mode_ret, 0, "listen_set_mode_ret");
	ASSERT_EQ(bss->listen_set_opt_ret, 0, "listen_set_opt_ret");
	ASSERT_EQ(bss->passive_mode_read, TCP_ECN_IN_ACCECN_OUT_ECN,
		  "passive_mode_read");
	ASSERT_EQ(bss->passive_opt_read, TCP_ACCECN_OPTION_PERSIST,
		  "passive_opt_read");

out:
	if (afd >= 0)
		close(afd);
	if (cfd >= 0)
		close(cfd);
	if (sfd >= 0)
		close(sfd);
	sockops_ecn_kfunc__destroy(skel);
	close(cg_fd);
}
