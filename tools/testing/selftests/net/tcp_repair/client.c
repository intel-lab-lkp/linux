// SPDX-License-Identifier: GPL-2.0-only

/* selftests/net/tcp_repair: TCP_REPAIR connection tests
 *
 * client.c - Run list of tests, send commands and data to server
 *
 * Copyright (c) 2026 Red Hat GmbH
 *
 * Author: Stefano Brivio <sbrivio@redhat.com>
 */

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>

#include <linux/tcp.h>		/* latest and greatest struct tcp_info, but */
#define SOL_TCP		6	/* we can't include netinet/tcp.h as a result */

#include "talk.h"

/**
 * srv() - Send command to server, return received value (not for ACCEPT)
 * @ctl:	Control socket
 * @op:		Command type
 * @arg:	Optional argument (always sent, might be zero)
 *
 * Return: integer value received by client as response
 */
int srv(int ctl, enum op op, int arg)
{
	int cmd[2] = { op, arg }, ret;

	send(ctl, cmd, sizeof(cmd), 0);
	if (op != ACCEPT)
		recv(ctl, &ret, sizeof(ret), 0);

	return ret;
}

/**
 * test_seq_slow_path() - Sequence doesn't change after sending one byte
 * @ctl:	Control socket
 * @data:	Data socket
 *
 * Return: 0 if the test passes, -1 if it fails
 */
int test_seq_slow_path(int ctl, int data)
{
	uint32_t seq1, seq2;

	(void)ctl;
	(void)data;

	srv(ctl, REPAIR, TCP_REPAIR_ON);
	seq1 = (uint32_t)srv(ctl, DUMP_RECV_SEQ, 0);

	send(data, (char *)("a"), 1, 0);

	seq2 = (uint32_t)srv(ctl, DUMP_RECV_SEQ, 0);

	if (seq1 != seq2) {
		fprintf(stderr, "Sequence changed in repair mode, %u -> %u\n",
			seq1, seq2);
		return -1;
	}

	return 0;
}

/**
 * test_seq_fast_path() - Sequence doesn't change after a large transfer
 * @ctl:	Control socket
 * @data:	Data socket
 *
 * Return: 0 if the test passes, -1 if it fails
 */
int test_seq_fast_path(int ctl, int data)
{
	char buf[1000] = { 0 };
	uint32_t seq1, seq2;
	int i;

	(void)ctl;
	(void)data;

	for (i = 0; i < 50; i++) {
		send(data, buf, sizeof(buf), 0);
		srv(ctl, RECV, sizeof(buf));
	}

	srv(ctl, REPAIR, TCP_REPAIR_ON);
	seq1 = (uint32_t)srv(ctl, DUMP_RECV_SEQ, 0);

	fcntl(data, F_SETFL, O_NONBLOCK);
	for (i = 0; i < 50; i++)
		send(data, buf, sizeof(buf), 0);

	seq2 = (uint32_t)srv(ctl, DUMP_RECV_SEQ, 0);

	if (seq1 != seq2) {
		fprintf(stderr, "Sequence changed in repair mode, %u -> %u\n",
			seq1, seq2);
		return -1;
	}

	return 0;
}

/**
 * test_acked_slow_path() - Our ACK sequence doesn't change after sending byte
 * @ctl:	Control socket
 * @data:	Data socket
 *
 * Return: 0 if the test passes, -1 if it fails
 */
int test_acked_slow_path(int ctl, int data)
{
	unsigned long acked1, acked2;
	struct tcp_info tinfo;
	socklen_t sl;

	(void)ctl;
	(void)data;

	srv(ctl, REPAIR, TCP_REPAIR_ON);

	sl = sizeof(tinfo);
	getsockopt(data, SOL_TCP, TCP_INFO, &tinfo, &sl);
	acked1 = tinfo.tcpi_bytes_acked;

	send(data, (char *)("a"), 1, 0);

	getsockopt(data, SOL_TCP, TCP_INFO, &tinfo, &sl);
	acked2 = tinfo.tcpi_bytes_acked;

	if (acked1 != acked2) {
		fprintf(stderr, "ACK received in repair mode, %lu -> %lu\n",
			acked1, acked2);
		return -1;
	}

	return 0;
}

