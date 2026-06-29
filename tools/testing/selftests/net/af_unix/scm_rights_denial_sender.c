// SPDX-License-Identifier: GPL-2.0-only

/*
 * scm_rights_denial_sender.c - Send fds over a Unix socket via SCM_RIGHTS
 *
 * Usage: ./scm_rights_denial_sender <socket_path> <file_to_send> [<file_to_send>...]
 *
 * Opens the specified files and sends their fds to a receiver connected
 * on the given Unix socket path. Used for testing LSM blocking of fd
 * passing.
 */

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>

#include "scm_rights_denial.h"

#define SEND_LOG(fmt, ...) fprintf(stdout, "sender: " fmt, ##__VA_ARGS__)
#define SEND_ERR(fmt, ...) fprintf(stderr, "sender: " fmt, ##__VA_ARGS__)

static int send_fds(int sock, int *fds, int num_fds)
{
	if (num_fds > MAX_FDS)
		return -1;

	char buf[1] = { 'X' };
	char ctrl[CMSG_SPACE(MAX_FDS * sizeof(int))] = { 0 };

	struct iovec iov = {
		.iov_base = buf,
		.iov_len  = sizeof(buf),
	};
	struct msghdr msg = {
		.msg_iov        = &iov,
		.msg_iovlen     = 1,
		.msg_control    = ctrl,
		.msg_controllen = CMSG_SPACE(num_fds * sizeof(int)),
	};

	struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
	cmsg->cmsg_level = SOL_SOCKET;
	cmsg->cmsg_type  = SCM_RIGHTS;
	cmsg->cmsg_len   = CMSG_LEN(num_fds * sizeof(int));
	memcpy(CMSG_DATA(cmsg), fds, num_fds * sizeof(int));

	ssize_t bytes_send = sendmsg(sock, &msg, 0);
	if (bytes_send < 0) {
		perror("sender: sendmsg");
		return -1;
	}

	return 0;
}

static inline int print_current_label(void)
{
	char label[256];
	if (!read_current_label(label, sizeof(label))) {
		SEND_LOG("running with Smack label '%s'\n", label);
		return 0;
	}
	return -1;
}

int main(int argc, char *argv[])
{
	if (argc < 3 || argc > 2 + MAX_FDS) {
		fprintf(stderr, "Usage: %s <socket_path> <file_to_send> [<file_to_send>...]\\n",
			argv[0]);
		fprintf(stderr, "Up to a maximum of %d files", MAX_FDS);
		return -1;
	}

	if (print_current_label()) {
		SEND_ERR("cannot read process Smack label");
		return -1;
	}

	int sock = socket(AF_UNIX, SOCK_STREAM, 0);
	if (sock < 0) {
		perror("sender: socket");
		return -1;
	}

	struct sockaddr_un addr = {};
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, argv[1], sizeof(addr.sun_path) - 1);

	if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		perror("sender: connect");
		goto out_sock;
	}

	SEND_LOG("connected to '%s'\n", argv[1]);

	int num_files = argc - 2;
	int fds[MAX_FDS];
	int i;
	for (i = 0; i < num_files; i++) {
		fds[i] = open(argv[2 + i], O_RDONLY);
		if (fds[i] < 0) {
			perror("sender: open file");
			goto out_opened;
		}
		SEND_LOG("opened '%s' as fd %d\n", argv[2 + i], fds[i]);
	}

	if (send_fds(sock, fds, num_files) < 0)
		goto out_opened;

	SEND_LOG("fds successfully sent:");
	for (int j = 0; j < num_files; j++)
		printf(" %d", fds[j]);
	putchar('\n');

out_opened:
	for (int j = 0; j < i; j++)
		close(fds[j]);
out_sock:
	close(sock);
	return -1;
}
