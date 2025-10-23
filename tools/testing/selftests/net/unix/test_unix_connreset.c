// SPDX-License-Identifier: GPL-2.0
/*
 * Selftest for UNIX socket close and ECONNRESET behaviour.
 *
 * This test verifies that:
 *  1. SOCK_STREAM sockets return EOF when peer closes normally.
 *  2. SOCK_STREAM sockets return ECONNRESET if peer closes with unread data.
 *  3. SOCK_DGRAM sockets do not return ECONNRESET when peer closes,
 *     unlike BSD where this error is observed.
 *
 * These tests document the intended Linux behaviour, distinguishing it from BSD.
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

#define SOCK_PATH "/tmp/test_unix_connreset.sock"

static void remove_socket_file(void)
{
	unlink(SOCK_PATH);
}

/* Test 1: peer closes normally */
TEST(stream_eof)
{
	int server, client, child;
	struct sockaddr_un addr = {0};
	char buf[16] = {0};
	ssize_t n;

	server = socket(AF_UNIX, SOCK_STREAM, 0);
	ASSERT_GE(server, 0);

	addr.sun_family = AF_UNIX;
	strcpy(addr.sun_path, SOCK_PATH);
	remove_socket_file();

	ASSERT_EQ(bind(server, (struct sockaddr *)&addr, sizeof(addr)), 0);
	ASSERT_EQ(listen(server, 1), 0);

	client = socket(AF_UNIX, SOCK_STREAM, 0);
	ASSERT_GE(client, 0);
	ASSERT_EQ(connect(client, (struct sockaddr *)&addr, sizeof(addr)), 0);

	child = accept(server, NULL, NULL);
	ASSERT_GE(child, 0);

	/* Peer closes normally */
	close(child);

	n = recv(client, buf, sizeof(buf), 0);
	EXPECT_EQ(n, 0);
	TH_LOG("recv=%zd errno=%d (%s)", n, errno, strerror(errno));

	close(client);
	close(server);
	remove_socket_file();
}

/* Test 2: peer closes with unread data */
TEST(stream_reset_unread)
{
	int server, client, child;
	struct sockaddr_un addr = {0};
	char buf[16] = {0};
	ssize_t n;

	server = socket(AF_UNIX, SOCK_STREAM, 0);
	ASSERT_GE(server, 0);

	addr.sun_family = AF_UNIX;
	strcpy(addr.sun_path, SOCK_PATH);
	remove_socket_file();

	ASSERT_EQ(bind(server, (struct sockaddr *)&addr, sizeof(addr)), 0);
	ASSERT_EQ(listen(server, 1), 0);

	client = socket(AF_UNIX, SOCK_STREAM, 0);
	ASSERT_GE(client, 0);
	ASSERT_EQ(connect(client, (struct sockaddr *)&addr, sizeof(addr)), 0);

	child = accept(server, NULL, NULL);
	ASSERT_GE(child, 0);

	/* Send data that will remain unread by client */
	send(client, "hello", 5, 0);
	close(child);

	n = recv(client, buf, sizeof(buf), 0);
	EXPECT_LT(n, 0);
	EXPECT_EQ(errno, ECONNRESET);
	TH_LOG("recv=%zd errno=%d (%s)", n, errno, strerror(errno));

	close(client);
	close(server);
	remove_socket_file();
}

/* Test 3: SOCK_DGRAM peer close */
TEST(dgram_reset)
{
	int server, client;
	int flags;
	struct sockaddr_un addr = {0};
	char buf[16] = {0};
	ssize_t n;

	server = socket(AF_UNIX, SOCK_DGRAM, 0);
	ASSERT_GE(server, 0);

	addr.sun_family = AF_UNIX;
	strcpy(addr.sun_path, SOCK_PATH);
	remove_socket_file();

	ASSERT_EQ(bind(server, (struct sockaddr *)&addr, sizeof(addr)), 0);

	client = socket(AF_UNIX, SOCK_DGRAM, 0);
	ASSERT_GE(client, 0);
	ASSERT_EQ(connect(client, (struct sockaddr *)&addr, sizeof(addr)), 0);

	send(client, "hello", 5, 0);
	close(server);

	flags = fcntl(client, F_GETFL, 0);
	fcntl(client, F_SETFL, flags | O_NONBLOCK);

	n = recv(client, buf, sizeof(buf), 0);
	TH_LOG("recv=%zd errno=%d (%s)", n, errno, strerror(errno));
	/* Expect EAGAIN or EWOULDBLOCK because there is no datagram and peer is closed. */
	EXPECT_LT(n, 0);
	EXPECT_TRUE(errno == EAGAIN);

	close(client);
	remove_socket_file();
}

TEST_HARNESS_MAIN

