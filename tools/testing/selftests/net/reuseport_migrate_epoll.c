// SPDX-License-Identifier: GPL-2.0

#define _GNU_SOURCE

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sched.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "../kselftest.h"

#define EPOLL_TIMEOUT_MS 500
#define TCP_MIGRATE_REQ_PATH "/proc/sys/net/ipv4/tcp_migrate_req"

struct reuseport_migrate_case {
	const char *name;
	int family;
	const char *addr;
};

static const struct reuseport_migrate_case test_cases[] = {
	{
		.name = "ipv4 epoll wake after reuseport migration",
		.family = AF_INET,
		.addr = "127.0.0.1",
	},
	{
		.name = "ipv6 epoll wake after reuseport migration",
		.family = AF_INET6,
		.addr = "::1",
	},
};

static void close_fd(int *fd)
{
	if (*fd >= 0) {
		close(*fd);
		*fd = -1;
	}
}

static bool unsupported_addr_err(int family, int err)
{
	return family == AF_INET6 &&
		(err == EAFNOSUPPORT ||
		 err == EPROTONOSUPPORT ||
		 err == EADDRNOTAVAIL);
}

static int make_sockaddr(const struct reuseport_migrate_case *test_case,
			 unsigned short port,
			 struct sockaddr_storage *addr,
			 socklen_t *addrlen)
{
	memset(addr, 0, sizeof(*addr));

	if (test_case->family == AF_INET) {
		struct sockaddr_in *addr4 = (struct sockaddr_in *)addr;

		addr4->sin_family = AF_INET;
		addr4->sin_port = htons(port);
		if (inet_pton(AF_INET, test_case->addr, &addr4->sin_addr) != 1)
			return -1;

		*addrlen = sizeof(*addr4);
		return 0;
	}

	if (test_case->family == AF_INET6) {
		struct sockaddr_in6 *addr6 = (struct sockaddr_in6 *)addr;

		addr6->sin6_family = AF_INET6;
		addr6->sin6_port = htons(port);
		if (inet_pton(AF_INET6, test_case->addr, &addr6->sin6_addr) != 1)
			return -1;

		*addrlen = sizeof(*addr6);
		return 0;
	}

	return -1;
}

static int create_reuseport_socket(const struct reuseport_migrate_case *test_case)
{
	int one = 1;
	int fd;

	fd = socket(test_case->family, SOCK_STREAM | SOCK_CLOEXEC, IPPROTO_TCP);
	if (fd < 0)
		return -1;

	if (test_case->family == AF_INET6 &&
	    setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &one, sizeof(one))) {
		close(fd);
		return -1;
	}

	if (setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one))) {
		close(fd);
		return -1;
	}

	return fd;
}

