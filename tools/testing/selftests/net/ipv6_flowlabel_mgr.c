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

#define explain(x)							\
	do { if (cfg_verbose) fprintf(stderr, "       " x "\n"); } while (0)

#define __expect(x)							\
	do {								\
		if (!(x))						\
			fprintf(stderr, "[OK]   " #x "\n");		\
		else							\
			error(1, 0, "[ERR]  " #x " (line %d)", __LINE__); \
	} while (0)

#define expect_pass(x)	__expect(x)
#define expect_fail(x)	__expect(!(x))

static bool cfg_long_running;
static bool cfg_verbose;

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
	int fd;

	fd = open("/proc/sys/net/ipv6/flowlabel_consistency", O_WRONLY);
	if (fd == -1)
		error(1, errno, "open flowlabel_consistency");
	if (write(fd, enable ? "1" : "0", 1) != 1)
		error(1, errno, "write flowlabel_consistency");
	if (close(fd))
		error(1, errno, "close flowlabel_consistency");
}

static void run_tests(int fd)
{
	int wstatus;
	pid_t pid;

	explain("cannot get non-existent label");
	expect_fail(flowlabel_get(fd, 1, IPV6_FL_S_ANY, 0));

	explain("cannot put non-existent label");
	expect_fail(flowlabel_put(fd, 1));

	explain("cannot create label greater than 20 bits");
	expect_fail(flowlabel_get(fd, 0x1FFFFF, IPV6_FL_S_ANY,
				  IPV6_FL_F_CREATE));

	explain("create a new label (FL_F_CREATE)");
	expect_pass(flowlabel_get(fd, 1, IPV6_FL_S_ANY, IPV6_FL_F_CREATE));
	explain("can get the label (without FL_F_CREATE)");
	expect_pass(flowlabel_get(fd, 1, IPV6_FL_S_ANY, 0));
	explain("can get it again with create flag set, too");
	expect_pass(flowlabel_get(fd, 1, IPV6_FL_S_ANY, IPV6_FL_F_CREATE));
	explain("cannot get it again with the exclusive (FL_FL_EXCL) flag");
	expect_fail(flowlabel_get(fd, 1, IPV6_FL_S_ANY,
					 IPV6_FL_F_CREATE | IPV6_FL_F_EXCL));
	explain("can now put exactly three references");
	expect_pass(flowlabel_put(fd, 1));
	expect_pass(flowlabel_put(fd, 1));
	expect_pass(flowlabel_put(fd, 1));
	expect_fail(flowlabel_put(fd, 1));

	explain("create a new exclusive label (FL_S_EXCL)");
	expect_pass(flowlabel_get(fd, 2, IPV6_FL_S_EXCL, IPV6_FL_F_CREATE));
	explain("cannot get it again in non-exclusive mode");
	expect_fail(flowlabel_get(fd, 2, IPV6_FL_S_ANY,  IPV6_FL_F_CREATE));
	explain("cannot get it again in exclusive mode either");
	expect_fail(flowlabel_get(fd, 2, IPV6_FL_S_EXCL, IPV6_FL_F_CREATE));
	expect_pass(flowlabel_put(fd, 2));

	if (cfg_long_running) {
		explain("cannot reuse the label, due to linger");
		expect_fail(flowlabel_get(fd, 2, IPV6_FL_S_ANY,
					  IPV6_FL_F_CREATE));
		explain("after sleep, can reuse");
		sleep(FL_MIN_LINGER * 2 + 1);
		expect_pass(flowlabel_get(fd, 2, IPV6_FL_S_ANY,
					  IPV6_FL_F_CREATE));
	}

	explain("create a new user-private label (FL_S_USER)");
	expect_pass(flowlabel_get(fd, 3, IPV6_FL_S_USER, IPV6_FL_F_CREATE));
	explain("cannot get it again in non-exclusive mode");
	expect_fail(flowlabel_get(fd, 3, IPV6_FL_S_ANY, 0));
	explain("cannot get it again in exclusive mode");
	expect_fail(flowlabel_get(fd, 3, IPV6_FL_S_EXCL, 0));
	explain("can get it again in user mode");
	expect_pass(flowlabel_get(fd, 3, IPV6_FL_S_USER, 0));
	explain("child process can get it too, but not after setuid(nobody)");
	pid = fork();
	if (pid == -1)
		error(1, errno, "fork");
	if (!pid) {
		expect_pass(flowlabel_get(fd, 3, IPV6_FL_S_USER, 0));
		if (setuid(USHRT_MAX))
			fprintf(stderr, "[INFO] skip setuid child test\n");
		else
			expect_fail(flowlabel_get(fd, 3, IPV6_FL_S_USER, 0));
		exit(0);
	}
	if (wait(&wstatus) == -1)
		error(1, errno, "wait");
	if (!WIFEXITED(wstatus) || WEXITSTATUS(wstatus) != 0)
		error(1, errno, "wait: unexpected child result");

	explain("create a new process-private label (FL_S_PROCESS)");
	expect_pass(flowlabel_get(fd, 4, IPV6_FL_S_PROCESS, IPV6_FL_F_CREATE));
	explain("can get it again");
	expect_pass(flowlabel_get(fd, 4, IPV6_FL_S_PROCESS, 0));
	explain("child process cannot can get it");
	pid = fork();
	if (pid == -1)
		error(1, errno, "fork");
	if (!pid) {
		expect_fail(flowlabel_get(fd, 4, IPV6_FL_S_PROCESS, 0));
		exit(0);
	}
	if (wait(&wstatus) == -1)
		error(1, errno, "wait");
	if (!WIFEXITED(wstatus) || WEXITSTATUS(wstatus) != 0)
		error(1, errno, "wait: unexpected child result");

	if (cfg_long_running) {
		explain("create a new label with FL_MIN_LINGER linger time");
		expect_pass(flowlabel_get(fd, 5, IPV6_FL_S_EXCL, IPV6_FL_F_CREATE));
		explain("renew the label to increase its linger time and put it");
		expect_pass(flowlabel_renew(fd, 5, 2 * (FL_MIN_LINGER * 2 + 1)));
		expect_pass(flowlabel_put(fd, 5));
		sleep(FL_MIN_LINGER * 2 + 1);
		explain("The label cannot be created because the new linger time is not over yet");
		expect_fail(flowlabel_get(fd, 5, IPV6_FL_S_ANY, IPV6_FL_F_CREATE));
	}

	explain("Prepare TCP SYN for REMOTE flag validation");
	int remote_listener = tcp_listen();
	int remote_cfd, remote_afd;
	tcp_connect(remote_listener, 6, &remote_cfd, &remote_afd);
	struct in6_flowlabel_req freq = {
		.flr_action = IPV6_FL_A_GET,
		.flr_flags = IPV6_FL_F_REMOTE,
	};
	socklen_t freq_len = sizeof(freq);
	explain("Query for label sent by client with IPV6_FL_F_REMOTE");
	expect_pass(getsockopt(remote_afd, SOL_IPV6, IPV6_FLOWLABEL_MGR, &freq, &freq_len));
	expect_pass(ntohl(freq.flr_label) != 6);
	close(remote_afd);
	close(remote_cfd);
	close(remote_listener);

	explain("Prepare TCP SYN for REFLECT flag validation");
	set_flowlabel_consistency(false);
	int reflect_listener = tcp_listen();
	struct in6_flowlabel_req reflect_on = {
		.flr_action = IPV6_FL_A_GET,
		.flr_flags = IPV6_FL_F_REFLECT,
	};
	explain("Enable REFLECT on the listener before the client connects");
	expect_pass(setsockopt(reflect_listener, SOL_IPV6, IPV6_FLOWLABEL_MGR, &reflect_on, sizeof(reflect_on)));
	int reflect_cfd, reflect_afd;
	tcp_connect(reflect_listener, 7, &reflect_cfd, &reflect_afd);
	struct in6_flowlabel_req reflect_query = {
		.flr_action = IPV6_FL_A_GET,
	};
	socklen_t reflect_query_len = sizeof(reflect_query);
	explain("Query the accepted socket's outgoing label, should be reflected");
	expect_pass(getsockopt(reflect_afd, SOL_IPV6, IPV6_FLOWLABEL_MGR, &reflect_query, &reflect_query_len));
	expect_pass(ntohl(reflect_query.flr_label) != 7);
	struct in6_flowlabel_req reflect_off = {
		.flr_action = IPV6_FL_A_PUT,
		.flr_flags = IPV6_FL_F_REFLECT,
	};
	explain("PUT+REFLECT disables reflection on the accepted socket");
	expect_pass(setsockopt(reflect_afd, SOL_IPV6, IPV6_FLOWLABEL_MGR, &reflect_off, sizeof(reflect_off)));
	explain("cannot disable reflection twice");
	expect_fail(setsockopt(reflect_afd, SOL_IPV6, IPV6_FLOWLABEL_MGR, &reflect_off, sizeof(reflect_off)));
	set_flowlabel_consistency(true);
	close(reflect_afd);
	close(reflect_cfd);
	close(reflect_listener);
}

static void parse_opts(int argc, char **argv)
{
	int c;

	while ((c = getopt(argc, argv, "lv")) != -1) {
		switch (c) {
		case 'l':
			cfg_long_running = true;
			break;
		case 'v':
			cfg_verbose = true;
			break;
		default:
			error(1, 0, "%s: parse error", argv[0]);
		}
	}
}

int main(int argc, char **argv)
{
	int fd;

	parse_opts(argc, argv);

	fd = socket(PF_INET6, SOCK_DGRAM, 0);
	if (fd == -1)
		error(1, errno, "socket");

	run_tests(fd);

	if (close(fd))
		error(1, errno, "close");

	return 0;
}
