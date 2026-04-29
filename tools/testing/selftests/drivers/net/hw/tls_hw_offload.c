// SPDX-License-Identifier: GPL-2.0
/*
 * TLS Hardware Offload Two-Node Test
 *
 * Tests kTLS hardware offload between two physical nodes using
 * hardcoded keys. Supports TLS 1.2/1.3, AES-GCM-128/256, and rekey.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <linux/tls.h>

#define TLS_RECORD_TYPE_HANDSHAKE		22
#define TLS_HANDSHAKE_KEY_UPDATE		0x18

#define MIN_BUF_SIZE   16

/* Initial key material */
static struct tls12_crypto_info_aes_gcm_128 tls_info_key0_128 = {
	.info = {
		.version = TLS_1_3_VERSION,
		.cipher_type = TLS_CIPHER_AES_GCM_128,
	},
	.iv = { 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08 },
	.key = { 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
		 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10 },
	.salt = { 0x01, 0x02, 0x03, 0x04 },
	.rec_seq = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
};

static struct tls12_crypto_info_aes_gcm_256 tls_info_key0_256 = {
	.info = {
		.version = TLS_1_3_VERSION,
		.cipher_type = TLS_CIPHER_AES_GCM_256,
	},
	.iv = { 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08 },
	.key = { 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
		 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10,
		 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18,
		 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f, 0x20 },
	.salt = { 0x01, 0x02, 0x03, 0x04 },
	.rec_seq = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
};

static int num_rekeys;
static int num_iterations = 100;
static int cipher_type = TLS_CIPHER_AES_GCM_128;
static int tls_version = TLS_1_3_VERSION;
static int server_port = 4433;
static char *server_ip;

static int send_size = 16384;
static int random_size_max;
/* Burst mode: sender keeps pushing records without reading from the peer;
 * receiver drains without echoing back. Only the client initiates rekey.
 */
static int burst_mode;
static int zc_rx;

/* XOR each byte with the generation so both endpoints derive the
 * same per-generation key without a real KDF. Generation 0 leaves
 * the base key unchanged.
 */
static void derive_key_fields(unsigned char *key, int key_size,
			      unsigned char *iv, int iv_size,
			      unsigned char *salt, int salt_size,
			      unsigned char *rec_seq, int rec_seq_size,
			      int generation)
{
	int i;

	for (i = 0; i < key_size; i++)
		key[i] ^= generation;
	for (i = 0; i < iv_size; i++)
		iv[i] ^= generation;
	for (i = 0; i < salt_size; i++)
		salt[i] ^= generation;
	memset(rec_seq, 0, rec_seq_size);
}

static void derive_key_128(struct tls12_crypto_info_aes_gcm_128 *key,
			   int generation)
{
	memcpy(key, &tls_info_key0_128, sizeof(*key));
	key->info.version = tls_version;
	derive_key_fields(key->key, TLS_CIPHER_AES_GCM_128_KEY_SIZE,
			  key->iv, TLS_CIPHER_AES_GCM_128_IV_SIZE,
			  key->salt, TLS_CIPHER_AES_GCM_128_SALT_SIZE,
			  key->rec_seq, TLS_CIPHER_AES_GCM_128_REC_SEQ_SIZE,
			  generation);
}

static void derive_key_256(struct tls12_crypto_info_aes_gcm_256 *key,
			   int generation)
{
	memcpy(key, &tls_info_key0_256, sizeof(*key));
	key->info.version = tls_version;
	derive_key_fields(key->key, TLS_CIPHER_AES_GCM_256_KEY_SIZE,
			  key->iv, TLS_CIPHER_AES_GCM_256_IV_SIZE,
			  key->salt, TLS_CIPHER_AES_GCM_256_SALT_SIZE,
			  key->rec_seq, TLS_CIPHER_AES_GCM_256_REC_SEQ_SIZE,
			  generation);
}

static const char *cipher_name(int cipher)
{
	switch (cipher) {
	case TLS_CIPHER_AES_GCM_128: return "AES-GCM-128";
	case TLS_CIPHER_AES_GCM_256: return "AES-GCM-256";
	default: return "unknown";
	}
}

