// SPDX-License-Identifier: GPL-2.0

#define _GNU_SOURCE

#include <stddef.h>
#include <arpa/inet.h>
#include <error.h>
#include <errno.h>
#include <net/if.h>
#include <linux/in.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <netinet/if_ether.h>
#include <netinet/ip.h>
#include <netinet/ip6.h>
#include <netinet/udp.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef UDP_STOP_RCV
#define UDP_STOP_RCV	105
#endif

static bool			cfg_do_ipv4;
static bool			cfg_do_ipv6;

static char			buf[1024];
static const char *syn		= "client request";
static const char *synack	= "server accepted";
static const char *ack		= "established";

const struct in6_addr addr6 = {
	{ { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 } }, /* 0::1 */
};

const struct in_addr addr4 = {
	__constant_htonl(0x7f000001), /* 127.0.0.1 */
};

static int __send_one(const struct sockaddr *srv, const socklen_t srv_len)
{
	int cli_fd = -1, ret = 0;

	cli_fd = socket(srv->sa_family, SOCK_DGRAM, 0);
	if (cli_fd <= 0)
		goto err;

	ret = connect(cli_fd, srv, srv_len);
	if (ret < 0)
		goto err;

	ret = send(cli_fd, syn, strlen(syn), 0);
	if (ret != strlen(syn)) {
		ret = -1;
		goto err;
	}

	return cli_fd;
err:
	if (cli_fd > 0)
		close(cli_fd);
	return -1;
}

static int send_one(const struct sockaddr *srv, const socklen_t srv_len)
{
	int cli_fd;

	cli_fd = __send_one(srv, srv_len);
	if (cli_fd <= 0)
		return -1;

	close(cli_fd);
	return 0;
}

static int send_many(const struct sockaddr *addr, const socklen_t alen)
{
	int i = 0, err;

	for (i = 0; i < 100; i++) {
		err = send_one(addr, alen);
		if (err)
			return err;
	}
	return 0;
}

/*	client			server
 *	"client request"->
 *			<-	"server accepted"
 *	"established"	->
 */
static void run_test(struct sockaddr *srv, socklen_t srv_len,
		     struct sockaddr *cli, socklen_t cli_len)
{
	socklen_t size;
	struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
	int one = 1, srv_fd = -1, ret;
	int session_fd = -1;
	int cli_fd;

	srv_fd = socket(srv->sa_family, SOCK_DGRAM, 0);
	if (srv_fd == -1)
		error(1, errno, "socket srv_fd");

	if (setsockopt(srv_fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one)))
		error(1, errno, "SO_REUSEPORT");

	if (bind(srv_fd, srv, srv_len))
		error(1, errno, "bind srv_fd");

	if (getsockname(srv_fd, srv, &srv_len))
		error(1, errno, "getsockname()");

	/* send syn to server */
	cli_fd = __send_one(srv, srv_len);
	if (cli_fd < 0)
		error(1, errno, "new_client_req()");

	ret = recvfrom(srv_fd, (char *)buf, sizeof(buf), MSG_WAITALL, cli, &cli_len);
	if (ret < 0)
		error(1, errno, "recvfrom()");

	/* create session for this request */
	session_fd = socket(srv->sa_family, SOCK_DGRAM, 0);
	if (session_fd == -1)
		error(1, errno, "socket session_fd");

	if (setsockopt(session_fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one)))
		error(1, errno, "SO_REUSEPORT");

	/* we ready to bind the server address and do not want to receive any packets */
	if (setsockopt(session_fd, SOL_UDP, UDP_STOP_RCV, &one, sizeof(one)))
		error(1, errno, "setsockopt WAIT_CONNECT");

	if (setsockopt(session_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)))
		error(1, errno, "setsockopt SO_RCVTIMEO");

	one = 0;
	size = sizeof(one);
	if (getsockopt(session_fd, SOL_UDP, UDP_STOP_RCV, &one, &size) ||
	    one != 1)
		error(1, errno, "getsockopt UDP_STOP_RCV");

	/* bind the same address as srv_fd */
	if (bind(session_fd, srv, srv_len))
		error(1, errno, "bind srv_fd");

	/* simulate many other requests */
	if (send_many(srv, srv_len))
		error(1, errno, "send_many()");

	/* should no data assigned to session_fd
	 * as we set UDP_STOP_RCV before
	 */
	ret = read(session_fd, (char *)buf, sizeof(buf));
	if (ret > 0)
		error(1, errno, "session_fd should no data received");

	/* build 4-tuple */
	ret = connect(session_fd, cli, cli_len);
	if (ret < 0)
		error(1, errno, "connect(cli)");

	/* now we are ready to communicate with specified client */
	one = 0;
	if (setsockopt(session_fd, SOL_UDP, UDP_STOP_RCV, &one, sizeof(one)))
		error(1, errno, "setsockopt WAIT_CONNECT");

	/* server sends synack to the client */
	ret = send(session_fd, synack, strlen(synack), 0);
	if (ret != strlen(synack))
		error(1, errno, "send(synack)");

	/* client receives the synack */
	ret = read(cli_fd, (char *)buf, sizeof(buf));
	if (ret != strlen(synack))
		error(1, errno, "read(synack)");

	/* client sends the ack to server */
	ret = send(cli_fd, ack, strlen(ack), 0);
	if (ret != strlen(ack))
		error(1, errno, "send(ack)");

	/* the server should receive the ack */
	ret = read(session_fd, (char *)buf, sizeof(buf));
	if (ret != strlen(ack))
		error(1, errno, "read(ack)");

	/* send many requests that not belongs to the session */
	if (send_many(srv, srv_len))
		error(1, errno, "send_many()");

	ret = read(session_fd, (char *)buf, sizeof(buf));
	if (ret > 0)
		error(1, errno, "session_fd should no data received");

	if (cli_fd != -1)
		close(cli_fd);
	if (srv_fd != -1)
		close(srv_fd);
	if (session_fd != -1)
		close(session_fd);
}

static void run_test_v4(void)
{
	struct sockaddr_in addr = {0};
	struct sockaddr_in cli = {0};

	addr.sin_family = AF_INET;
	addr.sin_port = 0;
	addr.sin_addr = addr4;

	run_test((void *)&addr, sizeof(addr), (void *)&cli, sizeof(cli));
	fprintf(stderr, "v4 OK\n");
}

static void run_test_v6(void)
{
	struct sockaddr_in6 addr = {0};
	struct sockaddr_in6 cli = {0};

	addr.sin6_family = AF_INET6;
	addr.sin6_port = 0;
	addr.sin6_addr = addr6;

	run_test((void *)&addr, sizeof(addr), (void *)&cli, sizeof(cli));
	fprintf(stderr, "v6 OK\n");
}

static void parse_opts(int argc, char **argv)
{
	int c;

	while ((c = getopt(argc, argv, "46")) != -1) {
		switch (c) {
		case '4':
			cfg_do_ipv4 = true;
			break;
		case '6':
			cfg_do_ipv6 = true;
			break;
		default:
			error(1, 0, "%s: parse error", argv[0]);
		}
	}

	if (!cfg_do_ipv4 && !cfg_do_ipv6) {
		cfg_do_ipv4 = 1;
		cfg_do_ipv6 = 1;
	}
}

int main(int argc, char **argv)
{
	parse_opts(argc, argv);

	if (cfg_do_ipv4)
		run_test_v4();
	if (cfg_do_ipv6)
		run_test_v6();

	fprintf(stderr, "test OK\n");
	return 0;
}
