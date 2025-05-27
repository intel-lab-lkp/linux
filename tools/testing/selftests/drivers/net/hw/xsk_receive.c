// SPDX-License-Identifier: GPL-2.0
#include <error.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <poll.h>
#include <stdatomic.h>
#include <unistd.h>
#include <sys/mman.h>
#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <linux/if_xdp.h>

#include "xsk_receive.skel.h"

#define load_acquire(p) \
	atomic_load_explicit((_Atomic typeof(*(p)) *)(p), memory_order_acquire)

#define store_release(p, v) \
	atomic_store_explicit((_Atomic typeof(*(p)) *)(p), v, \
			      memory_order_release)

#define UMEM_CHUNK_SIZE 0x1000
#define BUFFER_SIZE 0x2000

#define SERVER_PORT 8888
#define CLIENT_PORT 9999

const int num_entries = 256;
const char *pass_msg = "PASS";

int cfg_client;
int cfg_server;
char *cfg_server_ip;
char *cfg_client_ip;
int cfg_ifindex;
int cfg_redirect;
int cfg_zerocopy;

struct xdp_sock_context {
	int xdp_sock;
	void *umem_region;
	void *rx_ring;
	void *fill_ring;
	struct xdp_mmap_offsets off;
};

struct xdp_sock_context *setup_xdp_socket(int ifindex)
{
	struct xdp_mmap_offsets off;
	void *rx_ring, *fill_ring;
	struct xdp_umem_reg umem_reg = {};
	int optlen = sizeof(off);
	int umem_len, sock, ret, i;
	void *umem_region;
	uint32_t *fr_producer;
	uint64_t *addr;
	struct sockaddr_xdp sxdp = {
		.sxdp_family = AF_XDP,
		.sxdp_ifindex = ifindex,
		.sxdp_queue_id = 0,
		.sxdp_flags = XDP_USE_SG,
	};
	struct xdp_sock_context *ctx;

	ctx = malloc(sizeof(*ctx));
	if (!ctx)
		error(1, 0, "malloc()");

	if (cfg_zerocopy)
		sxdp.sxdp_flags |= XDP_ZEROCOPY;
	else
		sxdp.sxdp_flags |= XDP_COPY;

	umem_len = UMEM_CHUNK_SIZE * num_entries;
	umem_region = mmap(0, umem_len, PROT_READ | PROT_WRITE,
			   MAP_PRIVATE | MAP_ANONYMOUS, 0, 0);
	if (umem_region == MAP_FAILED)
		error(1, errno, "mmap() umem");
	ctx->umem_region = umem_region;

	sock = socket(AF_XDP, SOCK_RAW, 0);
	if (sock < 0)
		error(1, errno, "socket() XDP");
	ctx->xdp_sock = sock;

	ret = setsockopt(sock, SOL_XDP, XDP_RX_RING, &num_entries,
			 sizeof(num_entries));
	if (ret < 0)
		error(1, errno, "setsockopt() XDP_RX_RING");

	ret = setsockopt(sock, SOL_XDP, XDP_UMEM_COMPLETION_RING, &num_entries,
			 sizeof(num_entries));
	if (ret < 0)
		error(1, errno, "setsockopt() XDP_UMEM_COMPLETION_RING");

	ret = setsockopt(sock, SOL_XDP, XDP_UMEM_FILL_RING, &num_entries,
			 sizeof(num_entries));
	if (ret < 0)
		error(1, errno, "setsockopt() XDP_UMEM_FILL_RING");

	ret = getsockopt(sock, SOL_XDP, XDP_MMAP_OFFSETS, &off, &optlen);
	if (ret < 0)
		error(1, errno, "getsockopt()");
	ctx->off = off;

	rx_ring = mmap(0, off.rx.desc + num_entries * sizeof(struct xdp_desc),
		       PROT_READ | PROT_WRITE, MAP_SHARED, sock,
		       XDP_PGOFF_RX_RING);
	if (rx_ring == (void *)-1)
		error(1, errno, "mmap() rx-ring");
	ctx->rx_ring = rx_ring;

	fill_ring = mmap(0, off.fr.desc + num_entries * sizeof(uint64_t),
			 PROT_READ | PROT_WRITE, MAP_SHARED, sock,
			 XDP_UMEM_PGOFF_FILL_RING);
	if (fill_ring == (void *)-1)
		error(1, errno, "mmap() fill-ring");
	ctx->fill_ring = fill_ring;