static const char *version_name(int version)
{
	switch (version) {
	case TLS_1_2_VERSION: return "TLS 1.2";
	case TLS_1_3_VERSION: return "TLS 1.3";
	default: return "unknown";
	}
}

static int setup_tls_ulp(int fd)
{
	int ret;

	ret = setsockopt(fd, IPPROTO_TCP, TCP_ULP, "tls", sizeof("tls"));
	if (ret < 0) {
		printf("SETUP ERROR: TCP_ULP failed: %s\n", strerror(errno));
		return -1;
	}
	return 0;
}

static int set_zc_rx(int fd)
{
	int val = 1;

	if (setsockopt(fd, SOL_TLS, TLS_RX_EXPECT_NO_PAD, &val,
		       sizeof(val)) < 0) {
		printf("SETUP ERROR: TLS_RX_EXPECT_NO_PAD failed: %s\n",
		       strerror(errno));
		return -1;
	}
	return 0;
}

/* Send a TLS 1.3 KeyUpdate handshake record. The kernel only
 * inspects the HandshakeType byte to detect KeyUpdate, so don't
 * bother with the 3-byte length or request_update fields.
 */
static int send_tls_key_update(int fd)
{
	char cmsg_buf[CMSG_SPACE(sizeof(unsigned char))];
	unsigned char key_update_msg = TLS_HANDSHAKE_KEY_UPDATE;
	struct msghdr msg = {0};
	struct cmsghdr *cmsg;
	struct iovec iov;

	iov.iov_base = &key_update_msg;
	iov.iov_len = sizeof(key_update_msg);

	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
	msg.msg_control = cmsg_buf;
	msg.msg_controllen = sizeof(cmsg_buf);

	cmsg = CMSG_FIRSTHDR(&msg);
	cmsg->cmsg_level = SOL_TLS;
	cmsg->cmsg_type = TLS_SET_RECORD_TYPE;
	cmsg->cmsg_len = CMSG_LEN(sizeof(unsigned char));
	*CMSG_DATA(cmsg) = TLS_RECORD_TYPE_HANDSHAKE;
	msg.msg_controllen = cmsg->cmsg_len;

	if (sendmsg(fd, &msg, 0) < 0) {
		printf("sendmsg KeyUpdate failed: %s\n", strerror(errno));
		return -1;
	}

	printf("Sent TLS KeyUpdate handshake message\n");
	return 0;
}

static int recv_tls_message(int fd, char *buf, size_t buflen, int *record_type)
{
	char cmsg_buf[CMSG_SPACE(sizeof(unsigned char))];
	struct msghdr msg = {0};
	struct cmsghdr *cmsg;
	struct iovec iov;
	int ret;

	iov.iov_base = buf;
	iov.iov_len = buflen;

	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
	msg.msg_control = cmsg_buf;
	msg.msg_controllen = sizeof(cmsg_buf);

	ret = recvmsg(fd, &msg, 0);
	if (ret <= 0)
		return ret;

	cmsg = CMSG_FIRSTHDR(&msg);
	if (cmsg && cmsg->cmsg_level == SOL_TLS &&
	    cmsg->cmsg_type == TLS_GET_RECORD_TYPE)
		*record_type = *((unsigned char *)CMSG_DATA(cmsg));

	return ret;
}

/* Confirm a handshake record starting with HandshakeType KeyUpdate. */
static int check_keyupdate(const char *buf, int len, int record_type)
{
	if (record_type != TLS_RECORD_TYPE_HANDSHAKE) {
		printf("Expected handshake record (0x%02x), got 0x%02x\n",
		       TLS_RECORD_TYPE_HANDSHAKE, record_type);
		return -1;
	}
	if (len < 1 || (unsigned char)buf[0] != TLS_HANDSHAKE_KEY_UPDATE) {
		printf("Expected KeyUpdate (0x%02x), got 0x%02x\n",
		       TLS_HANDSHAKE_KEY_UPDATE,
		       len ? (unsigned char)buf[0] : 0);
		return -1;
	}
	printf("Received TLS KeyUpdate\n");
	return 0;
}

