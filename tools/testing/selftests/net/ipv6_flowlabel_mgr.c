// SPDX-License-Identifier: GPL-2.0
/* Test IPV6_FLOWINFO_MGR */

#define _GNU_SOURCE

#include <arpa/inet.h>
#include <error.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/in6.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include "kselftest_harness.h"

/* uapi/glibc weirdness may leave this undefined */
#ifndef IPV6_FLOWLABEL_MGR
#define IPV6_FLOWLABEL_MGR	32
#endif
#ifndef IPV6_FLOWINFO_SEND
#define IPV6_FLOWINFO_SEND	33
#endif

/* from net/ipv6/ip6_flowlabel.c */
#define FL_MIN_LINGER		6

#define INIT_SIN6_LOOPBACK(name)					\
	struct sockaddr_in6 name = {					\
		.sin6_family	= AF_INET6,				\
		.sin6_addr	= IN6ADDR_LOOPBACK_INIT,		\
		.sin6_port	= htons(8888),				\
	}

static int flowlabel_get(int fd, uint32_t label, uint8_t share, uint16_t flags)
{
	struct in6_flowlabel_req req = {
		.flr_action = IPV6_FL_A_GET,
		.flr_label = htonl(label),
		.flr_flags = flags,
		.flr_share = share,
	};

	/* do not pass IPV6_ADDR_ANY or IPV6_ADDR_MAPPED */
	req.flr_dst.s6_addr[0] = 0xfd;
	req.flr_dst.s6_addr[15] = 0x1;

	return setsockopt(fd, SOL_IPV6, IPV6_FLOWLABEL_MGR, &req, sizeof(req));
}

static int flowlabel_put(int fd, uint32_t label)
{
	struct in6_flowlabel_req req = {
		.flr_action = IPV6_FL_A_PUT,
		.flr_label = htonl(label),
	};

	return setsockopt(fd, SOL_IPV6, IPV6_FLOWLABEL_MGR, &req, sizeof(req));
}

static int flowlabel_renew(int fd, uint32_t label, uint16_t linger)
{
	struct in6_flowlabel_req req = {
		.flr_action = IPV6_FL_A_RENEW,
		.flr_label = htonl(label),
		.flr_linger = linger,
	};

	return setsockopt(fd, SOL_IPV6, IPV6_FLOWLABEL_MGR, &req, sizeof(req));
}

static int tcp_listen(void)
{
	INIT_SIN6_LOOPBACK(addr);
	const int one = 1;
	int fd;

	fd = socket(PF_INET6, SOCK_STREAM, 0);
	if (fd == -1)
		error(1, errno, "socket listener");
	if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)))
		error(1, errno, "setsockopt SO_REUSEADDR");
	if (bind(fd, (void *)&addr, sizeof(addr)))
		error(1, errno, "bind");
	if (listen(fd, 1))
		error(1, errno, "listen");

	return fd;
}

static void tcp_connect(int listener, uint32_t flowlabel, int *client, int *accepted)
{
	INIT_SIN6_LOOPBACK(addr);
	const int one = 1;
	int cfd, afd;

	cfd = socket(PF_INET6, SOCK_STREAM, 0);
	if (cfd == -1)
		error(1, errno, "socket client");

	if (flowlabel_get(cfd, flowlabel, IPV6_FL_S_EXCL, IPV6_FL_F_CREATE))
		error(1, errno, "flowlabel_get");
	if (setsockopt(cfd, SOL_IPV6, IPV6_FLOWINFO_SEND, &one, sizeof(one)))
		error(1, errno, "setsockopt flowinfo_send");
	addr.sin6_flowinfo = htonl(flowlabel);

	if (connect(cfd, (void *)&addr, sizeof(addr)))
		error(1, errno, "connect");

	afd = accept(listener, NULL, NULL);
	if (afd == -1)
		error(1, errno, "accept");

	*client = cfd;
	*accepted = afd;
}