	umem_reg.addr = (unsigned long long)ctx->umem_region;
	umem_reg.len = umem_len;
	umem_reg.chunk_size = UMEM_CHUNK_SIZE;
	ret = setsockopt(sock, SOL_XDP, XDP_UMEM_REG, &umem_reg,
			 sizeof(umem_reg));
	if (ret < 0)
		error(1, errno, "setsockopt() XDP_UMEM_REG");

	i = 0;
	while (1) {
		ret = bind(sock, (const struct sockaddr *)&sxdp, sizeof(sxdp));
		if (!ret)
			break;

		if (errno == EBUSY && i < 3) {
			i++;
			sleep(1);
		} else {
			error(1, errno, "bind() XDP");
		}
	}

	/* Submit all umem entries to fill ring */
	addr = fill_ring + off.fr.desc;
	for (i = 0; i < umem_len; i += UMEM_CHUNK_SIZE) {
		*addr = i;
		addr++;
	}
	fr_producer = fill_ring + off.fr.producer;
	store_release(fr_producer, num_entries);

	return ctx;
}

void setup_xdp_prog(int sock, int ifindex, int redirect)
{
	struct xsk_receive_bpf *bpf;
	int key, ret;

	bpf = xsk_receive_bpf__open_and_load();
	if (!bpf)
		error(1, 0, "open eBPF");

	key = 0;
	ret = bpf_map__update_elem(bpf->maps.xsk_map, &key, sizeof(key),
				   &sock, sizeof(sock), 0);
	if (ret < 0)
		error(1, errno, "eBPF map update");

	if (redirect) {
		ret = bpf_xdp_attach(ifindex,
				bpf_program__fd(bpf->progs.redirect_xsk_prog),
				0, NULL);
		if (ret < 0)
			error(1, errno, "attach eBPF");
	} else {
		ret = bpf_xdp_attach(ifindex,
				     bpf_program__fd(bpf->progs.dummy_prog),
				     0, NULL);
		if (ret < 0)
			error(1, errno, "attach eBPF");
	}
}

void send_pass_msg(int sock)
{
	int ret;
	struct sockaddr_in addr = {
		.sin_family = AF_INET,
		.sin_addr = inet_addr(cfg_client_ip),
		.sin_port = htons(CLIENT_PORT),
	};

	ret = sendto(sock, pass_msg, sizeof(pass_msg), 0,
		     (const struct sockaddr *)&addr, sizeof(addr));
	if (ret < 0)
		error(1, errno, "sendto()");
}

void server_recv_xdp(struct xdp_sock_context *ctx, int udp_sock)
{
	int ret;
	struct pollfd fds = {
		.fd = ctx->xdp_sock,
		.events = POLLIN,
	};

	ret = poll(&fds, 1, -1);
	if (ret < 0)
		error(1, errno, "poll()");

	if (fds.revents & POLLIN) {
		uint32_t *producer_ptr = ctx->rx_ring + ctx->off.rx.producer;
		uint32_t *consumer_ptr = ctx->rx_ring + ctx->off.rx.consumer;
		uint32_t producer, consumer;
		struct xdp_desc *desc;

		producer = load_acquire(producer_ptr);
		consumer = load_acquire(consumer_ptr);

		printf("Receive %d XDP buffers\n", producer - consumer);

		store_release(consumer_ptr, producer);
	} else {
		error(1, 0, "unexpected poll event: %d", fds.revents);
	}

	send_pass_msg(udp_sock);
}

void server_recv_udp(int sock)
{
	char *buffer;
	int i, ret;

	buffer = mmap(0, BUFFER_SIZE, PROT_READ | PROT_WRITE,
		      MAP_PRIVATE | MAP_ANONYMOUS, 0, 0);
	if (buffer == MAP_FAILED)
		error(1, errno, "mmap() send buffer");

	ret = recv(sock, buffer, BUFFER_SIZE, 0);
	if (ret < 0)
		error(1, errno, "recv()");

	if (ret != BUFFER_SIZE)
		error(1, errno, "message is truncated, expected: %d, got: %d",
		      BUFFER_SIZE, ret);

	for (i = 0; i < BUFFER_SIZE; i++)
		if (buffer[i] != 'a' + (i % 26))
			error(1, 0, "message mismatches at %d", i);

	send_pass_msg(sock);
}

