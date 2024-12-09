// SPDX-License-Identifier: GPL-2.0
#include <error.h>

#include <test_progs.h>
#include "sockmap_helpers.h"
#include "test_skmsg_load_helpers.skel.h"
#include "test_sockmap_strp.skel.h"
#define STRP_HEAD_LEN 4
#define STRP_BODY_LEN 6
#define STRP_FULL_LEN (STRP_HEAD_LEN + STRP_BODY_LEN)

static void test_sockmap_strp_partial_read(int family, int sotype)
{
	int zero = 0, recvd, off;
	int verdict, parser;
	int err, map;
	int c = -1, p = -1;
	struct test_sockmap_strp *strp = NULL;
	char snd[STRP_FULL_LEN] = "head+body\0";
	char rcv[256] = "0";

	strp = test_sockmap_strp__open_and_load();
	verdict = bpf_program__fd(strp->progs.prog_skb_verdict_pass);
	parser = bpf_program__fd(strp->progs.prog_skb_parser_partial);
	map = bpf_map__fd(strp->maps.sock_map);

	err = bpf_prog_attach(parser, map, BPF_SK_SKB_STREAM_PARSER, 0);
	if (!ASSERT_OK(err, "bpf_prog_attach stream parser"))
		goto out;

	err = bpf_prog_attach(verdict, map, BPF_SK_SKB_STREAM_VERDICT, 0);
	if (!ASSERT_OK(err, "bpf_prog_attach stream verdict"))
		goto out;

	err = create_pair(family, sotype, &c, &p);
	if (err)
		goto out;

	/* sk_data_ready of 'p' will be replaced by strparser handler */
	err = bpf_map_update_elem(map, &zero, &p, BPF_NOEXIST);
	if (!ASSERT_OK(err, "bpf_map_update_elem(zero, p)"))
		goto out_close;

	/* 1.1 send partial head, 1 byte header left*/
	off = STRP_HEAD_LEN - 1;
	xsend(c, snd, off, 0);
	recvd = recv_timeout(p, rcv, sizeof(rcv), MSG_DONTWAIT, 5);
	if (!ASSERT_EQ(-1, recvd, "insufficient head, should no data recvd"))
		goto out_close;

	/* 1.2 send remaining head and body */
	xsend(c, snd + off, STRP_FULL_LEN - off, 0);
	recvd = recv_timeout(p, rcv, sizeof(rcv), MSG_DONTWAIT, IO_TIMEOUT_SEC);
	if (!ASSERT_EQ(recvd, STRP_FULL_LEN, "should full data recvd"))
		goto out_close;

	/* 2.1 send partial head, 1 byte header left */
	off = STRP_HEAD_LEN - 1;
	xsend(c, snd, off, 0);

	/* 2.2 send remaining head and partial body, 1 byte body left */
	xsend(c, snd + off, STRP_FULL_LEN - off - 1, 0);
	off = STRP_FULL_LEN - 1;
	recvd = recv_timeout(p, rcv, sizeof(rcv), MSG_DONTWAIT, 1);
	if (!ASSERT_EQ(-1, recvd, "insufficient body, should no data read"))
		goto out_close;

	/* 2.3 send remain body */
	xsend(c, snd + off, STRP_FULL_LEN - off, 0);
	recvd = recv_timeout(p, rcv, sizeof(rcv), MSG_DONTWAIT, IO_TIMEOUT_SEC);
	if (!ASSERT_EQ(recvd, STRP_FULL_LEN, "should full data recvd"))
		goto out_close;

out_close:
	close(c);
	close(p);

out:
	test_sockmap_strp__destroy(strp);
}

static void test_sockmap_strp_pass(int family, int sotype, bool fionread)
{
	int zero = 0, sent, recvd, avail;
	int verdict, parser;
	int err, map;
	int c = -1, p = -1;
	int read_cnt = 10, i;
	struct test_sockmap_strp *strp = NULL;
	char snd[11] = "0123456789\0";
	char rcv[256] = "0";

	strp = test_sockmap_strp__open_and_load();
	verdict = bpf_program__fd(strp->progs.prog_skb_verdict_pass);
	parser = bpf_program__fd(strp->progs.prog_skb_parser);
	map = bpf_map__fd(strp->maps.sock_map);

	err = bpf_prog_attach(parser, map, BPF_SK_SKB_STREAM_PARSER, 0);
	if (!ASSERT_OK(err, "bpf_prog_attach stream parser"))
		goto out;

	err = bpf_prog_attach(verdict, map, BPF_SK_SKB_STREAM_VERDICT, 0);
	if (!ASSERT_OK(err, "bpf_prog_attach stream verdict"))
		goto out;

	err = create_pair(family, sotype, &c, &p);
	if (err)
		goto out;

	/* sk_data_ready of 'p' will be replaced by strparser handler */
	err = bpf_map_update_elem(map, &zero, &p, BPF_NOEXIST);
	if (!ASSERT_OK(err, "bpf_map_update_elem(p)"))
		goto out_close;

	/*
	 * Previously, we encountered issues such as deadlocks and
	 * sequence errors that resulted in the inability to read
	 * continuously. Therefore, we perform multiple iterations
	 * of testing here.
	 */
	for (i = 0; i < read_cnt; i++) {
		sent = xsend(c, snd, sizeof(snd), 0);
		if (!ASSERT_EQ(sent, sizeof(snd), "xsend(c)"))
			goto out_close;

		recvd = recv_timeout(p, rcv, sizeof(rcv), MSG_DONTWAIT,
				     IO_TIMEOUT_SEC);
		if (!ASSERT_EQ(recvd, sizeof(snd), "recv_timeout(p)")
		    || !ASSERT_OK(memcmp(snd, rcv, sizeof(snd)),
				  "recv_timeout(p)"))
			goto out_close;
	}

	if (fionread) {
		sent = xsend(c, snd, sizeof(snd), 0);
		if (!ASSERT_EQ(sent, sizeof(snd), "second xsend(c)"))
			goto out_close;

		err = ioctl(p, FIONREAD, &avail);
		if (!ASSERT_OK(err, "ioctl(FIONREAD) error")
		    || ASSERT_EQ(avail, sizeof(snd), "ioctl(FIONREAD)"))
			goto out_close;

		recvd = recv_timeout(p, rcv, sizeof(rcv), MSG_DONTWAIT,
							 IO_TIMEOUT_SEC);
		if (!ASSERT_EQ(recvd, sizeof(snd), "second recv_timeout(p)")
		    || ASSERT_OK(memcmp(snd, rcv, sizeof(snd)),
				 "second recv_timeout(p)"))
			goto out_close;
	}

out_close:
	close(c);
	close(p);

out:
	test_sockmap_strp__destroy(strp);
}