static int recv_tls_keyupdate(int fd)
{
	char buf[MIN_BUF_SIZE];
	int record_type = 0;
	int ret;

	ret = recv_tls_message(fd, buf, sizeof(buf), &record_type);
	if (ret < 0) {
		printf("recv_tls_message failed: %s\n", strerror(errno));
		return -1;
	}

	return check_keyupdate(buf, ret, record_type);
}

static int check_ekeyexpired(int fd)
{
	char buf[MIN_BUF_SIZE];
	int ret;

	ret = recv(fd, buf, sizeof(buf), MSG_DONTWAIT);
	if (ret == -1 && errno == EKEYEXPIRED) {
		printf("recv() returned EKEYEXPIRED as expected\n");
		return 0;
	} else if (ret == -1 && errno == EAGAIN) {
		printf("recv() returned EAGAIN (no pending data)\n");
		return 0;
	} else if (ret > 0) {
		printf("FAIL: recv() returned %d bytes, expected EKEYEXPIRED\n",
		       ret);
		return -1;
	} else {
		printf("FAIL: recv() returned unexpected error: %s\n",
		       strerror(errno));
		return -1;
	}
}

static int do_tls_rekey(int fd, int direction, int generation, int cipher)
{
	const char *dir = direction == TLS_TX ? "TX" : "RX";
	int ret;

	printf("%s TLS_%s %s gen %d...\n",
	       generation ? "Rekeying" : "Installing",
	       dir, cipher_name(cipher), generation);

	if (cipher == TLS_CIPHER_AES_GCM_256) {
		struct tls12_crypto_info_aes_gcm_256 key;

		derive_key_256(&key, generation);
		ret = setsockopt(fd, SOL_TLS, direction, &key, sizeof(key));
	} else {
		struct tls12_crypto_info_aes_gcm_128 key;

		derive_key_128(&key, generation);
		ret = setsockopt(fd, SOL_TLS, direction, &key, sizeof(key));
	}

	if (ret < 0) {
		printf("%sTLS_%s %s gen %d failed: %s\n",
		       generation ? "" : "SETUP ERROR: ", dir,
		       cipher_name(cipher), generation, strerror(errno));
		return -1;
	}
	printf("TLS_%s %s gen %d installed\n",
	       dir, cipher_name(cipher), generation);
	return 0;
}

