// SPDX-License-Identifier: GPL-2.0
/* Copyright Amazon.com Inc. or its affiliates. */
#define _GNU_SOURCE
#include <sched.h>

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>

#include "../../kselftest_harness.h"

FIXTURE(scm_rights)
{
	int fd[16];
};

FIXTURE_VARIANT(scm_rights)
{
	int type;
	char name[16];
	bool test_listener;
};

FIXTURE_VARIANT_ADD(scm_rights, dgram)
{
	.type = SOCK_DGRAM,
	.name = "UNIX ",
	.test_listener = false,
};

FIXTURE_VARIANT_ADD(scm_rights, stream)
{
	.type = SOCK_STREAM,
	.name = "UNIX-STREAM ",
	.test_listener = false,
};

FIXTURE_VARIANT_ADD(scm_rights, stream_listener)
{
	.type = SOCK_STREAM,
	.name = "UNIX-STREAM ",
	.test_listener = true,
};

static int count_sockets(struct __test_metadata *_metadata,
			 const FIXTURE_VARIANT(scm_rights) *variant)
{
	int sockets = -1, len, ret;
	size_t unused;
	char *line;
	FILE *f;

	f = fopen("/proc/net/protocols", "r");
	ASSERT_NE(NULL, f);

	len = strlen(variant->name);

	while (getline(&line, &unused, f) != -1) {
		int unused2;

		if (strncmp(line, variant->name, len))
			continue;

		ret = sscanf(line + len, "%d %d", &unused2, &sockets);
		ASSERT_EQ(2, ret);

		break;
	}

	ret = fclose(f);
	ASSERT_EQ(0, ret);

	return sockets;
}

FIXTURE_SETUP(scm_rights)
{
	int ret;

	ret = unshare(CLONE_NEWNET);
	ASSERT_EQ(0, ret);

	ret = count_sockets(_metadata, variant);
	ASSERT_EQ(0, ret);
}

FIXTURE_TEARDOWN(scm_rights)
{
	int ret;

	ret = count_sockets(_metadata, variant);
	ASSERT_EQ(0, ret);
}

static void create_listeners(struct __test_metadata *_metadata,
			     FIXTURE_DATA(scm_rights) *self,
			     int n)
{
	struct sockaddr_un addr = {
		.sun_family = AF_UNIX,
	};
	socklen_t addrlen;
	int i, ret;

	for (i = 0; i < n * 2; i += 2) {
		self->fd[i] = socket(AF_UNIX, SOCK_STREAM, 0);
		ASSERT_LE(0, self->fd[i]);

		addrlen = sizeof(addr.sun_family);
		ret = bind(self->fd[i], (struct sockaddr *)&addr, addrlen);
		ASSERT_EQ(0, ret);

		ret = listen(self->fd[i], -1);
		ASSERT_EQ(0, ret);

		addrlen = sizeof(addr);
		ret = getsockname(self->fd[i], (struct sockaddr *)&addr, &addrlen);
		ASSERT_EQ(0, ret);

		self->fd[i + 1] = socket(AF_UNIX, SOCK_STREAM, 0);
		ASSERT_LE(0, self->fd[i + 1]);

		ret = connect(self->fd[i + 1], (struct sockaddr *)&addr, addrlen);
		ASSERT_EQ(0, ret);
	}
}

static void create_socketpairs(struct __test_metadata *_metadata,
			       FIXTURE_DATA(scm_rights) *self,
			       const FIXTURE_VARIANT(scm_rights) *variant,
			       int n)
{
	int i, ret;

	for (i = 0; i < n * 2; i += 2) {
		ret = socketpair(AF_UNIX, variant->type, 0, self->fd + i);
		ASSERT_EQ(0, ret);
	}
}

static void __create_sockets(struct __test_metadata *_metadata,
			     FIXTURE_DATA(scm_rights) *self,
			     const FIXTURE_VARIANT(scm_rights) *variant,
			     int n)
{
	if (variant->test_listener)
		create_listeners(_metadata, self, n);
	else
		create_socketpairs(_metadata, self, variant, n);
}

static void __close_sockets(struct __test_metadata *_metadata,
			    FIXTURE_DATA(scm_rights) *self,
			    int n)
{
	int i, ret;

	for (i = 0; i < n * 2; i++) {
		ret = close(self->fd[i]);
		ASSERT_EQ(0, ret);
	}
}

void __send_fd(struct __test_metadata *_metadata,
	       const FIXTURE_DATA(scm_rights) *self,
	       int inflight, int receiver)
{
#define MSG "nop"
#define MSGLEN 3
	struct {
		struct cmsghdr cmsghdr;
		int fd;
	} cmsg = {
		.cmsghdr = {
			.cmsg_len = CMSG_LEN(sizeof(cmsg.fd)),
			.cmsg_level = SOL_SOCKET,
			.cmsg_type = SCM_RIGHTS,
		},
		.fd = self->fd[inflight * 2],
	};
	struct iovec iov = {
		.iov_base = MSG,
		.iov_len = MSGLEN,
	};
	struct msghdr msg = {
		.msg_name = NULL,
		.msg_namelen = 0,
		.msg_iov = &iov,
		.msg_iovlen = 1,
		.msg_control = &cmsg,
		.msg_controllen = CMSG_SPACE(sizeof(cmsg.fd)),
	};
	int ret;

	ret = sendmsg(self->fd[receiver * 2 + 1], &msg, 0);
	ASSERT_EQ(MSGLEN, ret);
}

#define create_sockets(n)				\
	__create_sockets(_metadata, self, variant, n)
#define close_sockets(n)				\
	__close_sockets(_metadata, self, n)
#define send_fd(inflight, receiver)			\
	__send_fd(_metadata, self, inflight, receiver)

TEST_F(scm_rights, self_ref)
{
	create_sockets(1);

	send_fd(0, 0);

	close_sockets(1);
}

TEST_F(scm_rights, triangle)
{
	create_sockets(3);

	send_fd(0, 1);
	send_fd(1, 2);
	send_fd(2, 0);

	close_sockets(3);
}

TEST_F(scm_rights, cross_edge)
{
	create_sockets(4);

	send_fd(0, 1);
	send_fd(1, 2);
	send_fd(2, 0);
	send_fd(1, 3);
	send_fd(3, 2);

	close_sockets(4);
}

TEST_HARNESS_MAIN
