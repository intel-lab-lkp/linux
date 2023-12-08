// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE
#define __EXPORTED_HEADERS__

#include <linux/uio.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#define __iovec_defined
#include <fcntl.h>
#include <malloc.h>
#include <error.h>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>

#include <linux/memfd.h>
#include <linux/if.h>
#include <linux/dma-buf.h>
#include <linux/udmabuf.h>
#include <libmnl/libmnl.h>
#include <linux/types.h>
#include <linux/netlink.h>
#include <linux/genetlink.h>
#include <linux/netdev.h>
#include <time.h>

#include "netdev-user.h"
#include <ynl.h>

#define PAGE_SHIFT 12
#define TEST_PREFIX "ncdevmem"
#define NUM_PAGES 16000

#ifndef MSG_SOCK_DEVMEM
#define MSG_SOCK_DEVMEM 0x2000000
#endif

/*
 * tcpdevmem netcat. Works similarly to netcat but does device memory TCP
 * instead of regular TCP. Uses udmabuf to mock a dmabuf provider.
 *
 * Usage:
 *
 *	On server:
 *	ncdevmem -s <server IP> -c <client IP> -f eth1 -d 3 -n 0000:06:00.0 -l \
 *		-p 5201 -v 7
 *
 *	On client:
 *	yes $(echo -e \\x01\\x02\\x03\\x04\\x05\\x06) | \
 *		tr \\n \\0 | \
 *		head -c 5G | \
 *		nc <server IP> 5201 -p 5201
 *
 * Note this is compatible with regular netcat. i.e. the sender or receiver can
 * be replaced with regular netcat to test the RX or TX path in isolation.
 */

static char *server_ip = "192.168.1.4";
static char *client_ip = "192.168.1.2";
static char *port = "5201";
static size_t do_validation;
static int queue_num = 15;
static char *ifname = "eth1";
static unsigned int ifindex = 3;
static char *nic_pci_addr = "0000:06:00.0";
static unsigned int iterations;
static unsigned int dmabuf_id;

void print_bytes(void *ptr, size_t size)
{
	unsigned char *p = ptr;
	int i;

	for (i = 0; i < size; i++)
		printf("%02hhX ", p[i]);
	printf("\n");
}

void print_nonzero_bytes(void *ptr, size_t size)
{
	unsigned char *p = ptr;
	unsigned int i;

	for (i = 0; i < size; i++)
		putchar(p[i]);
	printf("\n");
}

void validate_buffer(void *line, size_t size)
{
	static unsigned char seed = 1;
	unsigned char *ptr = line;
	int errors = 0;
	size_t i;

	for (i = 0; i < size; i++) {
		if (ptr[i] != seed) {
			fprintf(stderr,
				"Failed validation: expected=%u, actual=%u, index=%lu\n",
				seed, ptr[i], i);
			errors++;
			if (errors > 20)
				error(1, 0, "validation failed.");
		}
		seed++;
		if (seed == do_validation)
			seed = 0;
	}

	fprintf(stdout, "Validated buffer\n");
}

static void reset_flow_steering(void)
{
	char command[256];

	memset(command, 0, sizeof(command));
	snprintf(command, sizeof(command), "sudo ethtool -K %s ntuple off",
		 "eth1");
	system(command);

	memset(command, 0, sizeof(command));
	snprintf(command, sizeof(command), "sudo ethtool -K %s ntuple on",
		 "eth1");
	system(command);
}

static void configure_flow_steering(void)
{
	char command[256];

	memset(command, 0, sizeof(command));
	snprintf(command, sizeof(command),
		 "sudo ethtool -N %s flow-type tcp4 src-ip %s dst-ip %s src-port %s dst-port %s queue %d",
		 ifname, client_ip, server_ip, port, port, queue_num);
	system(command);
}