static void test_sockmap_strp_verdict(int family, int sotype)
{
	int zero = 0, one = 1, sent, recvd, off, total_sent;
	int verdict, parser;
	int err, map;
	int c0 = -1, p0 = -1, c1 = -1, p1 = -1;
	struct test_sockmap_strp *strp = NULL;
	char snd[11] = "0123456789\0";
	char rcv[256] = "0";

	strp = test_sockmap_strp__open_and_load();
	verdict = bpf_program__fd(strp->progs.prog_skb_verdict);
	parser = bpf_program__fd(strp->progs.prog_skb_parser);
	map = bpf_map__fd(strp->maps.sock_map);

	err = bpf_prog_attach(parser, map, BPF_SK_SKB_STREAM_PARSER, 0);
	if (!ASSERT_OK(err, "bpf_prog_attach stream parser"))
		goto out;

	err = bpf_prog_attach(verdict, map, BPF_SK_SKB_STREAM_VERDICT, 0);
	if (!ASSERT_OK(err, "bpf_prog_attach stream verdict"))
		goto out;

	/* We simulate a reverse proxy server.
	 * When p0 receives data from c0, we forward it to p1.
	 * From p1's perspective, it will consider this data
	 * as being sent by c1.
	 */
	err = create_socket_pairs(family, sotype, &c0, &c1, &p0, &p1);
	if (!ASSERT_OK(err, "create_socket_pairs()"))
		goto out;

	err = bpf_map_update_elem(map, &zero, &p0, BPF_NOEXIST);
	if (!ASSERT_OK(err, "bpf_map_update_elem(p0)"))
		goto out_close;

	err = bpf_map_update_elem(map, &one, &c1, BPF_NOEXIST);
	if (!ASSERT_OK(err, "bpf_map_update_elem(c1)"))
		goto out_close;

	total_sent = sizeof(snd);
	sent = xsend(c0, snd, total_sent, 0);
	if (!ASSERT_EQ(sent, total_sent, "xsend(c0)"))
		goto out_close;

	recvd = recv_timeout(p1, rcv, sizeof(rcv), MSG_DONTWAIT,
			     IO_TIMEOUT_SEC);
	if (!ASSERT_EQ(recvd, total_sent, "recv_timeout(p1)")
	    || !ASSERT_OK(memcmp(snd, rcv, total_sent),
			  "received data does not match the sent data"))
		goto out_close;

	/* send again to ensure the stream is functioning correctly. */
	total_sent = sizeof(snd);
	sent = xsend(c0, snd, total_sent, 0);
	if (!ASSERT_EQ(sent, total_sent, "second xsend(c0)"))
		goto out_close;

	/* partial read */
	off = total_sent/2;
	recvd = recv_timeout(p1, rcv, off, MSG_DONTWAIT,
			     IO_TIMEOUT_SEC);
	recvd += recv_timeout(p1, rcv + off, sizeof(rcv) - off, MSG_DONTWAIT,
			      IO_TIMEOUT_SEC);

	if (!ASSERT_EQ(recvd, total_sent, "partial recv_timeout(p1)")
	    || !ASSERT_OK(memcmp(snd, rcv, total_sent),
			  "partial received data does not match the sent data"))
		goto out_close;

out_close:
	close(c0);
	close(c1);
	close(p0);
	close(p1);
out:
	test_sockmap_strp__destroy(strp);
}

void test_sockmap_strp(void)
{
	if (test__start_subtest("sockmap strp tcp pass"))
		test_sockmap_strp_pass(AF_INET, SOCK_STREAM, false);
	if (test__start_subtest("sockmap strp tcp v6 pass"))
		test_sockmap_strp_pass(AF_INET6, SOCK_STREAM, false);
	if (test__start_subtest("sockmap strp tcp pass fionread"))
		test_sockmap_strp_pass(AF_INET, SOCK_STREAM, true);
	if (test__start_subtest("sockmap strp tcp v6 pass fionread"))
		test_sockmap_strp_pass(AF_INET6, SOCK_STREAM, true);
	if (test__start_subtest("sockmap strp tcp verdict"))
		test_sockmap_strp_verdict(AF_INET, SOCK_STREAM);
	if (test__start_subtest("sockmap strp tcp v6 verdict"))
		test_sockmap_strp_verdict(AF_INET6, SOCK_STREAM);
	if (test__start_subtest("sockmap strp tcp partial read"))
		test_sockmap_strp_partial_read(AF_INET, SOCK_STREAM);
}
