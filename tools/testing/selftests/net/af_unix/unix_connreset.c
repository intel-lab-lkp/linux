// SPDX-License-Identifier: GPL-2.0
/*
 * Selftest for AF_UNIX socket close and ECONNRESET behaviour.
 *
 * This test verifies that:
 *  1. SOCK_STREAM sockets return EOF when peer closes normally.
 *  2. SOCK_STREAM sockets return ECONNRESET if peer closes with unread data.
 *  3. SOCK_DGRAM sockets do not return ECONNRESET when peer closes.
 *
 * These tests document the intended Linux behaviour.
 *
 */

#define _GNU_SOURCE
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include "../../kselftest_harness.h"

#define SOCK_PATH "/tmp/af_unix_connreset.sock"

static void remove_socket_file(void)
{
	unlink(SOCK_PATH);
}

FIXTURE(unix_sock)
{
	int server;
	int client;
	int child;
};

FIXTURE_VARIANT(unix_sock)
{
	int socket_type;
	const char *name;
};

/* Define variants: stream and datagram */
FIXTURE_VARIANT_ADD(unix_sock, stream) {
	.socket_type = SOCK_STREAM,
	.name = "SOCK_STREAM",
};

FIXTURE_VARIANT_ADD(unix_sock, dgram) {
	.socket_type = SOCK_DGRAM,
	.name = "SOCK_DGRAM",
};

FIXTURE_SETUP(unix_sock)
{
	struct sockaddr_un addr = {};
	int err;

	addr.sun_family = AF_UNIX;
	strcpy(addr.sun_path, SOCK_PATH);

	self->server = socket(AF_UNIX, variant->socket_type, 0);
	ASSERT_LT(-1, self->server);

	err = bind(self->server, (struct sockaddr *)&addr, sizeof(addr));
	ASSERT_EQ(0, err);

	if (variant->socket_type == SOCK_STREAM) {
		err = listen(self->server, 1);
		ASSERT_EQ(0, err);

		self->client = socket(AF_UNIX, SOCK_STREAM, 0);
		ASSERT_LT(-1, self->client);

		err = connect(self->client, (struct sockaddr *)&addr, sizeof(addr));
		ASSERT_EQ(0, err);

		self->child = accept(self->server, NULL, NULL);
		ASSERT_LT(-1, self->child);
	} else {
		/* Datagram: bind and connect only */
		self->client = socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK, 0);
		ASSERT_LT(-1, self->client);

		err = connect(self->client, (struct sockaddr *)&addr, sizeof(addr));
		ASSERT_EQ(0, err);
	}
}

FIXTURE_TEARDOWN(unix_sock)
{
	if (variant->socket_type == SOCK_STREAM)
		close(self->child);

	close(self->client);
	close(self->server);
	remove_socket_file();
}

/* Test 1: peer closes normally */
TEST_F(unix_sock, eof)
{
	char buf[16] = {};
	ssize_t n;

	if (variant->socket_type != SOCK_STREAM)
		SKIP(return, "This test only applies to SOCK_STREAM");

	/* Peer closes normally */
	close(self->child);

	n = recv(self->client, buf, sizeof(buf), 0);
	TH_LOG("%s: recv=%zd errno=%d (%s)", variant->name, n, errno, strerror(errno));
	if (n == -1)
		ASSERT_EQ(ECONNRESET, errno);

	if (n != -1)
		ASSERT_EQ(0, n);
}

/* Test 2: peer closes with unread data */
TEST_F(unix_sock, reset_unread)
{
	char buf[16] = {};
	ssize_t n;

	if (variant->socket_type != SOCK_STREAM)
		SKIP(return, "This test only applies to SOCK_STREAM");

	/* Send data that will remain unread by client */
	send(self->client, "hello", 5, 0);
	close(self->child);

	n = recv(self->client, buf, sizeof(buf), 0);
	TH_LOG("%s: recv=%zd errno=%d (%s)", variant->name, n, errno, strerror(errno));
	ASSERT_EQ(-1, n);
	ASSERT_EQ(ECONNRESET, errno);
}

/* Test 3: SOCK_DGRAM peer close */
TEST_F(unix_sock, dgram_reset)
{
	char buf[16] = {};
	ssize_t n;

	if (variant->socket_type != SOCK_DGRAM)
		SKIP(return, "This test only applies to SOCK_DGRAM");

	send(self->client, "hello", 5, 0);
	close(self->server);

	n = recv(self->client, buf, sizeof(buf), 0);
	TH_LOG("%s: recv=%zd errno=%d (%s)", variant->name, n, errno, strerror(errno));
	/* Expect EAGAIN because there is no datagram and peer is closed. */
	ASSERT_EQ(-1, n);
	ASSERT_EQ(EAGAIN, errno);
}

TEST_HARNESS_MAIN

