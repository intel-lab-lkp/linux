// SPDX-License-Identifier: GPL-2.0-only
/*
 * virtio_vsock_test - vsock.ko test suite for VirtIO transport.
 *
 * Copyright (C) 2024 SaluteDevices.
 *
 * Author: Arseniy Krasnov <AVKrasnov@salutedevices.com>
 */
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <linux/if.h>
#include <linux/if_packet.h>
#include <linux/if_ether.h>
#include <linux/virtio_vsock.h>
#include <linux/vsockmon.h>
#include <linux/sockios.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/socket.h>

#include "control.h"
#include "util.h"

#define RAW_SOCK_RCV_BUF	(1024 * 1024)

#define TOTAL_TX_BYTES		(1024 * 256)

#define VIRTIO_VSOCK_MAX_PKT_BUF_SIZE	(1024 * 64)

#define VIRTIO_VSOCK_GUEST_RX_BUF_SIZE	(4096)
#define VIRTIO_VSOCK_HOST_RX_BUF_SIZE	(1024 * 64)

static const char *interface;

static void test_stream_deferred_credit_update_client(const struct test_opts *opts)
{
	unsigned char buf[VIRTIO_VSOCK_MAX_PKT_BUF_SIZE];
	int fd;
	int i;

	fd = vsock_stream_connect(opts->peer_cid, opts->peer_port);
	if (fd < 0) {
		perror("connect");
		exit(EXIT_FAILURE);
	}

	control_expectln("SERVERREADY");
	memset(buf, 0, sizeof(buf));

	for (i = 0; i < TOTAL_TX_BYTES / VIRTIO_VSOCK_MAX_PKT_BUF_SIZE; i++) {
		if (write(fd, buf, sizeof(buf)) != sizeof(buf)) {
			perror("write");
			exit(EXIT_FAILURE);
		}
	}

	control_writeln("CLIENTDONE");
	control_expectln("SERVERDONE");

	close(fd);
}

static int create_raw_sock(const char *ifname)
{
	struct sockaddr_ll addr_ll;
	struct ifreq ifr;
	size_t rcv_buf;
	int fd;

	fd = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
	if (fd < 0) {
		perror("socket");
		exit(EXIT_FAILURE);
	}

	memset(&ifr, 0, sizeof(ifr));
	snprintf(ifr.ifr_name, sizeof(ifr.ifr_name),
		 "%s", ifname);

	rcv_buf = RAW_SOCK_RCV_BUF;
	if (setsockopt(fd, SOL_SOCKET, SO_RCVBUFFORCE, &rcv_buf,
				sizeof(rcv_buf))) {
		perror("setsockopt(SO_RCVBUFFORCE)");
		exit(EXIT_FAILURE);
	}

	if (ioctl(fd, SIOCGIFINDEX, &ifr)) {
		perror("ioctl(SIOCGIFINDEX)");
		exit(EXIT_FAILURE);
	}

	memset(&addr_ll, 0, sizeof(addr_ll));
	addr_ll.sll_family = AF_PACKET;
	addr_ll.sll_ifindex = ifr.ifr_ifindex;
	addr_ll.sll_protocol = htons(ETH_P_ALL);

	if (bind(fd, (struct sockaddr *)&addr_ll, sizeof(struct sockaddr_ll)) < 0) {
		perror("bind");
		exit(EXIT_FAILURE);
	}

	return fd;
}


static void check_raw_packet(int raw_fd, uint16_t expected_op)
{
	struct af_vsockmon_hdr *mhdr;
	struct virtio_vsock_hdr *hdr;
	unsigned char buf[VIRTIO_VSOCK_MAX_PKT_BUF_SIZE +
			  sizeof(*mhdr) + sizeof(*hdr)] = { 0 };
	ssize_t res;

	res = read(raw_fd, buf, sizeof(buf));

	if (res == -1) {
		if (expected_op == 0xffff && errno == EAGAIN)
			return;

		fprintf(stderr, "Unexpected raw read: %i\n", errno);
		exit(EXIT_FAILURE);
	}

	mhdr = (struct af_vsockmon_hdr *)buf;
	hdr =  (struct virtio_vsock_hdr *)(mhdr + 1);

	if (hdr->op != expected_op) {
		fprintf(stderr, "Unexpected op: %hhu, but %hhu expected\n",
				hdr->op, expected_op);
		exit(EXIT_FAILURE);
	}
}