static void set_flowlabel_consistency(bool enable)
{
	/* flowlabel_consistency must be disable to use the IPV6_FL_F_REFLECT
	 * flag. This helper function is required for setting up and tearing
	 * down test cases for this flag.
	 */
	int fd;

	fd = open("/proc/sys/net/ipv6/flowlabel_consistency", O_WRONLY);
	if (fd == -1)
		error(1, errno, "open flowlabel_consistency");
	if (write(fd, enable ? "1" : "0", 1) != 1)
		error(1, errno, "write flowlabel_consistency");
	if (close(fd))
		error(1, errno, "close flowlabel_consistency");
}

TEST(cannot_get_non_existent_label)
{
	int fd, err;

	fd = socket(PF_INET6, SOCK_DGRAM, 0);
	ASSERT_GE(fd, 0) TH_LOG("socket failed");

	err = flowlabel_get(fd, 1, IPV6_FL_S_ANY, 0);
	ASSERT_TRUE(err) TH_LOG("expected get of a non-existent label to fail");

	ASSERT_EQ(0, close(fd));
}

TEST(cannot_put_non_existent_label)
{
	int fd, err;

	fd = socket(PF_INET6, SOCK_DGRAM, 0);
	ASSERT_GE(fd, 0) TH_LOG("socket failed");

	err = flowlabel_put(fd, 1);
	ASSERT_TRUE(err) TH_LOG("expected put of a non-existent label to fail");

	ASSERT_EQ(0, close(fd));
}

TEST(cannot_create_label_greater_than_20_bits)
{
	int fd, err;

	fd = socket(PF_INET6, SOCK_DGRAM, 0);
	ASSERT_GE(fd, 0) TH_LOG("socket failed");

	err = flowlabel_get(fd, 0x1FFFFF, IPV6_FL_S_ANY, IPV6_FL_F_CREATE);
	ASSERT_TRUE(err) TH_LOG("expected label > 20 bits to be rejected");

	ASSERT_EQ(0, close(fd));
}

TEST(can_create_and_get_and_put_labels)
{
	int fd, err;

	fd = socket(PF_INET6, SOCK_DGRAM, 0);
	ASSERT_GE(fd, 0) TH_LOG("socket failed");

	err = flowlabel_get(fd, 1, IPV6_FL_S_ANY, IPV6_FL_F_CREATE);
	ASSERT_TRUE(!err) TH_LOG("failed to create label (FL_F_CREATE)");

	err = flowlabel_get(fd, 1, IPV6_FL_S_ANY, 0);
	ASSERT_TRUE(!err) TH_LOG("failed to get the label without FL_F_CREATE");

	err = flowlabel_get(fd, 1, IPV6_FL_S_ANY, IPV6_FL_F_CREATE);
	ASSERT_TRUE(!err) TH_LOG("failed to get it again with create flag set, too");

	err = flowlabel_get(fd, 1, IPV6_FL_S_ANY, IPV6_FL_F_CREATE | IPV6_FL_F_EXCL);
	ASSERT_TRUE(err) TH_LOG("expected FL_F_EXCL to reject an already-existing label");

	err = flowlabel_put(fd, 1);
	ASSERT_TRUE(!err) TH_LOG("failed to put first reference");
	err = flowlabel_put(fd, 1);
	ASSERT_TRUE(!err) TH_LOG("failed to put second reference");
	err = flowlabel_put(fd, 1);
	ASSERT_TRUE(!err) TH_LOG("failed to put third reference");
	err = flowlabel_put(fd, 1);
	ASSERT_TRUE(err) TH_LOG("expected fourth put to fail, no references left");

	ASSERT_EQ(0, close(fd));
}

TEST(exclusive_label_share)
{
	int fd, err;

	fd = socket(PF_INET6, SOCK_DGRAM, 0);
	ASSERT_GE(fd, 0) TH_LOG("socket failed");

	err = flowlabel_get(fd, 2, IPV6_FL_S_EXCL, IPV6_FL_F_CREATE);
	ASSERT_TRUE(!err) TH_LOG("failed to create a new exclusive label (FL_S_EXCL)");

	err = flowlabel_get(fd, 2, IPV6_FL_S_ANY, IPV6_FL_F_CREATE);
	ASSERT_TRUE(err) TH_LOG("expected reuse in non-exclusive mode to fail");

	err = flowlabel_get(fd, 2, IPV6_FL_S_EXCL, IPV6_FL_F_CREATE);
	ASSERT_TRUE(err) TH_LOG("expected reuse in exclusive mode to fail too");

	err = flowlabel_put(fd, 2);
	ASSERT_TRUE(!err) TH_LOG("failed to put the exclusive label");

	err = flowlabel_get(fd, 2, IPV6_FL_S_ANY, IPV6_FL_F_CREATE);
	ASSERT_TRUE(err) TH_LOG("expected reuse to fail, due to linger");

	sleep(FL_MIN_LINGER * 2 + 1);

	err = flowlabel_get(fd, 2, IPV6_FL_S_ANY, IPV6_FL_F_CREATE);
	ASSERT_TRUE(!err) TH_LOG("expected reuse to succeed after linger");

	ASSERT_EQ(0, close(fd));
}

