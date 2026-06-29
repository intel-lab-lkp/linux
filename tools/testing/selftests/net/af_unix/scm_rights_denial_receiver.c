// SPDX-License-Identifier: GPL-2.0

/*
 * scm_rights_denial_receiver.c - Receive fds over a Unix socket via SCM_RIGHTS
 *
 * Usage: ./scm_rights_denial_receiver <socket_path>
 *
 * Listens on the given Unix socket path, accepts a connection, and
 * attempts to receive file descriptors via SCM_RIGHTS. Reports
 * whether the fds were delivered or blocked.
 *
 * Used for testing LSM (Smack) blocking of fd passing.
 */

#include <sys/socket.h>
#include <sys/un.h>
#include <sys/xattr.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>

#include "scm_rights_denial.h"

#define RECV_LOG(fmt, ...) printf("receiver: " fmt, ##__VA_ARGS__)
#define RECV_ERR(fmt, ...) fprintf(stderr, "receiver: " fmt, ##__VA_ARGS__)

static int recv_fds(int sock, int *fds)
{
	char buf[1];
	char ctrl[CMSG_SPACE(MAX_FDS * sizeof(int))];

	struct iovec iov = {
		.iov_base = buf,
		.iov_len  = sizeof(buf),
	};
	struct msghdr msg = {
		.msg_iov        = &iov,
		.msg_iovlen     = 1,
		.msg_control    = ctrl,
		.msg_controllen = sizeof(ctrl),
	};

	ssize_t bytes_read = recvmsg(sock, &msg, 0);
	if (bytes_read < 0) {
		perror("receiver: recvmsg");
		return -1;
	}
	if (bytes_read == 0) {
		RECV_ERR("connection closed, no data received\n");
		return -1;
	}

	struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
	if (!CMSG_IS_SCM_RIGHTS(cmsg)) {
		RECV_ERR("no SCM_RIGHTS in control message\n");
		return -1;
	}

	int num_fd_slots = (cmsg->cmsg_len - CMSG_LEN(0)) / sizeof(int);
	memcpy(fds, CMSG_DATA(cmsg), num_fd_slots  * sizeof(int));

	RECV_LOG("got %d fd slot(s):", num_fd_slots);
	for (int i = 0; i < num_fd_slots ; i++) {
		if (fds[i] < 0)
			printf(" %s", strerrorname_np(-fds[i]));
		else
			printf(" %d", fds[i]);
	}
	putchar('\n');

	return num_fd_slots;
}

static inline int print_current_label(void)
{
	char label[256];
	if (!read_current_label(label, sizeof(label))) {
		RECV_LOG("running with Smack label '%s'\n", label);
		return 0;
	}
	return -1;
}

int main(int argc, char *argv[])
{
	if (argc != 2) {
		fprintf(stderr, "Usage: %s <socket_path>\n", argv[0]);
		return -1;
	}

	if (print_current_label()) {
		RECV_ERR("cannot read process Smack label");
		return -1;
	}

	int listen_sock = socket(AF_UNIX, SOCK_STREAM, 0);
	if (listen_sock < 0) {
		perror("receiver: socket");
		return -1;
	}

	struct sockaddr_un addr = {};
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, argv[1], sizeof(addr.sun_path) - 1);

	/* Remove any stale socket file */
	unlink(argv[1]);

	if (bind(listen_sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		perror("receiver: bind");
		return -1;
	}

	if (listen(listen_sock, 1) < 0) {
		perror("receiver: listen");
		return -1;
	}

	RECV_LOG("listening on '%s'\n", argv[1]);

	int conn_sock = accept(listen_sock, NULL, NULL);
	if (conn_sock < 0) {
		perror("receiver: accept");
		return -1;
	}

	RECV_LOG("connection accepted\n");

	int one = 1;
	if (setsockopt(conn_sock, SOL_SOCKET, SO_RIGHTS_NOTRUNC,
		       &one, sizeof(one)) < 0) {
		perror("receiver: setsockopt(SO_RIGHTS_NOTRUNC)");
		goto out_sock;
	}

	/* Try to receive the fds */
	int fds[MAX_FDS];
	int num_fds = recv_fds(conn_sock, fds);
	if (num_fds < 0)
		goto out_sock;

	/* Try to use the received fds -- read and print their contents */
	RECV_LOG("attempting to read from received fds...\n");
	int i;
	for (i = 0; i < num_fds; ++i) {
		char readbuf[256];

		if (fds[i] < 0) {
			RECV_LOG("fd in position %i blocked\n", i);
			continue;
		} else if (fds[i] == 0) {
			RECV_LOG("bad fd in position %i\n", i);
			goto out_recv;
		}

		ssize_t n = read(fds[i], readbuf, sizeof(readbuf) - 1);
		if (n < 0) {
			perror("receiver: read from received fd");
			goto out_recv;
		}

		readbuf[n] = '\0';
		RECV_LOG("read %zd bytes from fd at position %i: '%s'\n", n, i, readbuf);
	}

	RECV_LOG("final result:\n");
	for (int j = 0; j < num_fds; ++j) {
		if (fds[j] < 0) {
			printf("BLOCKED");
		} else {
			printf("PASSED");
			close(fds[j]);
		}
		putchar(' ');
	}

	close(conn_sock);
	close(listen_sock);
	unlink(argv[1]);
	return 0;

out_recv:
	for (int j = 0; j < num_fds; ++j) {
		if (fds[j] > 0)
			close(fds[j]);
	}

out_sock:
	close(conn_sock);
	close(listen_sock);
	unlink(argv[1]);
	return -1;
}
