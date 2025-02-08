#include "test_progs.h"
#include "network_helpers.h"

#include "so_timestamping.skel.h"

#define CG_NAME "/so-timestamping-test"

static const char addr4_str[] = "127.0.0.1";
static const char addr6_str[] = "::1";
static struct so_timestamping *skel;

static void test_tcp(int family)
{
	struct so_timestamping__bss *bss = skel->bss;
	char buf[] = "testing testing";
	int sfd = -1, cfd = -1;
	int n;

	memset(bss, 0, sizeof(*bss));

	sfd = start_server(family, SOCK_STREAM,
			   family == AF_INET6 ? addr6_str : addr4_str, 0, 0);
	if (!ASSERT_OK_FD(sfd, "start_server"))
		goto out;

	cfd = connect_to_fd(sfd, 0);
	if (!ASSERT_OK_FD(cfd, "connect_to_fd_server"))
		goto out;

	n = write(cfd, buf, sizeof(buf));
	if (!ASSERT_EQ(n, sizeof(buf), "send to server"))
		goto out;

	ASSERT_EQ(bss->nr_active, 1, "nr_active");
	ASSERT_EQ(bss->nr_snd, 2, "nr_snd");
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
	struct netns_obj *ns;
	int cg_fd;

	cg_fd = test__join_cgroup(CG_NAME);
	if (!ASSERT_OK_FD(cg_fd, "join cgroup"))
		return;

	ns = netns_new("so_timestamping_ns", true);
	if (!ASSERT_OK_PTR(ns, "create ns"))
		goto done;

	skel = so_timestamping__open_and_load();
	if (!ASSERT_OK_PTR(skel, "open and load skel"))
		goto done;

	if (!ASSERT_OK(so_timestamping__attach(skel), "attach skel"))
		goto done;

	skel->links.skops_sockopt =
		bpf_program__attach_cgroup(skel->progs.skops_sockopt, cg_fd);
	if (!ASSERT_OK_PTR(skel->links.skops_sockopt, "attach cgroup"))
		goto done;

	test_tcp(AF_INET6);
	test_tcp(AF_INET);

done:
	so_timestamping__destroy(skel);
	netns_free(ns);
	close(cg_fd);
}
