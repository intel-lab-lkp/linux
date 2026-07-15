// SPDX-License-Identifier: GPL-2.0
#include <arpa/inet.h>
#include <bpf/libbpf.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "kselftest_harness.h"

#define MAX_ITERATIONS 100
#define PAYLOAD_END 'e'

static int start_listening(struct __test_metadata *_metadata, uint16_t *port)
{
	struct sockaddr_in addr;
	socklen_t addrlen;
	int fd;

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = INADDR_ANY;

	fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

	ASSERT_NE(fd, -1);
	ASSERT_EQ(bind(fd, (struct sockaddr *)&addr, sizeof(addr)), 0);
	ASSERT_EQ(listen(fd, 5), 0);

	addrlen = sizeof(addr);

	ASSERT_EQ(getsockname(fd, (struct sockaddr *)&addr, &addrlen), 0);

	*port = addr.sin_port;

	return fd;
}

static void process_client(int fd, atomic_int *running, bool *failed)
{
	char buf[1024];
	struct timeval timeo;

	timeo.tv_sec = 0;
	timeo.tv_usec = 1000;

	if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeo, sizeof(timeo)))
		*failed = true;

	while (atomic_load(running)) {
		ssize_t len = recvfrom(fd, buf, sizeof(buf), 0, NULL, NULL);

		if (len == -1) {
			if (errno != EAGAIN && errno != EINTR)
				*failed = true;
			continue;
		}

		if (len <= 0 || buf[len - 1] == PAYLOAD_END)
			break;
	}

	send(fd, "test", 4, MSG_NOSIGNAL);

	close(fd);
}

struct bpf_t {
	struct bpf_object *obj;
	struct bpf_link *on_sockops;
	struct bpf_link *on_recv;
};

static void setup_bpf(struct __test_metadata *_metadata, const char *path, bool recv,
						struct bpf_t *bpf)
{
	struct bpf_program *prog;
	int cgroup;

	memset(bpf, 0, sizeof(*bpf));
	bpf->obj = bpf_object__open_file(path, NULL);

	ASSERT_NE(bpf->obj, NULL);
	ASSERT_EQ(bpf_object__load(bpf->obj), 0);

	prog = bpf_object__find_program_by_name(bpf->obj, "on_sockops");
	ASSERT_NE(prog, NULL);

	cgroup = open("/sys/fs/cgroup", O_RDONLY);
	ASSERT_NE(cgroup, -1);

	bpf->on_sockops = bpf_program__attach_cgroup(prog, cgroup);
	close(cgroup);

	ASSERT_NE(bpf->on_sockops, NULL);

	if (recv) {
		struct bpf_map *map = bpf_object__find_map_by_name(bpf->obj, "map_socks");

		ASSERT_NE(map, NULL);

		prog = bpf_object__find_program_by_name(bpf->obj, "on_recv");
		ASSERT_NE(prog, NULL);

		bpf->on_recv = bpf_program__attach_sockmap(prog, bpf_map__fd(map));
		ASSERT_NE(bpf->on_recv, NULL);
	}
}

struct server_t {
	int fd;
	atomic_int running;
	pthread_t thread;
	bool thread_created;
};

static void *run_server(void *arg)
{
	struct server_t *server = arg;
	bool failed = false;

	while (atomic_load(&server->running)) {
		int client_fd = accept(server->fd, NULL, NULL);

		if (client_fd == -1) {
			if (!atomic_load(&server->running))
				break;

			continue;
		}

		process_client(client_fd, &server->running, &failed);
	}

	if (failed)
		return (void *)1;

	return NULL;
}

static int send_payload(struct __test_metadata *_metadata, int fd, const char *buf, size_t len)
{
	size_t remaining = len;

	do {
		ssize_t bytes = write(fd, buf + (len - remaining), remaining);

		if (bytes < 0) {
			if (errno == EINTR)
				continue;

			return -1;
		}

		remaining -= bytes;
	} while (remaining);

	return 0;
}

FIXTURE(sockmap_recvfrom)
{
	struct server_t server;
	struct sockaddr_in addr;
	struct bpf_t bpf;
	char *payload;
	size_t payload_len;
};