/**
 * test_acked_fast_path() - Our ACK sequence doesn't change after large transfer
 * @ctl:	Control socket
 * @data:	Data socket
 *
 * Return: 0 if the test passes, -1 if it fails
 */
int test_acked_fast_path(int ctl, int data)
{
	unsigned long acked1, acked2;
	char buf[1000] = { 0 };
	struct tcp_info tinfo;
	socklen_t sl;
	int i;

	(void)ctl;
	(void)data;

	for (i = 0; i < 50; i++) {
		send(data, buf, sizeof(buf), 0);
		srv(ctl, RECV, sizeof(buf));
	}

	srv(ctl, REPAIR, TCP_REPAIR_ON);

	sl = sizeof(tinfo);
	getsockopt(data, SOL_TCP, TCP_INFO, &tinfo, &sl);
	acked1 = tinfo.tcpi_bytes_acked;

	fcntl(data, F_SETFL, O_NONBLOCK);
	for (i = 0; i < 50; i++)
		send(data, buf, sizeof(buf), 0);

	getsockopt(data, SOL_TCP, TCP_INFO, &tinfo, &sl);
	acked2 = tinfo.tcpi_bytes_acked;

	if (acked1 != acked2) {
		fprintf(stderr, "ACK received in repair mode, %lu -> %lu\n",
			acked1, acked2);
		return -1;
	}

	return 0;
}

/**
 * struct test - List of tests
 * @desc:	Test description
 * @f:		Function executing the test
 */
struct {
	char *desc;
	int (*f)(int ctl, int data);
} test[] = {
	{
		"Sequence freezes in repair mode, slow path TCP input",
		test_seq_slow_path,
	},
	{
		"Sequence freezes in repair mode, fast path TCP input",
		test_seq_fast_path,
	},
	{
		"No ACKs in repair mode, slow path TCP input",
		test_acked_slow_path,
	},
	{
		"No ACKs in repair mode, fast path TCP input",
		test_acked_fast_path,
	},
};

/**
 * main() - Entry point, connect control socket to server and run list of tests
 * @argc:	Argument count, must be 3 (two options)
 * @argv:	Options: server address and port
 *
 * Return: -1 on bad usage, 0 on success, 1 if at least one test fails
 */
int main(int argc, char **argv)
{
	struct addrinfo hints = { 0, AF_UNSPEC, SOCK_STREAM, 0, 0,
				  NULL, NULL, NULL };
	int ctl, data, ret = 0;
	struct addrinfo *r;
	unsigned i;

	if (argc != 3) {
		fprintf(stderr, "%s DST_ADDR DST_PORT\n", argv[0]);
		return -1;
	}

	getaddrinfo(argv[1], argv[2], &hints, &r);

	ctl = socket(r->ai_family, SOCK_STREAM, IPPROTO_TCP);
	setsockopt(ctl, SOL_SOCKET, SO_REUSEADDR, &((int){ 1 }), sizeof(int));
	connect(ctl, r->ai_addr, r->ai_addrlen);

	for (i = 0; i < sizeof(test) / sizeof(test[0]); i++) {
		int rc;

		data = socket(r->ai_family, SOCK_STREAM, IPPROTO_TCP);
		setsockopt(data, SOL_SOCKET, SO_REUSEADDR,
			   &((int){ 1 }), sizeof(int));
		srv(ctl, ACCEPT, 0);
		connect(data, r->ai_addr, r->ai_addrlen);

		rc = test[i].f(ctl, data);

		close(data);

		if (rc) {
			fprintf(stdout, "TEST: %-60s  [FAIL]\n", test[i].desc);
			ret = 1;
		} else {
			fprintf(stdout, "TEST: %-60s  [ OK ]\n", test[i].desc);
		}
	}

	return ret;
}
