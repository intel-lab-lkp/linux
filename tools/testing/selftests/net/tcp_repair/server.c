// SPDX-License-Identifier: GPL-2.0-only
/* selftests/net/tcp_repair: TCP_REPAIR connection tests
 *
 * server.c - Receive commands and data, set TCP_REPAIR options on data socket
 *
 * Copyright (c) 2026 Red Hat GmbH
 *
 * Author: Stefano Brivio <sbrivio@redhat.com>
 */

#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>

#include <linux/tcp.h>		/* needed for TCP_REPAIR constants but */
#define SOL_TCP		6	/* we can't include netinet/tcp.h as a result */

#include "talk.h"

/**
 * cmd_accept() - Accept data connection (must be first command in test)
 * @unused:	Not used
 * @listen:	Listening socket
 * @data:	Return value from accept(), set on return
 *
 * Return: 0
 */
int cmd_accept(int unused, int listen, int *data)
{
	(void)unused;

	if (*data != -1)
		close(*data);

	*data = accept(listen, NULL, NULL);

	return 0;
}

/**
 * cmd_dump_recv_seq() - Dump receive sequence of data socket
 * @unused:	Not used
 * @unused2:	Not used
 * @data:	File descriptor for data socket
 *
 * Return: receive sequence of data socket
 */
int cmd_dump_recv_seq(int unused, int unused2, int *data)
{
	socklen_t sl;
	int v;

	(void)unused;
	(void)unused2;

	v = TCP_RECV_QUEUE;
	setsockopt(*data, SOL_TCP, TCP_REPAIR_QUEUE, &v, sizeof(v));

	sl = sizeof(v);
	getsockopt(*data, SOL_TCP, TCP_QUEUE_SEQ, &v, &sl);
	return v;
}

/**
 * cmd_exit() - Exit successfully
 * @unused:	Not used
 * @unused2:	Not used
 * @unused3:	Not used
 *
 * Return: this function doesn't actually return
 */
int cmd_exit(int unused, int unused2, int *unused3)
{
	(void)unused;
	(void)unused2;
	(void)unused3;

	exit(EXIT_SUCCESS);
	return 0;
}

/**
 * cmd_recv() - Receive (discard) a given amount of bytes
 * @len:	Amount of bytes the client wants us to receive
 * @unused:	Not used
 * @data:	File descriptor for data socket
 *
 * Return: return code from recv()
 */
int cmd_recv(int len, int unused, int *data)
{
	(void)unused;

	return recv(*data, NULL, len, MSG_TRUNC);
}

/**
 * cmd_repair() - Set repair mode to mode supplied by client
 * @mode:	Value for socket option provided by the client
 * @unused:	Not used
 * @data:	File descriptor for data socket
 *
 * Return: return code from setsockopt()
 */
int cmd_repair(int mode, int unused, int *data)
{
	(void)unused;

	return setsockopt(*data, SOL_TCP, TCP_REPAIR, &mode, sizeof(mode));
}

/* List of commands and their handlers */
int (*fn[])(int arg, int listen, int *data) = {
	[ACCEPT]	= cmd_accept,
	[DUMP_RECV_SEQ]	= cmd_dump_recv_seq,
	[EXIT]		= cmd_exit,
	[RECV]		= cmd_recv,
	[REPAIR]	= cmd_repair,
};

/**
 * main() - Entry point, accept control connection and dispatch commands
 * @argc:	Argument count, must be 2 (one option)
 * @argv:	Options: server port
 *
 * Return: 0 on success, exit on failure
 */
int main(int argc, char **argv)
{
	struct sockaddr_in a = { AF_INET, htons(atoi(argv[1])), { 0 }, { 0 } };
	int s, ctl, data = -1, cmd[2];

	s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &((int){ 1 }), sizeof(int));

	if (argc != 2) {
		fprintf(stderr, "%s PORT\n", argv[0]);
		exit(EXIT_FAILURE);
	}

	bind(s, (struct sockaddr *)&a, sizeof(a));
	listen(s, 0);
	ctl = accept(s, NULL, NULL);

	while (recv(ctl, cmd, sizeof(cmd), 0) == sizeof(cmd)) {
		int ret = fn[cmd[0]](cmd[1], s, &data);
		if (cmd[0] != ACCEPT)
			send(ctl, &ret, sizeof(ret), 0);
	}

	return 0;
}
