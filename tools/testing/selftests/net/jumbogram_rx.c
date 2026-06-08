// SPDX-License-Identifier: GPL-2.0

#define _GNU_SOURCE

#include <arpa/inet.h>
#include <error.h>
#include <errno.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>

#define POLL_TIMEOUT 10
#define RCVBUF_SIZE (1 << 21)

static const char *cfg_bind_addr = "::";
static int cfg_connect_timeout_ms;
static int cfg_rcv_timeout_ms;
static int cfg_port = 8000;
static int cfg_total_len;
static int cfg_fastopen;

static unsigned long bytes;
static bool interrupted;

static void sigint_handler(int signum)
{
	if (signum == SIGINT)
		interrupted = true;
}

static void wait_for_data(int fd, int timeout_ms)
{
	struct pollfd pfd;

	pfd.events = POLLIN;
	pfd.revents = 0;
	pfd.fd = fd;

	while (true) {
		int ret = poll(&pfd, 1, POLL_TIMEOUT);

		if (interrupted || (ret > 0 && pfd.revents == POLLIN))
			break;
		else if (ret > 0)
			error(1, errno, "poll: 0x%x expected 0x%x\n",
			      pfd.revents, POLLIN);
		else if (ret == -1)
			error(1, errno, "poll");

		if (!timeout_ms)
			continue;

		timeout_ms -= POLL_TIMEOUT;
		if (timeout_ms <= 0) {
			interrupted = true;
			break;
		}

		/* no events and more time to wait, do poll again */
	}
}

static int accept_connection(void)
{
	static struct sockaddr_in6 addr;
	int server_fd, fd;
	int val;

	server_fd = socket(PF_INET6, SOCK_STREAM, 0);
	if (server_fd == -1)
		error(1, errno, "socket");

	val = RCVBUF_SIZE;
	if (setsockopt(server_fd, SOL_SOCKET, SO_RCVBUF, &val, sizeof(val)))
		error(1, errno, "setsockopt rcvbuf");

	val = 1;
	if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEPORT, &val, sizeof(val)))
		error(1, errno, "setsockopt reuseport");

	if (cfg_fastopen &&
	    setsockopt(server_fd, SOL_TCP, TCP_FASTOPEN, &val, sizeof(val)))
		error(1, errno, "setsockopt fastopen");

	addr.sin6_family = AF_INET6;
	addr.sin6_port = htons(cfg_port);
	if (inet_pton(AF_INET6, cfg_bind_addr, &addr.sin6_addr) != 1)
		error(1, 0, "ipv6 parse error: %s", cfg_bind_addr);

	if (bind(server_fd, &addr, sizeof(addr)))
		error(1, errno, "bind");

	if (listen(server_fd, 1))
		error(1, errno, "listen");

	puts("listening for connection");

	wait_for_data(server_fd, cfg_connect_timeout_ms);
	if (interrupted)
		exit(0);

	fd = accept(server_fd, NULL, NULL);
	if (fd == -1)
		error(1, errno, "accept");

	if (close(server_fd))
		error(1, errno, "close accept fd");

	return fd;
}

static bool receive_packets(int fd)
{
	int ret;

	while (true) {
		ret = recv(fd, NULL, RCVBUF_SIZE, MSG_TRUNC | MSG_DONTWAIT);
		if (ret > 0)
			bytes += ret;
		else if (ret == 0)
			return true;
		else if (errno == EAGAIN)
			return false;
		else
			error(1, errno, "recv");
	}

}


static void usage(const char *filepath)
{
	error(1, 0, "Usage: %s [-C connect_timeout] [-b addr] [-f] [-p port]"
	      " [-l total_len] [-n packetnr] [-R rcv_timeout]",
	      filepath);
}

static void parse_opts(int argc, char **argv)
{
	int c;

	while ((c = getopt(argc, argv, "b:C:fhl:p:R:")) != -1) {
		switch (c) {
		case 'b':
			cfg_bind_addr = optarg;
			break;
		case 'C':
			cfg_connect_timeout_ms = strtoul(optarg, NULL, 0);
			break;
		case 'f':
			cfg_fastopen = true;
			break;
		case 'h':
			usage(argv[0]);
			break;
		case 'l':
			cfg_total_len = strtoul(optarg, NULL, 0);
			break;
		case 'p':
			cfg_port = strtoul(optarg, NULL, 0);
			break;
		case 'R':
			cfg_rcv_timeout_ms = strtoul(optarg, NULL, 0);
			break;
		default:
			exit(1);
		}
	}

	if (optind != argc)
		usage(argv[0]);
}

int main(int argc, char **argv)
{
	parse_opts(argc, argv);

	signal(SIGINT, sigint_handler);

	int fd = accept_connection();
	int stop = false;

	while (!interrupted && !stop) {
		wait_for_data(fd, cfg_rcv_timeout_ms);
		stop = receive_packets(fd);
	}

	if (cfg_total_len && (bytes != cfg_total_len))
		error(1, 0, "wrong data length! got %ld, expected %d\n",
		      bytes, cfg_total_len);

	if (close(fd))
		error(1, errno, "close");

	return 0;
}