static int do_client(void)
{
	char *buf = NULL, *echo_buf = NULL;
	int max_size, rekey_interval;
	ssize_t echo_total, echo_n;
	int csk = -1, ret, i, j;
	struct sockaddr_in sa;
	int test_result = -1;
	int current_gen = 0;
	int next_rekey_at;
	ssize_t n;

	max_size = random_size_max > 0 ? random_size_max : send_size;
	if (max_size < MIN_BUF_SIZE)
		max_size = MIN_BUF_SIZE;
	buf = malloc(max_size);
	if (!burst_mode)
		echo_buf = malloc(max_size);
	if (!buf || (!burst_mode && !echo_buf)) {
		printf("SETUP ERROR: failed to allocate buffers\n");
		goto out;
	}

	csk = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (csk < 0) {
		printf("SETUP ERROR: failed to create socket: %s\n",
		       strerror(errno));
		goto out;
	}

	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_addr.s_addr = inet_addr(server_ip);
	sa.sin_port = htons(server_port);
	printf("Connecting to %s:%d...\n", server_ip, server_port);

	ret = connect(csk, (struct sockaddr *)&sa, sizeof(sa));
	if (ret < 0) {
		printf("SETUP ERROR: connect failed: %s\n", strerror(errno));
		goto out;
	}
	printf("Connected!\n");

	if (setup_tls_ulp(csk) < 0)
		goto out;

	if (do_tls_rekey(csk, TLS_TX, 0, cipher_type) < 0 ||
	    do_tls_rekey(csk, TLS_RX, 0, cipher_type) < 0)
		goto out;

	if (num_rekeys)
		printf("TLS %s setup complete. Will perform %d rekey(s).\n",
		       cipher_name(cipher_type), num_rekeys);
	else
		printf("TLS setup complete.\n");

	if (random_size_max > 0)
		printf("Sending %d messages of random size (1..%d bytes)...\n",
		       num_iterations, random_size_max);
	else
		printf("Sending %d messages of %d bytes...\n",
		       num_iterations, send_size);

	rekey_interval = num_iterations / (num_rekeys + 1);
	next_rekey_at = rekey_interval;

	for (i = 1; i <= num_iterations; i++) {
		int this_size;

		if (random_size_max > 0)
			this_size = (rand() % random_size_max) + 1;
		else
			this_size = send_size;

		/* In burst mode, use a per-iteration fill pattern so the
		 * receiver can detect any plaintext corruption without a
		 * round-trip echo.
		 */
		if (burst_mode) {
			memset(buf, i & 0xFF, this_size);
		} else {
			for (j = 0; j < this_size; j++)
				buf[j] = rand() & 0xFF;
		}

		n = send(csk, buf, this_size, 0);
		if (n != this_size) {
			printf("FAIL: send failed: %s\n", strerror(errno));
			goto out;
		}
		/* Throttle per-iteration progress lines on long burst runs so
		 * stdout over ssh doesn't become the bottleneck.
		 */
		if (!burst_mode || num_iterations <= 1000 || (i % 1000) == 0 ||
		    i == num_iterations)
			printf("Sent %zd bytes (iteration %d)\n", n, i);

		if (!burst_mode) {
			echo_total = 0;
			while (echo_total < n) {
				echo_n = recv(csk, echo_buf + echo_total,
					      n - echo_total, 0);
				if (echo_n < 0) {
					printf("FAIL: Echo recv failed: %s\n",
					       strerror(errno));
					goto out;
				}
				if (echo_n == 0) {
					printf("FAIL: Connection closed during echo\n");
					goto out;
				}
				echo_total += echo_n;
			}

			if (memcmp(buf, echo_buf, n) != 0) {
				printf("FAIL: Echo data mismatch!\n");
				goto out;
			}
			printf("Received echo %zd bytes (ok)\n", echo_total);
		}

		/* Rekey at intervals. In echo mode this is a full bidirectional
		 * exchange; in burst mode the client only rotates its TX key
		 * and sends KeyUpdate - the peer is expected to follow.
		 */
		if (num_rekeys && current_gen < num_rekeys &&
		    i == next_rekey_at) {
			current_gen++;
			printf("\n=== Client Rekey gen %d ===\n", current_gen);

			ret = send_tls_key_update(csk);
			if (ret < 0) {
				printf("FAIL: send KeyUpdate\n");
				goto out;
			}

			ret = do_tls_rekey(csk, TLS_TX, current_gen, cipher_type);
			if (ret < 0)
				goto out;

			if (!burst_mode) {
				if (recv_tls_keyupdate(csk) < 0) {
					printf("FAIL: recv KeyUpdate from server\n");
					goto out;
				}

				if (check_ekeyexpired(csk) < 0)
					goto out;

				ret = do_tls_rekey(csk, TLS_RX, current_gen,
						   cipher_type);
				if (ret < 0)
					goto out;
			}

			next_rekey_at += rekey_interval;
			printf("=== Client Rekey gen %d Complete ===\n\n",
			       current_gen);
		}
	}

	test_result = 0;
out:
	if (num_rekeys)
		printf("Rekeys completed: %d/%d\n", current_gen, num_rekeys);
	if (csk >= 0)
		close(csk);
	free(buf);
	free(echo_buf);
	return test_result;
}