TEST(user_private_label_share)
{
	int fd, err, wstatus;
	pid_t pid;

	fd = socket(PF_INET6, SOCK_DGRAM, 0);
	ASSERT_GE(fd, 0) TH_LOG("socket failed");

	err = flowlabel_get(fd, 3, IPV6_FL_S_USER, IPV6_FL_F_CREATE);
	ASSERT_TRUE(!err) TH_LOG("failed to create a new user-private label (FL_S_USER)");

	err = flowlabel_get(fd, 3, IPV6_FL_S_ANY, 0);
	ASSERT_TRUE(err) TH_LOG("expected get in non-exclusive mode to fail");

	err = flowlabel_get(fd, 3, IPV6_FL_S_EXCL, 0);
	ASSERT_TRUE(err) TH_LOG("expected get in exclusive mode to fail");

	err = flowlabel_get(fd, 3, IPV6_FL_S_USER, 0);
	ASSERT_TRUE(!err) TH_LOG("failed to get it again in user mode");

	pid = fork();
	ASSERT_NE(-1, pid) TH_LOG("fork failed");
	if (!pid) {
		if (flowlabel_get(fd, 3, IPV6_FL_S_USER, 0))
			exit(1);
		if (setuid(USHRT_MAX)) {
			fprintf(stderr, "[INFO] skip setuid child test\n");
			exit(0);
		}
		if (!flowlabel_get(fd, 3, IPV6_FL_S_USER, 0))
			exit(1);
		exit(0);
	}
	ASSERT_EQ(pid, wait(&wstatus)) TH_LOG("wait failed");
	ASSERT_TRUE(WIFEXITED(wstatus)) TH_LOG("child did not exit normally");
	ASSERT_EQ(0, WEXITSTATUS(wstatus)) TH_LOG("child reported unexpected result");

	ASSERT_EQ(0, close(fd));
}

TEST(process_private_label_share)
{
	int fd, err, wstatus;
	pid_t pid;

	fd = socket(PF_INET6, SOCK_DGRAM, 0);
	ASSERT_GE(fd, 0) TH_LOG("socket failed");

	err = flowlabel_get(fd, 4, IPV6_FL_S_PROCESS, IPV6_FL_F_CREATE);
	ASSERT_TRUE(!err) TH_LOG("failed to create a new process-private label (FL_S_PROCESS)");

	err = flowlabel_get(fd, 4, IPV6_FL_S_PROCESS, 0);
	ASSERT_TRUE(!err) TH_LOG("failed to get it again");

	pid = fork();
	ASSERT_NE(-1, pid) TH_LOG("fork failed");
	if (!pid) {
		if (!flowlabel_get(fd, 4, IPV6_FL_S_PROCESS, 0))
			exit(1);
		exit(0);
	}
	ASSERT_EQ(pid, wait(&wstatus)) TH_LOG("wait failed");
	ASSERT_TRUE(WIFEXITED(wstatus)) TH_LOG("child did not exit normally");
	ASSERT_EQ(0, WEXITSTATUS(wstatus)) TH_LOG("child reported unexpected result");

	ASSERT_EQ(0, close(fd));
}