FIXTURE_VARIANT(sockmap_recvfrom)
{
	bool with_recv;
};

FIXTURE_VARIANT_ADD(sockmap_recvfrom, recvmsg)
{
	.with_recv = false
};

FIXTURE_VARIANT_ADD(sockmap_recvfrom, recvmsg_parser)
{
	.with_recv = true
};

FIXTURE_SETUP(sockmap_recvfrom)
{
	memset(&self->server, 0, sizeof(self->server));
	self->server.fd = -1;

	memset(&self->addr, 0, sizeof(self->addr));

	self->payload_len = 1024 * 1024 * 25;
	self->payload = malloc(self->payload_len);
	ASSERT_NE(self->payload, NULL);

	memset(self->payload, 0, self->payload_len);
	self->payload[self->payload_len - 1] = PAYLOAD_END;

	setup_bpf(_metadata, "sockmap_recvfrom.bpf.o", variant->with_recv, &self->bpf);
	atomic_store(&self->server.running, 1);
	self->server.fd = start_listening(_metadata, &self->addr.sin_port);

	self->addr.sin_family = AF_INET;
	self->addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

	ASSERT_EQ(pthread_create(&self->server.thread, NULL, &run_server, &self->server), 0);
	self->server.thread_created = true;
}

FIXTURE_TEARDOWN(sockmap_recvfrom)
{
	atomic_store(&self->server.running, 0);

	if (self->server.fd != -1)
		shutdown(self->server.fd, SHUT_RD);

	if (self->server.thread_created) {
		void *retval = NULL;

		pthread_join(self->server.thread, &retval);
		EXPECT_EQ(retval, NULL);
	}

	if (self->server.fd != -1)
		close(self->server.fd);

	free(self->payload);
	bpf_link__destroy(self->bpf.on_sockops);

	if (self->bpf.on_recv)
		bpf_link__destroy(self->bpf.on_recv);

	bpf_object__close(self->bpf.obj);
}

TEST_F(sockmap_recvfrom, no_timeout)
{
	char ignored[128];

	for (int i = 0; i < MAX_ITERATIONS; ++i) {
		int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

		ASSERT_NE(fd, -1);
		ASSERT_EQ(connect(fd, (struct sockaddr *)&self->addr, sizeof(self->addr)), 0);

		ASSERT_EQ(send_payload(_metadata, fd, self->payload, self->payload_len), 0);

		if (recvfrom(fd, ignored, sizeof(ignored), 0, NULL, NULL) < 0)
			ASSERT_NE(errno, EAGAIN);

		close(fd);
	}
}

static int64_t to_nanos(struct timespec *time)
{
	return (time->tv_sec * 1000000000LL) + time->tv_nsec;
}

TEST_F(sockmap_recvfrom, with_timeout)
{
	char ignored[128];
	struct timeval timeo;

	timeo.tv_sec = 0;
	timeo.tv_usec = 5000;

	/* remove the payload end delimiter so the server never responds and recvfrom times out. */
	self->payload[self->payload_len - 1] = 0;

	for (int i = 0; i < MAX_ITERATIONS; ++i) {
		struct timespec beg;
		struct timespec end;
		int err;

		int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

		ASSERT_NE(fd, -1);
		ASSERT_EQ(connect(fd, (struct sockaddr *)&self->addr, sizeof(self->addr)), 0);

		ASSERT_EQ(send_payload(_metadata, fd, self->payload, self->payload_len), 0);

		ASSERT_EQ(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeo, sizeof(timeo)), 0);

		clock_gettime(CLOCK_MONOTONIC, &beg);
		ASSERT_EQ(recvfrom(fd, ignored, sizeof(ignored), 0, NULL, NULL), -1);
		err = errno;
		clock_gettime(CLOCK_MONOTONIC, &end);

		ASSERT_EQ(err, EAGAIN);
		ASSERT_GE(to_nanos(&end) - to_nanos(&beg), timeo.tv_usec * 1000);

		close(fd);
	}
}

TEST_HARNESS_MAIN
