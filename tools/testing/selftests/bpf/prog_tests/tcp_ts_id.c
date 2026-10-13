// SPDX-License-Identifier: GPL-2.0
#include <errno.h>

#include <test_progs.h>
#include <network_helpers.h>

#include "test_tcp_ts_id_kern.skel.h"

#define LO_ADDR6		"::1"
#define CG_NAME			"/tcp-ts-id-test"

#ifndef TCP_TS_ID
#define TCP_TS_ID		47
#define TCP_TS_ID_BITS_SHIFT	8
#endif

#define TS_ID_BITS_SYSCTL	"/proc/sys/net/ipv4/tcp_ts_id_bits"
#define TIMESTAMPS_SYSCTL	"/proc/sys/net/ipv4/tcp_timestamps"
#define EXPECTED_TS_ID		7
#define EXPECTED_TS_ID_BITS	4
#define EXPECTED_TS_ID_BITS_STR	"4"

static char saved_ts_id_bits[16];
static char saved_timestamps[16];

static int save_sysctl(const char *path, char *buf)
{
	int err = 0;
	FILE *f;

	f = fopen(path, "r");
	if (!f)
		return -errno;

	if (fscanf(f, "%15s", buf) != 1)
		err = -EINVAL;
	fclose(f);

	return err;
}

static void restore_sysctl(const char *path, const char *buf)
{
	if (buf[0])
		write_sysctl(path, buf);
}

static void run_ts_id_test(int cg_fd, bool decline)
{
	int listen_fd = -1, cli_fd = -1, accept_fd = -1;
	struct test_tcp_ts_id_kern *skel;
	socklen_t optlen;
	int ts_id;

	skel = test_tcp_ts_id_kern__open_and_load();
	if (!ASSERT_OK_PTR(skel, "open_and_load"))
		return;

	skel->bss->ts_id_reply = EXPECTED_TS_ID;
	skel->bss->ts_id_decline = decline;

	skel->links.tcp_ts_id_sockops =
		bpf_program__attach_cgroup(skel->progs.tcp_ts_id_sockops,
					   cg_fd);
	if (!ASSERT_OK_PTR(skel->links.tcp_ts_id_sockops, "attach_cgroup"))
		goto out;

	/* The listener has to be created after the attach to get LISTEN_CB. */
	listen_fd = start_server(AF_INET6, SOCK_STREAM, LO_ADDR6, 0, 0);
	if (!ASSERT_NEQ(listen_fd, -1, "start_server"))
		goto out;

	cli_fd = connect_to_fd(listen_fd, 0);
	if (!ASSERT_NEQ(cli_fd, -1, "connect_to_fd"))
		goto out;

	accept_fd = accept(listen_fd, NULL, NULL);
	if (!ASSERT_NEQ(accept_fd, -1, "accept"))
		goto out;

	ASSERT_EQ(skel->bss->ts_id_cb_count, 1, "cb_count");
	ASSERT_EQ(skel->bss->ts_id_cb_syn, 1, "cb_on_syn");
	ASSERT_EQ(skel->bss->ts_id_cb_bits, EXPECTED_TS_ID_BITS, "cb_bits");

	/* The identifier must reach the child socket created by the SYN. */
	optlen = sizeof(ts_id);
	if (ASSERT_OK(getsockopt(accept_fd, IPPROTO_TCP, TCP_TS_ID, &ts_id,
				 &optlen), "getsockopt(accept_fd, TCP_TS_ID)"))
		ASSERT_EQ(ts_id, decline ? 0 :
			  (EXPECTED_TS_ID_BITS << TCP_TS_ID_BITS_SHIFT) |
			  EXPECTED_TS_ID, "server_ts_id");

	/* Identifiers are passive-open only, so the opener must have none. */
	optlen = sizeof(ts_id);
	if (ASSERT_OK(getsockopt(cli_fd, IPPROTO_TCP, TCP_TS_ID, &ts_id,
				 &optlen), "getsockopt(cli_fd, TCP_TS_ID)"))
		ASSERT_EQ(ts_id, 0, "client_ts_id");
out:
	if (accept_fd != -1)
		close(accept_fd);
	if (cli_fd != -1)
		close(cli_fd);
	if (listen_fd != -1)
		close(listen_fd);
	test_tcp_ts_id_kern__destroy(skel);
}

void test_tcp_ts_id(void)
{
	int cg_fd = -1;
	int err;

	err = save_sysctl(TS_ID_BITS_SYSCTL, saved_ts_id_bits);
	if (err == -ENOENT) {
		test__skip();
		return;
	}
	if (!ASSERT_OK(err, "read " TS_ID_BITS_SYSCTL))
		return;

	if (!ASSERT_OK(save_sysctl(TIMESTAMPS_SYSCTL, saved_timestamps),
		       "read " TIMESTAMPS_SYSCTL))
		goto out;

	/* The callback only runs when timestamps are negotiated. */
	if (!ASSERT_OK(write_sysctl(TIMESTAMPS_SYSCTL, "1"),
		       "write " TIMESTAMPS_SYSCTL))
		goto out;

	/* Identifiers are disabled by default, so the test has to opt in. */
	if (!ASSERT_OK(write_sysctl(TS_ID_BITS_SYSCTL, EXPECTED_TS_ID_BITS_STR),
		       "write " TS_ID_BITS_SYSCTL))
		goto out;

	cg_fd = test__join_cgroup(CG_NAME);
	if (!ASSERT_GE(cg_fd, 0, "test__join_cgroup(" CG_NAME ")"))
		goto out;

	if (test__start_subtest("assign"))
		run_ts_id_test(cg_fd, false);

	/* Only one sockops program may be attached to the cgroup at a time. */
	if (test__start_subtest("decline"))
		run_ts_id_test(cg_fd, true);
out:
	if (cg_fd != -1)
		close(cg_fd);
	restore_sysctl(TIMESTAMPS_SYSCTL, saved_timestamps);
	restore_sysctl(TS_ID_BITS_SYSCTL, saved_ts_id_bits);
}