static int bind_rx_queue(unsigned int ifindex, unsigned int dmabuf_fd,
			 struct netdev_queue_dmabuf *queues,
			 unsigned int n_queue_index, struct ynl_sock **ys)
{
	struct netdev_bind_rx_req *req = NULL;
	struct netdev_bind_rx_rsp *rsp = NULL;
	struct ynl_error yerr;

	*ys = ynl_sock_create(&ynl_netdev_family, &yerr);
	if (!*ys) {
		fprintf(stderr, "YNL: %s\n", yerr.msg);
		return -1;
	}

	req = netdev_bind_rx_req_alloc();
	netdev_bind_rx_req_set_ifindex(req, ifindex);
	netdev_bind_rx_req_set_dmabuf_fd(req, dmabuf_fd);
	__netdev_bind_rx_req_set_queues(req, queues, n_queue_index);

	rsp = netdev_bind_rx(*ys, req);
	if (!rsp) {
		perror("netdev_bind_rx");
		goto err_close;
	}

	if (!rsp->_present.dmabuf_id) {
		perror("dmabuf_id not present");
		goto err_close;
	}

	printf("got dmabuf id=%d\n", rsp->dmabuf_id);
	dmabuf_id = rsp->dmabuf_id;

	netdev_bind_rx_req_free(req);
	netdev_bind_rx_rsp_free(rsp);

	return 0;

err_close:
	fprintf(stderr, "YNL failed: %s\n", (*ys)->err.msg);
	netdev_bind_rx_req_free(req);
	ynl_sock_destroy(*ys);
	return -1;
}

static void create_udmabuf(int *devfd, int *memfd, int *buf, size_t dmabuf_size)
{
	struct udmabuf_create create;
	int ret;

	*devfd = open("/dev/udmabuf", O_RDWR);
	if (*devfd < 0) {
		error(70, 0,
		      "%s: [skip,no-udmabuf: Unable to access DMA buffer device file]\n",
		      TEST_PREFIX);
	}

	*memfd = memfd_create("udmabuf-test", MFD_ALLOW_SEALING);
	if (*memfd < 0)
		error(70, 0, "%s: [skip,no-memfd]\n", TEST_PREFIX);

	/* Required for udmabuf */
	ret = fcntl(*memfd, F_ADD_SEALS, F_SEAL_SHRINK);
	if (ret < 0)
		error(73, 0, "%s: [skip,fcntl-add-seals]\n", TEST_PREFIX);

	ret = ftruncate(*memfd, dmabuf_size);
	if (ret == -1)
		error(74, 0, "%s: [FAIL,memfd-truncate]\n", TEST_PREFIX);

	memset(&create, 0, sizeof(create));

	create.memfd = *memfd;
	create.offset = 0;
	create.size = dmabuf_size;
	*buf = ioctl(*devfd, UDMABUF_CREATE, &create);
	if (*buf < 0)
		error(75, 0, "%s: [FAIL, create udmabuf]\n", TEST_PREFIX);
}

