// SPDX-License-Identifier: GPL-2.0

#define _GNU_SOURCE

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include "../kselftest.h"

#define ACCEPT_BLOCK_TIMEOUT_MS 1000
#define ACCEPT_CLEANUP_TIMEOUT_MS 1000
#define ACCEPT_WAKE_TIMEOUT_MS 2000
#define TCP_MIGRATE_REQ_PATH "/proc/sys/net/ipv4/tcp_migrate_req"

struct reuseport_migrate_case {
	const char *name;
	int family;
	const char *addr;
};

struct accept_result {
	int listener_fd;
	atomic_int started;
	atomic_int tid;
	int accepted_fd;
	int err;
};

static const struct reuseport_migrate_case test_cases[] = {
	{
		.name = "ipv4 blocking accept wake after reuseport migration",
		.family = AF_INET,
		.addr = "127.0.0.1",
	},
	{
		.name = "ipv6 blocking accept wake after reuseport migration",
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

static void noop_handler(int sig)
{
	(void)sig;
}

static void *accept_thread(void *arg)
{
	struct accept_result *result = arg;

	atomic_store_explicit(&result->tid, (int)syscall(SYS_gettid),
			      memory_order_release);
	atomic_store_explicit(&result->started, 1, memory_order_release);
	result->accepted_fd = accept4(result->listener_fd, NULL, NULL,
				      SOCK_CLOEXEC);
	if (result->accepted_fd < 0)
		result->err = errno;

	return NULL;
}

static int read_thread_state(int tid, char *state)
{
	char *close_paren;
	char path[64];
	char buf[256];
	ssize_t len;
	int fd;

	snprintf(path, sizeof(path), "/proc/self/task/%d/stat", tid);

	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -errno;

	len = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (len < 0)
		return -errno;
	if (!len)
		return -EINVAL;

	buf[len] = '\0';
	close_paren = strrchr(buf, ')');
	if (!close_paren || close_paren[1] != ' ' || !close_paren[2])
		return -EINVAL;

	*state = close_paren[2];
	return 0;
}

static int wait_for_accept_to_block(const struct reuseport_migrate_case *test_case,
				    int tid)
{
	char state = '\0';
	int ret;
	int i;

	/*
	 * A started thread is not enough here: we need to know the waiter
	 * has actually gone to sleep in accept() before closing listener_a,
	 * otherwise migration can race ahead of waiter registration. Poll
	 * /proc task state because the pthread APIs can tell us whether the
	 * thread has exited, but not whether it is already blocked in the
	 * target syscall.
	 */
	for (i = 0; i < ACCEPT_BLOCK_TIMEOUT_MS; i++) {
		ret = read_thread_state(tid, &state);
		if (!ret) {
			if (state == 'S' || state == 'D')
				return KSFT_PASS;
			if (state == 'Z')
				break;
		} else if (ret == -ENOENT) {
			break;
		}

		usleep(1000);
	}

	ksft_print_msg("%s: accept waiter never blocked before migration\n",
		       test_case->name);
	return KSFT_FAIL;
}

static int join_thread_with_timeout(pthread_t thread, int timeout_ms,
				    bool *timed_out)
{
	struct timespec deadline;
	int err;

	*timed_out = false;

	if (clock_gettime(CLOCK_REALTIME, &deadline))
		return KSFT_FAIL;

	deadline.tv_nsec += timeout_ms * 1000000LL;
	deadline.tv_sec += deadline.tv_nsec / 1000000000LL;
	deadline.tv_nsec %= 1000000000LL;

	err = pthread_timedjoin_np(thread, NULL, &deadline);
	if (!err)
		return KSFT_PASS;

	if (err != ETIMEDOUT)
		return KSFT_FAIL;

	*timed_out = true;
	return KSFT_FAIL;
}

static int interrupt_accept_thread(pthread_t thread)
{
	int err;

	err = pthread_kill(thread, SIGUSR1);
	if (err && err != ESRCH)
		return KSFT_FAIL;

	return KSFT_PASS;
}

static int stop_accept_thread(pthread_t thread, bool *timed_out)
{
	if (interrupt_accept_thread(thread))
		return KSFT_FAIL;

	return join_thread_with_timeout(thread, ACCEPT_CLEANUP_TIMEOUT_MS,
					timed_out);
}

static int run_test(const struct reuseport_migrate_case *test_case)
{
	struct accept_result result = {
		.listener_fd = -1,
		.started = 0,
		.tid = -1,
		.accepted_fd = -1,
		.err = 0,
	};
	struct sockaddr_storage addr;
	struct sigaction sa = {
		.sa_handler = noop_handler,
	};
	bool thread_joined = false;
	bool cleanup_timed_out;
	int listener_a = -1;
	int listener_b = -1;
	int ret = KSFT_FAIL;
	socklen_t addrlen;
	pthread_t thread;
	int client = -1;
	bool timed_out;
	int probe = -1;
	int tid;

	if (make_sockaddr(test_case, 0, &addr, &addrlen)) {
		ksft_print_msg("%s: failed to build socket address\n",
			       test_case->name);
		goto out;
	}

	if (sigemptyset(&sa.sa_mask)) {
		ksft_perror("sigemptyset");
		goto out;
	}

	if (sigaction(SIGUSR1, &sa, NULL)) {
		ksft_perror("sigaction(SIGUSR1)");
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

	result.listener_fd = listener_b;
	if (pthread_create(&thread, NULL, accept_thread, &result)) {
		ksft_perror("pthread_create");
		goto out;
	}

	while (!atomic_load_explicit(&result.started, memory_order_acquire))
		sched_yield();

	tid = atomic_load_explicit(&result.tid, memory_order_acquire);
	if (wait_for_accept_to_block(test_case, tid))
		goto out_with_thread;

	close_fd(&listener_a);

	ret = join_thread_with_timeout(thread, ACCEPT_WAKE_TIMEOUT_MS, &timed_out);
	if (ret == KSFT_PASS) {
		thread_joined = true;
		if (result.accepted_fd < 0) {
			ksft_print_msg("%s: blocking accept() returned err=%d (%s)\n",
				       test_case->name, result.err,
				       strerror(result.err));
			ret = KSFT_FAIL;
		}

		goto out_with_thread;
	}

	if (!timed_out) {
		ksft_print_msg("%s: join_thread_with_timeout() failed\n",
			       test_case->name);
		goto out_with_thread;
	}

	if (stop_accept_thread(thread, &cleanup_timed_out) == KSFT_FAIL) {
		ksft_print_msg("%s: failed to stop blocking accept waiter\n",
			       test_case->name);
		goto out_with_thread;
	}
	thread_joined = true;

	if (result.accepted_fd >= 0) {
		ksft_print_msg("%s: blocking accept() completed only in cleanup\n",
			       test_case->name);
		goto out_with_thread;
	}

	if (result.err != EINTR) {
		ksft_print_msg("%s: blocking accept() returned err=%d (%s)\n",
			       test_case->name, result.err,
			       strerror(result.err));
		goto out_with_thread;
	}

	probe = accept4(listener_b, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
	if (probe >= 0) {
		ksft_print_msg("%s: accept queue was populated, but blocking accept() timed out\n",
			       test_case->name);
	} else if (errno == EAGAIN || errno == EWOULDBLOCK) {
		ksft_print_msg("%s: target listener had no queued child after migration\n",
			       test_case->name);
	} else {
		ksft_perror("accept4(listener_b)");
	}

out_with_thread:
	close_fd(&probe);
	if (!thread_joined) {
		if (stop_accept_thread(thread, &cleanup_timed_out) == KSFT_FAIL) {
			ksft_print_msg("%s: failed to stop blocking accept waiter\n",
				       test_case->name);
			ret = KSFT_FAIL;
			goto out;
		}

		thread_joined = true;
	}
	if (thread_joined)
		close_fd(&result.accepted_fd);

out:
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
