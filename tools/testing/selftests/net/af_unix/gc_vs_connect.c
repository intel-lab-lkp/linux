// SPDX-License-Identifier: GPL-2.0

#define _GNU_SOURCE
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <stdlib.h>
#include <pthread.h>
#include <sys/un.h>
#include <sys/socket.h>

#define SOCK_TYPE	SOCK_STREAM	/* or SOCK_SEQPACKET */

union {
	char buf[CMSG_SPACE(sizeof(int))];
	struct cmsghdr align;
} cbuf;

struct iovec io = {
	.iov_base = (char[1]) {0},
	.iov_len = 1
};

struct msghdr msg = {
	.msg_iov = &io,
	.msg_iovlen = 1,
	.msg_control = cbuf.buf,
	.msg_controllen = sizeof(cbuf.buf)
};

pthread_barrier_t barr;
struct sockaddr_un saddr;
int salen, client;

static void barrier(void)
{
	int ret = pthread_barrier_wait(&barr);

	assert(!ret || ret == PTHREAD_BARRIER_SERIAL_THREAD);
}

static int socket_unix(void)
{
	int sock = socket(AF_UNIX, SOCK_TYPE, 0);

	assert(sock != -1);
	return sock;
}

static int recv_fd(int socket)
{
	struct cmsghdr *cmsg;
	int ret, fd;

	ret = recvmsg(socket, &msg, 0);
	assert(ret == 1 && !(msg.msg_flags & MSG_CTRUNC));

	cmsg = CMSG_FIRSTHDR(&msg);
	assert(cmsg);
	memcpy(&fd, CMSG_DATA(cmsg), sizeof(fd));
	assert(fd >= 0);

	return fd;
}

static void send_fd(int socket, int fd)
{
	struct cmsghdr *cmsg;
	int ret;

	cmsg = CMSG_FIRSTHDR(&msg);
	assert(cmsg);
	cmsg->cmsg_level = SOL_SOCKET;
	cmsg->cmsg_type = SCM_RIGHTS;
	cmsg->cmsg_len = CMSG_LEN(sizeof(fd));

	memcpy(CMSG_DATA(cmsg), &fd, sizeof(fd));

	do {
		ret = sendmsg(socket, &msg, 0);
		assert(ret == 1 || (ret == -1 && errno == ENOTCONN));
	} while (ret != 1);
}

static void *racer_connect(void *arg)
{
	for (;;) {
		int ret;

		barrier();
		ret = connect(client, (struct sockaddr *)&saddr, salen);
		assert(!ret);
		barrier();
	}

	return NULL;
}

static void *racer_gc(void *arg)
{
	for (;;)
		close(socket_unix()); /* trigger GC */

	return NULL;
}

int main(void)
{
	pthread_t thread_conn, thread_gc;
	int ret, pair[2];

	printf("running\n");

	ret = pthread_barrier_init(&barr, NULL, 2);
	assert(!ret);

	ret = pthread_create(&thread_conn, NULL, racer_connect, NULL);
	assert(!ret);

	ret = pthread_create(&thread_gc, NULL, racer_gc, NULL);
	assert(!ret);

	ret = socketpair(AF_UNIX, SOCK_TYPE, 0, pair);
	assert(!ret);

	saddr.sun_family = AF_UNIX;
	salen = sizeof(saddr.sun_family) +
		sprintf(saddr.sun_path, "%c/unix-gc-%d", '\0', getpid());

	for (;;) {
		int server, victim;

		server = socket_unix();
		ret = bind(server, (struct sockaddr *)&saddr, salen);
		assert(!ret);
		ret = listen(server, -1);
		assert(!ret);

		send_fd(pair[0], server);
		close(server);

		client = socket_unix();
		victim = socket_unix();

		barrier();
		send_fd(client, victim);
		close(victim);
		barrier();

		server = recv_fd(pair[1]);
		close(client);
		close(server);
	}

	return 0;
}