static void test_stream_deferred_credit_update_server(const struct test_opts *opts)
{
	char buf[TOTAL_TX_BYTES] = { 0 };
	int raw_fd, fd, rx_packet_size, i;
	int credit_update_pkts;
	struct timeval tv;
	size_t total_recv;

	fd = vsock_stream_accept(VMADDR_CID_ANY, opts->peer_port, NULL);
	if (fd < 0) {
		perror("accept");
		exit(EXIT_FAILURE);
	}

	raw_fd = create_raw_sock(interface);

	tv.tv_sec = 2;
	tv.tv_usec = 0;

	if (setsockopt(raw_fd, SOL_SOCKET, SO_RCVTIMEO,
		       (void *)&tv, sizeof(tv))) {
		perror("setsockopt(SO_RCVTIMEO)");
		exit(EXIT_FAILURE);
	}

	control_writeln("SERVERREADY");
	control_expectln("CLIENTDONE");

	total_recv = 0;

	/* Wait, until we receive whole data. */
	while (1) {
		ssize_t ret;

		ret = recv(fd, buf, sizeof(buf), MSG_PEEK);
		if (ret == sizeof(buf))
			break;

		if (ret <= 0) {
			fprintf(stderr, "unexpected 'recv()' return: %zi\n", ret);
			exit(EXIT_FAILURE);
		}
	}

	if (opts->peer_cid == VMADDR_CID_HOST)
		rx_packet_size = VIRTIO_VSOCK_GUEST_RX_BUF_SIZE;
	else
		rx_packet_size = VIRTIO_VSOCK_HOST_RX_BUF_SIZE;

	while (1) {
		ssize_t ret;

		ret = read(fd, buf, rx_packet_size);
		if (ret <= 0) {
			perror("read");
			exit(EXIT_FAILURE);
		}

		check_raw_packet(raw_fd, VIRTIO_VSOCK_OP_RW);

		total_recv += ret;

		if (total_recv >= TOTAL_TX_BYTES)
			break;
	}

	if (total_recv != TOTAL_TX_BYTES) {
		fprintf(stderr, "Invalid number of received bytes: %zu",
			total_recv);
		exit(EXIT_FAILURE);
	}

	credit_update_pkts = VIRTIO_VSOCK_MAX_PKT_BUF_SIZE / rx_packet_size;
	for (i = 0; i < credit_update_pkts; i++)
		check_raw_packet(raw_fd, VIRTIO_VSOCK_OP_CREDIT_UPDATE);

	/* Check that there are no new packets. */
	check_raw_packet(raw_fd, 0xffff);

	control_writeln("SERVERDONE");

	close(fd);
	close(raw_fd);
}

static struct test_case test_cases[] = {
	{
		.name = "SOCK_STREAM deferred credit update",
		.run_client = test_stream_deferred_credit_update_client,
		.run_server = test_stream_deferred_credit_update_server,
	},
	{}
};

static const char optstring[] = "";
static const struct option longopts[] = {
	{
		.name = "control-host",
		.has_arg = required_argument,
		.val = 'H',
	},
	{
		.name = "control-port",
		.has_arg = required_argument,
		.val = 'P',
	},
	{
		.name = "mode",
		.has_arg = required_argument,
		.val = 'm',
	},
	{
		.name = "peer-cid",
		.has_arg = required_argument,
		.val = 'p',
	},
	{
		.name = "peer-port",
		.has_arg = required_argument,
		.val = 'q',
	},
	{
		.name = "interface",
		.has_arg = required_argument,
		.val = 'i',
	},
	{
		.name = "help",
		.has_arg = no_argument,
		.val = '?',
	},
	{},
};

static void usage(void)
{
	fprintf(stderr, "Usage: virtio_vsock_test [--help] [--control-host=<host>] --control-port=<port> --mode=client|server --peer-cid=<cid> [--peer-port=<port>] [--list] [--interface=<iface name>]\n"
		"\n"
		"  Server: virtio_vsock_test --control-port=1234 --mode=server --peer-cid=3 --interface=vsockmon0\n"
		"  Client: virtio_vsock_test --control-host=192.168.0.1 --control-port=1234 --mode=client --peer-cid=2\n"
		"\n"
		"Run AF_VSOCK tests, specific for virtio transport. Requires\n"
		"permissions to open raw socket.\n"
		"\n"
		"A TCP control socket connection is used to coordinate tests\n"
		"between the client and the server.  The server requires a\n"
		"listen address and the client requires an address to\n"
		"connect to.\n"
		"\n"
		"The CID of the other side must be given with --peer-cid=<cid>.\n"
		"During the test, two AF_VSOCK ports will be used: the port\n"
		"specified with --peer-port=<port> (or the default port)\n"
		"and the next one.\n"
		"\n"
		"Options:\n"
		"  --help                 This help message\n"
		"  --control-host <host>  Server IP address to connect to\n"
		"  --control-port <port>  Server port to listen on/connect to\n"
		"  --mode client|server   Server or client mode\n"
		"  --peer-cid <cid>       CID of the other side\n"
		"  --peer-port <port>     AF_VSOCK port used for the test [default: %d]\n"
		"  --interface <iface name>\n",
		DEFAULT_PEER_PORT
	       );
}

int main(int argc, char *argv[])
{
	const char *control_host = NULL;
	const char *control_port = NULL;
	struct test_opts opts = {
		.mode = TEST_MODE_UNSET,
		.peer_cid = VMADDR_CID_ANY,
		.peer_port = DEFAULT_PEER_PORT,
	};

	for (;;) {
		int opt = getopt_long(argc, argv, optstring, longopts, NULL);

		if (opt == -1)
			break;

		switch (opt) {
		case 'H':
			control_host = optarg;
			break;
		case 'P':
			control_port = optarg;
			break;
		case 'm':
			if (strcmp(optarg, "client") == 0)
				opts.mode = TEST_MODE_CLIENT;
			else if (strcmp(optarg, "server") == 0)
				opts.mode = TEST_MODE_SERVER;
			else {
				fprintf(stderr, "--mode must be \"client\" or \"server\"\n");
				return EXIT_FAILURE;
			}
			break;
		case 'p':
			opts.peer_cid = parse_cid(optarg);
			break;
		case 'q':
			opts.peer_port = parse_port(optarg);
			break;
		case 'i':
			interface = optarg;
		default:
			usage();
		}
	}

	if (!control_host) {
		if (opts.mode != TEST_MODE_SERVER)
			usage();
		control_host = "0.0.0.0";
	}

	if (!interface)
		interface = "vsockmon0";

	control_init(control_host, control_port,
		     opts.mode == TEST_MODE_SERVER);

	run_tests(test_cases, &opts);

	return EXIT_SUCCESS;
}
