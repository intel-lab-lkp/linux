// SPDX-License-Identifier: GPL-2.0

#define _GNU_SOURCE

#include <arpa/inet.h>
#include <error.h>
#include <errno.h>
#include <netinet/ip6.h>
#include <netinet/tcp.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>

#define RCVBUF_SIZE (1 << 21)

static const char *cfg_bind_addr = "::";
static int cfg_payload_len = 499700;
static int cfg_port = 8000;
static int cfg_msg_nr = 1;
static bool cfg_fastopen;

static char buf[RCVBUF_SIZE];
static bool interrupted;

static void sigint_handler(int signum)
{
	if (signum == SIGINT)
		interrupted = true;
}

static void send_message(int fd, const char *buf, size_t len)
{
	int done = 0;
	int ret;

	while (done < len) {
		ret = send(fd, &buf[done], len - done, 0);
		if (ret < 0)
			error(1, errno, "send");

		done += ret;
	}
}

static int connect_to_server(void)
{
	struct sockaddr_in6 addr;
	int ret;
	int fd;

	fd = socket(PF_INET6, SOCK_STREAM, 0);
	if (fd == -1)
		error(1, errno, "socket");

	addr.sin6_family = AF_INET6;
	addr.sin6_port = htons(cfg_port);
	if (inet_pton(AF_INET6, cfg_bind_addr, &addr.sin6_addr) != 1)
		error(1, 0, "ipv6 parse error: %s", cfg_bind_addr);

	if (cfg_fastopen) {
		ret = sendto(fd, &buf, cfg_payload_len, MSG_FASTOPEN, &addr,
			     sizeof(addr));
		if (ret < 0)
			error(1, errno, "sendto");

		send_message(fd, &buf[ret], cfg_payload_len - ret);
		cfg_msg_nr--;
	} else if (connect(fd, &addr, sizeof(addr))) {
		error(1, errno, "connect");
	}

	return fd;
}

static void usage(const char *filepath)
{
	error(1, 0, "Usage: %s [-D dst ip] [-f] [-M messagenr] [-p port]"
		    " [-s sendsize]",
		    filepath);
}

static void parse_opts(int argc, char **argv)
{
	int c;

	while ((c = getopt(argc, argv, "D:fhM:p:s:")) != -1) {
		switch (c) {
		case 'D':
			cfg_bind_addr = optarg;
			break;
		case 'f':
			cfg_fastopen = true;
			break;
		case 'h':
			usage(argv[0]);
			break;
		case 'M':
			cfg_msg_nr = strtoul(optarg, NULL, 10);
			break;
		case 'p':
			cfg_port = strtoul(optarg, NULL, 0);
			break;
		case 's':
			cfg_payload_len = strtoul(optarg, NULL, 0);
			break;
		default:
			exit(1);
		}
	}

	if (optind != argc)
		usage(argv[0]);

	if (cfg_payload_len > RCVBUF_SIZE)
		error(1, 0, "payload length %u exceeds max %u",
		      cfg_payload_len, RCVBUF_SIZE);
}

int main(int argc, char **argv)
{
	int fd;
	int i;

	parse_opts(argc, argv);

	memset(buf, 'A', sizeof(buf));

	signal(SIGINT, sigint_handler);

	fd = connect_to_server();
	for (i = 0; i < cfg_msg_nr && !interrupted; i++)
		send_message(fd, buf, cfg_payload_len);

	if (close(fd))
		error(1, errno, "close");

	return 0;
}