TEST(renew_label_linger)
{
	/* After a label with EXCL share is put and lingered, it must be
	 * possible to create a new one. Check if RENEW action extends
	 * the linger period of put label, blocking creation after previous
	 * linger time.
	 */
	int fd, err;

	fd = socket(PF_INET6, SOCK_DGRAM, 0);
	ASSERT_GE(fd, 0) TH_LOG("socket failed");

	err = flowlabel_get(fd, 5, IPV6_FL_S_EXCL, IPV6_FL_F_CREATE);
	ASSERT_TRUE(!err) TH_LOG("failed to create a new label with FL_MIN_LINGER linger time");

	err = flowlabel_renew(fd, 5, 2 * (FL_MIN_LINGER * 2 + 1));
	ASSERT_TRUE(!err) TH_LOG("failed to renew the label to increase its linger time");

	err = flowlabel_put(fd, 5);
	ASSERT_TRUE(!err) TH_LOG("failed to put the label");

	sleep(FL_MIN_LINGER * 2 + 1);

	err = flowlabel_get(fd, 5, IPV6_FL_S_ANY, IPV6_FL_F_CREATE);
	ASSERT_TRUE(err) TH_LOG("expected reuse to fail, new linger time not over yet");

	ASSERT_EQ(0, close(fd));
}

TEST(remote_flag)
{
	/* The REMOTE flag, used for getsockopt, is expected to retrieve the
	 * label from the latest received header.
	 */
	struct in6_flowlabel_req freq = {
		.flr_action = IPV6_FL_A_GET,
		.flr_flags = IPV6_FL_F_REMOTE,
	};
	socklen_t freq_len = sizeof(freq);
	int listener, cfd, afd, err;

	listener = tcp_listen();
	tcp_connect(listener, 6, &cfd, &afd);

	err = getsockopt(afd, SOL_IPV6, IPV6_FLOWLABEL_MGR, &freq, &freq_len);
	ASSERT_TRUE(!err) TH_LOG("getsockopt with IPV6_FL_F_REMOTE failed");
	ASSERT_EQ(6, ntohl(freq.flr_label)) TH_LOG("unexpected remote flow label");

	ASSERT_EQ(0, close(afd));
	ASSERT_EQ(0, close(cfd));
	ASSERT_EQ(0, close(listener));
}

TEST(reflect_flag)
{
	/* The REFLECT flag acts as a trigger to the REPFLOW bit. When REPFLOW
	 * is triggered for a socket, it adopts the label received from the
	 * connected socket.
	 */
	struct in6_flowlabel_req reflect_on = {
		.flr_action = IPV6_FL_A_GET,
		.flr_flags = IPV6_FL_F_REFLECT,
	};
	struct in6_flowlabel_req reflect_query = {
		.flr_action = IPV6_FL_A_GET,
	};
	struct in6_flowlabel_req reflect_off = {
		.flr_action = IPV6_FL_A_PUT,
		.flr_flags = IPV6_FL_F_REFLECT,
	};
	socklen_t reflect_query_len = sizeof(reflect_query);
	int listener, cfd, afd, err;

	set_flowlabel_consistency(false);

	listener = tcp_listen();
	err = setsockopt(listener, SOL_IPV6, IPV6_FLOWLABEL_MGR, &reflect_on, sizeof(reflect_on));
	ASSERT_TRUE(!err) TH_LOG("failed to enable REFLECT on the listener");

	tcp_connect(listener, 7, &cfd, &afd);

	err = getsockopt(afd, SOL_IPV6, IPV6_FLOWLABEL_MGR, &reflect_query, &reflect_query_len);
	ASSERT_TRUE(!err) TH_LOG("failed to query the accepted socket's outgoing label");
	ASSERT_EQ(7, ntohl(reflect_query.flr_label)) TH_LOG("accepted socket did not reflect client's label");

	err = setsockopt(afd, SOL_IPV6, IPV6_FLOWLABEL_MGR, &reflect_off, sizeof(reflect_off));
	ASSERT_TRUE(!err) TH_LOG("failed to disable REFLECT on the accepted socket");

	err = setsockopt(afd, SOL_IPV6, IPV6_FLOWLABEL_MGR, &reflect_off, sizeof(reflect_off));
	ASSERT_TRUE(err) TH_LOG("expected disabling REFLECT twice to fail");

	set_flowlabel_consistency(true);

	ASSERT_EQ(0, close(afd));
	ASSERT_EQ(0, close(cfd));
	ASSERT_EQ(0, close(listener));
}

TEST_HARNESS_MAIN