int setup_udp_sock(const char *addr, int port)
{
	int sock, ret;
	struct sockaddr_in saddr = {
		.sin_family = AF_INET,
		.sin_addr = inet_addr(addr),
		.sin_port = htons(port),
	};

	sock = socket(AF_INET, SOCK_DGRAM, 0);
	if (sock < 0)
		error(1, errno, "socket() UDP");

	ret = bind(sock, (const struct sockaddr *)&saddr, sizeof(saddr));
	if (ret < 0)
		error(1, errno, "bind() UDP");

	return sock;
}

void run_server(void)
{
	int udp_sock;
	struct xdp_sock_context *ctx;

	ctx = setup_xdp_socket(cfg_ifindex);
	setup_xdp_prog(ctx->xdp_sock, cfg_ifindex, cfg_redirect);
	udp_sock = setup_udp_sock(cfg_server_ip, SERVER_PORT);

	if (cfg_redirect)
		server_recv_xdp(ctx, udp_sock);
	else
		server_recv_udp(udp_sock);
}

void run_client(void)
{
	char *buffer;
	int sock, ret, i;
	struct sockaddr_in addr = {
		.sin_family = AF_INET,
		.sin_addr = inet_addr(cfg_server_ip),
		.sin_port = htons(SERVER_PORT),
	};

	buffer = mmap(0, BUFFER_SIZE, PROT_READ | PROT_WRITE,
		      MAP_PRIVATE | MAP_ANONYMOUS, 0, 0);
	if (buffer == MAP_FAILED)
		error(1, errno, "mmap() send buffer");

	for (i = 0; i < BUFFER_SIZE; i++)
		buffer[i] = 'a' + (i % 26);

	sock = setup_udp_sock(cfg_client_ip, CLIENT_PORT);

	ret = sendto(sock, buffer, BUFFER_SIZE, 0,
		     (const struct sockaddr *)&addr, sizeof(addr));
	if (ret < 0)
		error(1, errno, "sendto()");

	if (ret != BUFFER_SIZE)
		error(1, 0, "sent buffer is truncated, expected: %d got: %d",
		      BUFFER_SIZE, ret);

	ret = recv(sock, buffer, BUFFER_SIZE, 0);
	if (ret < 0)
		error(1, errno, "recv()");

	if ((ret != sizeof(pass_msg)) || strcmp(buffer, pass_msg))
		error(1, 0, "message mismatches, expected: %s, got: %s",
		      pass_msg, buffer);
}

void print_usage(char *prog)
{
	fprintf(stderr, "Usage: %s (-c|-s) -r<server_ip> -l<client_ip>"
		" -i<server_ifname> [-d] [-z]\n", prog);
}

void parse_opts(int argc, char **argv)
{
	int opt;
	char *ifname = NULL;

	while ((opt = getopt(argc, argv, "hcsr:l:i:dz")) != -1) {
		switch (opt) {
		case 'c':
			if (cfg_server)
				error(1, 0, "Pass one of -s or -c");

			cfg_client = 1;
			break;
		case 's':
			if (cfg_client)
				error(1, 0, "Pass one of -s or -c");

			cfg_server = 1;
			break;
		case 'r':
			cfg_server_ip = optarg;
			break;
		case 'l':
			cfg_client_ip = optarg;
			break;
		case 'i':
			ifname = optarg;
			break;
		case 'd':
			cfg_redirect = 1;
			break;
		case 'z':
			cfg_zerocopy = 1;
			break;
		case 'h':
		default:
			print_usage(argv[0]);
			exit(1);
		}
	}

	if (!cfg_client && !cfg_server)
		error(1, 0, "Pass one of -s or -c");

	if (ifname) {
		cfg_ifindex = if_nametoindex(ifname);
		if (!cfg_ifindex)
			error(1, errno, "Invalid interface %s", ifname);
	}
}

int main(int argc, char **argv)
{
	parse_opts(argc, argv);
	if (cfg_client)
		run_client();
	else if (cfg_server)
		run_server();

	return 0;
}
