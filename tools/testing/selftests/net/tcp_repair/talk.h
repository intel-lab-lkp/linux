// SPDX-License-Identifier: GPL-2.0-only

/* selftests/net/tcp_repair: TCP_REPAIR connection tests
 *
 * talk.h - Communication protocol for client and server
 *
 * Copyright (c) 2026 Red Hat GmbH
 *
 * Author: Stefano Brivio <sbrivio@redhat.com>
 */

/**
 * enum op - Server command type (taking optional int argument, returning int)
 * @ACCEPT		Accept connection on data socket (doesn't return int)
 * @DUMP_RECV_SEQ	Dump receive sequence, return it to the client
 * @EXIT		Exit, return 0 to the client
 * @RECV		Try receiving given amount of bytes, return received
 * @REPAIR		Set repair mode to argument, return setsockopt() value
 */
enum op {
	ACCEPT,
	DUMP_RECV_SEQ,
	EXIT,
	RECV,
	REPAIR,
};
