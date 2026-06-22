// SPDX-License-Identifier: GPL-2.0
#include <inttypes.h>
#include "aolib.h"

static void *server_fn(void *arg)
{
	int sk, lsk;
	ssize_t bytes;

	lsk = test_listen_socket(this_ip_addr, test_server_port, 1);

	/* Server only has the specific key for the client.
	 * It expects KeyID 100, signed with "pass_specific".
	 */
	if (test_add_key(lsk, "pass_specific", this_ip_dest, -1, 100, 100))
		test_error("setsockopt(TCP_AO_ADD_KEY)");

	synchronize_threads(); /* 1: Server ready and key added */

	if (test_wait_fd(lsk, TEST_TIMEOUT_SEC, 0))
		test_error("test_wait_fd()");

	sk = accept(lsk, NULL, NULL);
	if (sk < 0)
		test_error("accept()");

	synchronize_threads(); /* 2: Connection accepted */

	/* Verify we can receive data from the client */
	bytes = test_server_run(sk, 0, 0);
	if (bytes < 0) {
		test_fail("server: failed to receive data");
	} else {
		test_ok("server: connection authenticated successfully");
	}

	close(sk);
	close(lsk);
	return NULL;
}

static void *client_fn(void *arg)
{
	int sk = socket(test_family, SOCK_STREAM, IPPROTO_TCP);
	union tcp_addr wildcard_addr = {};

	if (sk < 0)
		test_error("socket()");

	/* Client adds keys in the "wrong" order (wildcard last) to trigger shadowing.
	 * 1. Specific key (Key B, ID 100)
	 * 2. Wildcard key (Key A, ID 101)
	 *
	 * Without the fix, the wildcard key will be at the head of the list
	 * and will shadow the specific key during outbound lookup, causing
	 * the client to send a SYN with KeyID 101 (which the server doesn't have).
	 */

	/* 1. Add specific key */
	if (test_add_key(sk, "pass_specific", this_ip_dest, -1, 100, 100))
		test_error("setsockopt(TCP_AO_ADD_KEY) specific");

	/* 2. Add wildcard key (any address, prefix 0) */
	if (test_add_key(sk, "pass_wildcard", wildcard_addr, 0, 101, 101))
		test_error("setsockopt(TCP_AO_ADD_KEY) wildcard");

	synchronize_threads(); /* 1: Client ready and keys added => connect() */

	if (test_connect_socket(sk, this_ip_dest, test_server_port) <= 0) {
		test_fail("client: failed to connect (shadowing bug present?)");
		close(sk);
		return NULL;
	}

	synchronize_threads(); /* 2: Connection established */

	/* Send some data to verify the connection works */
	if (test_client_verify(sk, 100, 20)) {
		test_fail("client: verify failed");
	} else {
		test_ok("client: connection established and verified (precedence correct)");
	}

	close(sk);
	return NULL;
}

int main(int argc, char *argv[])
{
	/* We expect 2 test results: 1 from server, 1 from client */
	test_init(2, server_fn, client_fn);
	return 0;
}