static int do_server(void)
{
	int lsk = -1, csk = -1, ret;
	ssize_t n, total = 0, sent;
	/* Burst-mode data verification state: client fills each iteration's
	 * send_size-byte block with (send_iter & 0xff). A single recv may
	 * return part of a block or span iterations, so track position as
	 * (send_iter, remaining) - bytes left in the current block.
	 */
	int send_iter = 1;
	int remaining = send_size;
	struct sockaddr_in sa;
	int test_result = -1;
	int current_gen = 0;
	int recv_count = 0;
	char *buf = NULL;
	int record_type;
	int buf_size;
	int one = 1;

	buf_size = send_size;
	if (buf_size < MIN_BUF_SIZE)
		buf_size = MIN_BUF_SIZE;
	buf = malloc(buf_size);
	if (!buf) {
		printf("SETUP ERROR: failed to allocate buffer\n");
		goto out;
	}

	lsk = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (lsk < 0) {
		printf("SETUP ERROR: failed to create socket: %s\n",
		       strerror(errno));
		goto out;
	}

	setsockopt(lsk, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_addr.s_addr = INADDR_ANY;
	sa.sin_port = htons(server_port);

	ret = bind(lsk, (struct sockaddr *)&sa, sizeof(sa));
	if (ret < 0) {
		printf("SETUP ERROR: bind failed: %s\n", strerror(errno));
		goto out;
	}

	ret = listen(lsk, 1);
	if (ret < 0) {
		printf("SETUP ERROR: listen failed: %s\n", strerror(errno));
		goto out;
	}

	printf("Server listening on 0.0.0.0:%d\n", server_port);
	printf("Waiting for client connection...\n");

	csk = accept(lsk, (struct sockaddr *)NULL, (socklen_t *)NULL);
	if (csk < 0) {
		printf("SETUP ERROR: accept failed: %s\n", strerror(errno));
		goto out;
	}
	printf("Client connected!\n");

	if (setup_tls_ulp(csk) < 0)
		goto out;

	if (do_tls_rekey(csk, TLS_TX, 0, cipher_type) < 0 ||
	    do_tls_rekey(csk, TLS_RX, 0, cipher_type) < 0)
		goto out;

	if (zc_rx && set_zc_rx(csk) < 0)
		goto out;

	printf("TLS %s setup complete. Receiving...\n",
	       cipher_name(cipher_type));

	/* Main receive loop */
	while (1) {
		n = recv_tls_message(csk, buf, buf_size, &record_type);
		if (n == 0) {
			printf("Connection closed by client\n");
			break;
		}
		if (n < 0) {
			printf("FAIL: recv failed: %s\n", strerror(errno));
			goto out;
		}

		/* Handle KeyUpdate. In echo mode the server mirrors the
		 * rekey back to the peer; in burst mode it only rotates
		 * its RX key and keeps draining.
		 */
		if (record_type == TLS_RECORD_TYPE_HANDSHAKE) {
			if (check_keyupdate(buf, n, record_type) < 0)
				goto out;
			current_gen++;
			printf("\n=== Server Rekey gen %d ===\n", current_gen);

			if (check_ekeyexpired(csk) < 0)
				goto out;

			ret = do_tls_rekey(csk, TLS_RX, current_gen, cipher_type);
			if (ret < 0)
				goto out;

			if (!burst_mode) {
				ret = send_tls_key_update(csk);
				if (ret < 0) {
					printf("FAIL: send KeyUpdate\n");
					goto out;
				}

				ret = do_tls_rekey(csk, TLS_TX, current_gen,
						   cipher_type);
				if (ret < 0)
					goto out;
			}

			printf("=== Server Rekey gen %d Complete ===\n\n",
			       current_gen);
			continue;
		}

		total += n;
		recv_count++;
		/* Throttle per-record progress lines on long burst runs. */
		if (!burst_mode || (recv_count % 1000) == 0)
			printf("Received %zd bytes (total: %zd, count: %d)\n",
			       n, total, recv_count);

		/* Burst mode: verify payload matches the client's fill
		 * pattern. TLS record boundaries may differ from send()
		 * boundaries, so walk the received buffer in chunks that
		 * fit within the current iteration's remaining bytes.
		 * Catches decrypt-succeeded-but-plaintext-corrupt bugs
		 * that AEAD counters alone would miss.
		 */
		if (burst_mode) {
			int off = 0;

			while (off < n) {
				unsigned char expect = send_iter & 0xFF;
				int chunk = n - off;
				int j;

				if (chunk > remaining)
					chunk = remaining;

				for (j = 0; j < chunk; j++) {
					if ((unsigned char)buf[off + j] != expect) {
						printf("FAIL: data mismatch recv #%d offset %d:"
						       " expected 0x%02x got 0x%02x"
						       " (iter %d, remaining %d)\n",
						       recv_count, off + j,
						       expect,
						       (unsigned char)buf[off + j],
						       send_iter, remaining);
						goto out;
					}
				}

				off += chunk;
				remaining -= chunk;
				if (remaining == 0) {
					send_iter++;
					remaining = send_size;
				}
			}
			continue;
		}

		for (sent = 0; sent < n; sent += ret) {
			ret = send(csk, buf + sent, n - sent, 0);
			if (ret < 0) {
				printf("FAIL: Echo send failed: %s\n",
				       strerror(errno));
				goto out;
			}
		}
		printf("Echoed %zd bytes back to client\n", n);
	}

	test_result = 0;
out:
	printf("Connection closed. Total received: %zd bytes\n", total);
	if (num_rekeys)
		printf("Rekeys completed: %d\n", current_gen);

	if (csk >= 0)
		close(csk);
	if (lsk >= 0)
		close(lsk);
	free(buf);
	return test_result;
}

static int parse_cipher_option(const char *arg)
{
	if (strcmp(arg, "128") == 0) {
		cipher_type = TLS_CIPHER_AES_GCM_128;
		return 0;
	} else if (strcmp(arg, "256") == 0) {
		cipher_type = TLS_CIPHER_AES_GCM_256;
		return 0;
	}
	printf("ERROR: Invalid cipher '%s'. Must be 128 or 256.\n", arg);
	return -1;
}

static int parse_version_option(const char *arg)
{
	if (strcmp(arg, "1.2") == 0) {
		tls_version = TLS_1_2_VERSION;
		return 0;
	} else if (strcmp(arg, "1.3") == 0) {
		tls_version = TLS_1_3_VERSION;
		return 0;
	}
	printf("ERROR: Invalid TLS version '%s'. Must be 1.2 or 1.3.\n", arg);
	return -1;
}

static void print_usage(const char *prog)
{
	printf("TLS Hardware Offload Two-Node Test\n\n");
	printf("Usage:\n");
	printf("  %s server [OPTIONS]\n", prog);
	printf("  %s client -s <ip> [OPTIONS]\n", prog);
	printf("\nOptions:\n");
	printf("  -s <ip>       Server IPv4 address (client, required)\n");
	printf("  -p <port>     Server port (default: 4433)\n");
	printf("  -b <size>     Send buffer size in bytes (default: 16384)\n");
	printf("  -r <max>      Use random send buffer sizes (1..<max>)\n");
	printf("  -v <version>  TLS version: 1.2 or 1.3 (default: 1.3)\n");
	printf("  -c <cipher>   Cipher: 128 or 256 (default: 128)\n");
	printf("  -n <N>        Number of send/echo iterations (default: 100)\n");
	printf("  -k <N>        Perform N rekeys (client only, TLS 1.3; N < iterations)\n");
	printf("  -B            Burst mode: client sends continuously without echo;\n");
	printf("                server drains and handles KeyUpdate without responding.\n");
	printf("  -Z            Enable zero-copy RX (TLS_RX_EXPECT_NO_PAD);\n");
	printf("                server only, TLS 1.3 only.\n");
	printf("  -h            Show this help message\n");
	printf("\nExample:\n");
	printf("  Node A: %s server\n", prog);
	printf("  Node B: %s client -s 192.168.20.2\n", prog);
	printf("\nRekey Example (3 rekeys, TLS 1.3 only):\n");
	printf("  Node A: %s server\n", prog);
	printf("  Node B: %s client -s 192.168.20.2 -k 3\n", prog);
	printf("\nBurst Mode Example (client stresses TX rekey under load):\n");
	printf("  Node A: %s server -B\n", prog);
	printf("  Node B: %s client -s 192.168.20.2 -B -k 3\n", prog);
}

int main(int argc, char *argv[])
{
	int send_size_set = 0;
	int is_server;
	int opt;

	if (argc < 2 ||
	    (strcmp(argv[1], "server") && strcmp(argv[1], "client"))) {
		print_usage(argv[0]);
		return 1;
	}
	is_server = !strcmp(argv[1], "server");

	optind = 2; /* skip subcommand */
	while ((opt = getopt(argc, argv, "s:p:b:r:c:v:k:n:BZh")) != -1) {
		switch (opt) {
		case 's':
			server_ip = optarg;
			break;
		case 'B':
			burst_mode = 1;
			break;
		case 'Z':
			zc_rx = 1;
			break;
		case 'p':
			server_port = atoi(optarg);
			if (server_port < 1 || server_port > 65535) {
				printf("ERROR: Invalid port '%s'. Must be 1..65535.\n",
				       optarg);
				return 1;
			}
			break;
		case 'b':
			send_size = atoi(optarg);
			if (send_size < 1) {
				printf("ERROR: Invalid buffer size '%s'. Must be >= 1.\n",
				       optarg);
				return 1;
			}
			send_size_set = 1;
			break;
		case 'r':
			random_size_max = atoi(optarg);
			if (random_size_max < 1) {
				printf("ERROR: Invalid random size '%s'. Must be >= 1.\n",
				       optarg);
				return 1;
			}
			break;
		case 'c':
			if (parse_cipher_option(optarg) < 0)
				return 1;
			break;
		case 'v':
			if (parse_version_option(optarg) < 0)
				return 1;
			break;
		case 'k':
			num_rekeys = atoi(optarg);
			if (num_rekeys < 1) {
				printf("ERROR: Invalid rekey count '%s'. Must be >= 1.\n",
				       optarg);
				return 1;
			}
			break;
		case 'n':
			num_iterations = atoi(optarg);
			if (num_iterations < 1) {
				printf("ERROR: Invalid iteration count '%s'. Must be >= 1.\n",
				       optarg);
				return 1;
			}
			break;
		case 'h':
			print_usage(argv[0]);
			return 0;
		default:
			print_usage(argv[0]);
			return 1;
		}
	}

	if (send_size_set && random_size_max > 0) {
		printf("ERROR: -b and -r are mutually exclusive\n");
		return 1;
	}

	if (zc_rx && tls_version != TLS_1_3_VERSION) {
		printf("ERROR: -Z (TLS_RX_EXPECT_NO_PAD) requires TLS 1.3\n");
		return 1;
	}

	if (burst_mode && random_size_max > 0) {
		printf("ERROR: -B and -r are mutually exclusive\n");
		return 1;
	}

	if (is_server) {
		if (server_ip) {
			printf("warning: -s is ignored in server mode\n");
			server_ip = NULL;
		}
		if (random_size_max > 0) {
			printf("warning: -r is ignored in server mode\n");
			random_size_max = 0;
		}
		if (num_rekeys) {
			printf("warning: -k is ignored in server mode\n");
			num_rekeys = 0;
		}
	} else {
		if (!server_ip) {
			printf("ERROR: Client requires -s <ip> option\n");
			return 1;
		}
		if (tls_version == TLS_1_2_VERSION && num_rekeys) {
			printf("ERROR: TLS 1.2 does not support rekey\n");
			return 1;
		}
		if (num_rekeys >= num_iterations) {
			printf("ERROR: num_rekeys (%d) must be < num_iterations (%d)\n",
			       num_rekeys, num_iterations);
			return 1;
		}
		if (zc_rx) {
			printf("ERROR: -Z applies to the server (receiver) only\n");
			return 1;
		}
	}

	printf("TLS Version: %s\n", version_name(tls_version));
	printf("Cipher: %s\n", cipher_name(cipher_type));
	if (random_size_max > 0)
		printf("Buffer size: random (1..%d)\n", random_size_max);
	else
		printf("Buffer size: %d\n", send_size);

	if (num_rekeys)
		printf("Rekey testing ENABLED: %d rekey(s)\n", num_rekeys);
	if (burst_mode)
		printf("Burst mode ENABLED\n");
	if (zc_rx)
		printf("Zero-copy RX ENABLED\n");

	srand(time(NULL));

	if (is_server)
		return do_server() ? 1 : 0;

	return do_client() ? 1 : 0;
}