static int set_nonblocking(int fd)
{
	int flags;

	flags = fcntl(fd, F_GETFL);
	if (flags < 0)
		return -1;

	return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int enable_tcp_migrate_req(void)
{
	int len;
	int fd;

	fd = open(TCP_MIGRATE_REQ_PATH, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		if (errno == ENOENT || errno == EACCES ||
		    errno == EPERM || errno == EROFS)
			return KSFT_SKIP;
		return KSFT_FAIL;
	}

	len = write(fd, "1", 1);
	if (len != 1) {
		if (errno == EACCES || errno == EPERM || errno == EROFS) {
			close(fd);
			return KSFT_SKIP;
		}

		close(fd);
		return KSFT_FAIL;
	}

	close(fd);
	return KSFT_PASS;
}

static void setup_netns(void)
{
	int ret;

	if (unshare(CLONE_NEWNET))
		ksft_exit_skip("unshare(CLONE_NEWNET): %s\n", strerror(errno));

	if (system("ip link set lo up"))
		ksft_exit_skip("failed to bring up lo interface in netns\n");

	ret = enable_tcp_migrate_req();
	if (ret == KSFT_SKIP)
		ksft_exit_skip("failed to enable tcp_migrate_req\n");
	if (ret == KSFT_FAIL)
		ksft_exit_fail_msg("failed to enable tcp_migrate_req\n");
}

static int run_test(const struct reuseport_migrate_case *test_case)
{
	struct sockaddr_storage addr;
	struct epoll_event ev = {
		.events = EPOLLIN,
	};
	int listener_a = -1;
	int listener_b = -1;
	int ret = KSFT_FAIL;
	socklen_t addrlen;
	int accepted = -1;
	int client = -1;
	int epfd = -1;
	int n;

	if (make_sockaddr(test_case, 0, &addr, &addrlen)) {
		ksft_print_msg("%s: failed to build socket address\n",
			       test_case->name);
		goto out;
	}

	listener_a = create_reuseport_socket(test_case);
	if (listener_a < 0) {
		if (unsupported_addr_err(test_case->family, errno)) {
			ret = KSFT_SKIP;
			goto out;
		}

		ksft_perror("socket(listener_a)");
		goto out;
	}

	if (bind(listener_a, (struct sockaddr *)&addr, addrlen)) {
		if (unsupported_addr_err(test_case->family, errno)) {
			ret = KSFT_SKIP;
			goto out;
		}

		ksft_perror("bind(listener_a)");
		goto out;
	}

	if (listen(listener_a, 1)) {
		ksft_perror("listen(listener_a)");
		goto out;
	}

	addrlen = sizeof(addr);
	if (getsockname(listener_a, (struct sockaddr *)&addr, &addrlen)) {
		ksft_perror("getsockname(listener_a)");
		goto out;
	}

	listener_b = create_reuseport_socket(test_case);
	if (listener_b < 0) {
		if (unsupported_addr_err(test_case->family, errno)) {
			ret = KSFT_SKIP;
			goto out;
		}

		ksft_perror("socket(listener_b)");
		goto out;
	}

	if (bind(listener_b, (struct sockaddr *)&addr, addrlen)) {
		ksft_perror("bind(listener_b)");
		goto out;
	}

	client = socket(test_case->family, SOCK_STREAM | SOCK_CLOEXEC, IPPROTO_TCP);
	if (client < 0) {
		if (unsupported_addr_err(test_case->family, errno)) {
			ret = KSFT_SKIP;
			goto out;
		}

		ksft_perror("socket(client)");
		goto out;
	}

	/* Connect while only listener_a is listening, ensuring the
	 * child lands in listener_a's accept queue deterministically.
	 */
	if (connect(client, (struct sockaddr *)&addr, addrlen)) {
		if (unsupported_addr_err(test_case->family, errno)) {
			ret = KSFT_SKIP;
			goto out;
		}

		ksft_perror("connect(client)");
		goto out;
	}

	if (listen(listener_b, 1)) {
		ksft_perror("listen(listener_b)");
		goto out;
	}

	if (set_nonblocking(listener_b)) {
		ksft_perror("set_nonblocking(listener_b)");
		goto out;
	}

	epfd = epoll_create1(EPOLL_CLOEXEC);
	if (epfd < 0) {
		ksft_perror("epoll_create1");
		goto out;
	}

	ev.data.fd = listener_b;
	if (epoll_ctl(epfd, EPOLL_CTL_ADD, listener_b, &ev)) {
		ksft_perror("epoll_ctl(ADD listener_b)");
		goto out;
	}

	close_fd(&listener_a);

	n = epoll_wait(epfd, &ev, 1, EPOLL_TIMEOUT_MS);
	if (n < 0) {
		ksft_perror("epoll_wait");
		goto out;
	}

	accepted = accept4(listener_b, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
	if (accepted < 0) {
		if (errno == EAGAIN || errno == EWOULDBLOCK) {
			ksft_print_msg("%s: target listener had no queued child after migration\n",
				       test_case->name);
			goto out;
		}

		ksft_perror("accept4(listener_b)");
		goto out;
	}

	if (n != 1) {
		ksft_print_msg("%s: accept queue was populated, but epoll_wait() timed out\n",
			       test_case->name);
		goto out;
	}

	if (ev.data.fd != listener_b || !(ev.events & EPOLLIN)) {
		ksft_print_msg("%s: unexpected epoll event fd=%d events=%#x\n",
			       test_case->name, ev.data.fd, ev.events);
		goto out;
	}

	ret = KSFT_PASS;

out:
	close_fd(&accepted);
	close_fd(&epfd);
	close_fd(&client);
	close_fd(&listener_b);
	close_fd(&listener_a);

	return ret;
}

int main(void)
{
	int status = KSFT_PASS;
	int ret;
	int i;

	setup_netns();

	ksft_print_header();
	ksft_set_plan(ARRAY_SIZE(test_cases));

	for (i = 0; i < ARRAY_SIZE(test_cases); i++) {
		ret = run_test(&test_cases[i]);
		ksft_test_result_code(ret, test_cases[i].name, NULL);

		if (ret == KSFT_FAIL)
			status = KSFT_FAIL;
	}

	if (status == KSFT_FAIL)
		ksft_exit_fail();

	ksft_finished();
}