int do_server(void)
{
	char ctrl_data[sizeof(int) * 20000];
	struct netdev_queue_dmabuf *queues;
	size_t non_page_aligned_frags = 0;
	struct sockaddr_in client_addr;
	struct sockaddr_in server_sin;
	size_t page_aligned_frags = 0;
	int devfd, memfd, buf, ret;
	size_t total_received = 0;
	socklen_t client_addr_len;
	bool is_devmem = false;
	char *buf_mem = NULL;
	struct ynl_sock *ys;
	size_t dmabuf_size;
	char iobuf[819200];
	char buffer[256];
	int socket_fd;
	int client_fd;
	size_t i = 0;
	int opt = 1;

	dmabuf_size = getpagesize() * NUM_PAGES;

	create_udmabuf(&devfd, &memfd, &buf, dmabuf_size);

	reset_flow_steering();
	configure_flow_steering();

	sleep(1);

	queues = malloc(sizeof(*queues) * 1);

	queues[0]._present.type = 1;
	queues[0]._present.idx = 1;
	queues[0].type = NETDEV_QUEUE_TYPE_RX;
	queues[0].idx = queue_num;
	if (bind_rx_queue(ifindex, buf, queues, 1, &ys))
		error(1, 0, "Failed to bind\n");

	buf_mem = mmap(NULL, dmabuf_size, PROT_READ | PROT_WRITE, MAP_SHARED,
		       buf, 0);
	if (buf_mem == MAP_FAILED)
		error(1, 0, "mmap()");

	server_sin.sin_family = AF_INET;
	server_sin.sin_port = htons(atoi(port));

	ret = inet_pton(server_sin.sin_family, server_ip, &server_sin.sin_addr);
	if (socket < 0)
		error(79, 0, "%s: [FAIL, create socket]\n", TEST_PREFIX);

	socket_fd = socket(server_sin.sin_family, SOCK_STREAM, 0);
	if (socket < 0)
		error(errno, errno, "%s: [FAIL, create socket]\n", TEST_PREFIX);

	ret = setsockopt(socket_fd, SOL_SOCKET, SO_REUSEPORT, &opt,
			 sizeof(opt));
	if (ret)
		error(errno, errno, "%s: [FAIL, set sock opt]\n", TEST_PREFIX);

	ret = setsockopt(socket_fd, SOL_SOCKET, SO_REUSEADDR, &opt,
			 sizeof(opt));
	if (ret)
		error(errno, errno, "%s: [FAIL, set sock opt]\n", TEST_PREFIX);

	printf("binding to address %s:%d\n", server_ip,
	       ntohs(server_sin.sin_port));

	ret = bind(socket_fd, &server_sin, sizeof(server_sin));
	if (ret)
		error(errno, errno, "%s: [FAIL, bind]\n", TEST_PREFIX);

	ret = listen(socket_fd, 1);
	if (ret)
		error(errno, errno, "%s: [FAIL, listen]\n", TEST_PREFIX);

	client_addr_len = sizeof(client_addr);

	inet_ntop(server_sin.sin_family, &server_sin.sin_addr, buffer,
		  sizeof(buffer));
	printf("Waiting or connection on %s:%d\n", buffer,
	       ntohs(server_sin.sin_port));
	client_fd = accept(socket_fd, &client_addr, &client_addr_len);

	inet_ntop(client_addr.sin_family, &client_addr.sin_addr, buffer,
		  sizeof(buffer));
	printf("Got connection from %s:%d\n", buffer,
	       ntohs(client_addr.sin_port));

	while (1) {
		struct iovec iov = { .iov_base = iobuf,
				     .iov_len = sizeof(iobuf) };
		struct dmabuf_cmsg *dmabuf_cmsg = NULL;
		struct dma_buf_sync sync = { 0 };
		struct cmsghdr *cm = NULL;
		struct msghdr msg = { 0 };
		struct dmabuf_token token;
		ssize_t ret;

		is_devmem = false;
		printf("\n\n");

		msg.msg_iov = &iov;
		msg.msg_iovlen = 1;
		msg.msg_control = ctrl_data;
		msg.msg_controllen = sizeof(ctrl_data);
		ret = recvmsg(client_fd, &msg, MSG_SOCK_DEVMEM);
		printf("recvmsg ret=%ld\n", ret);
		if (ret < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
			continue;
		if (ret < 0) {
			perror("recvmsg");
			continue;
		}
		if (ret == 0) {
			printf("client exited\n");
			goto cleanup;
		}

		i++;
		for (cm = CMSG_FIRSTHDR(&msg); cm; cm = CMSG_NXTHDR(&msg, cm)) {
			if (cm->cmsg_level != SOL_SOCKET ||
			    (cm->cmsg_type != SCM_DEVMEM_DMABUF &&
			     cm->cmsg_type != SCM_DEVMEM_LINEAR)) {
				fprintf(stdout, "skipping non-devmem cmsg\n");
				continue;
			}

			dmabuf_cmsg = (struct dmabuf_cmsg *)CMSG_DATA(cm);
			is_devmem = true;

			if (cm->cmsg_type == SCM_DEVMEM_LINEAR) {
				/* TODO: process data copied from skb's linear
				 * buffer.
				 */
				fprintf(stdout,
					"SCM_DEVMEM_LINEAR. dmabuf_cmsg->frag_size=%u\n",
					dmabuf_cmsg->frag_size);

				continue;
			}

			token.token_start = dmabuf_cmsg->frag_token;
			token.token_count = 1;

			total_received += dmabuf_cmsg->frag_size;
			printf("received frag_page=%llu, in_page_offset=%llu, frag_offset=%llu, frag_size=%u, token=%u, total_received=%lu, dmabuf_id=%u\n",
			       dmabuf_cmsg->frag_offset >> PAGE_SHIFT,
			       dmabuf_cmsg->frag_offset % getpagesize(),
			       dmabuf_cmsg->frag_offset, dmabuf_cmsg->frag_size,
			       dmabuf_cmsg->frag_token, total_received,
			       dmabuf_cmsg->dmabuf_id);

			if (dmabuf_cmsg->dmabuf_id != dmabuf_id)
				error(1, 0,
				      "received on wrong dmabuf_id: flow steering error\n");

			if (dmabuf_cmsg->frag_size % getpagesize())
				non_page_aligned_frags++;
			else
				page_aligned_frags++;

			sync.flags = DMA_BUF_SYNC_READ | DMA_BUF_SYNC_START;
			ioctl(buf, DMA_BUF_IOCTL_SYNC, &sync);

			if (do_validation)
				validate_buffer(
					((unsigned char *)buf_mem) +
						dmabuf_cmsg->frag_offset,
					dmabuf_cmsg->frag_size);
			else
				print_nonzero_bytes(
					((unsigned char *)buf_mem) +
						dmabuf_cmsg->frag_offset,
					dmabuf_cmsg->frag_size);

			sync.flags = DMA_BUF_SYNC_READ | DMA_BUF_SYNC_END;
			ioctl(buf, DMA_BUF_IOCTL_SYNC, &sync);

			ret = setsockopt(client_fd, SOL_SOCKET,
					 SO_DEVMEM_DONTNEED, &token,
					 sizeof(token));
			if (ret != 1)
				error(1, 0,
				      "SO_DEVMEM_DONTNEED not enough tokens");
		}
		if (!is_devmem)
			error(1, 0, "flow steering error\n");

		printf("total_received=%lu\n", total_received);
	}

	fprintf(stdout, "%s: ok\n", TEST_PREFIX);

	fprintf(stdout, "page_aligned_frags=%lu, non_page_aligned_frags=%lu\n",
		page_aligned_frags, non_page_aligned_frags);

	fprintf(stdout, "page_aligned_frags=%lu, non_page_aligned_frags=%lu\n",
		page_aligned_frags, non_page_aligned_frags);

cleanup:

	munmap(buf_mem, dmabuf_size);
	close(client_fd);
	close(socket_fd);
	close(buf);
	close(memfd);
	close(devfd);
	ynl_sock_destroy(ys);

	return 0;
}

int main(int argc, char *argv[])
{
	int is_server = 0, opt;

	while ((opt = getopt(argc, argv, "ls:c:p:v:q:f:n:i:d:")) != -1) {
		switch (opt) {
		case 'l':
			is_server = 1;
			break;
		case 's':
			server_ip = optarg;
			break;
		case 'c':
			client_ip = optarg;
			break;
		case 'p':
			port = optarg;
			break;
		case 'v':
			do_validation = atoll(optarg);
			break;
		case 'q':
			queue_num = atoi(optarg);
			break;
		case 'f':
			ifname = optarg;
			break;
		case 'd':
			ifindex = atoi(optarg);
			break;
		case 'n':
			nic_pci_addr = optarg;
			break;
		case 'i':
			iterations = atoll(optarg);
			break;
		case '?':
			printf("unknown option: %c\n", optopt);
			break;
		}
	}

	for (; optind < argc; optind++)
		printf("extra arguments: %s\n", argv[optind]);

	if (is_server)
		return do_server();

	return 0;
}
