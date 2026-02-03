// SPDX-License-Identifier: GPL-2.0
/*
 * TLS Hardware Offload Two-Node Test
 *
 * This test uses hardcoded keys (no TLS handshake) to test kTLS
 * hardware offload between two physical nodes. Both nodes must
 * use the same key material.
 *
 * For rekey testing, proper TLS KeyUpdate handshake messages are
 * sent via sendmsg/recvmsg with TLS_SET_RECORD_TYPE/TLS_GET_RECORD_TYPE.
 *
 * The test verifies TLS counters from /proc/net/tls_stat:
 *   - TlsTxDevice/TlsRxDevice: HW offload was used
 *   - TlsTxRekeyOk/TlsRxRekeyOk: Rekey operations succeeded
 *   - TlsRxRekeyReceived: KeyUpdate messages received (server)
 *   - TlsDecryptError: No decryption errors occurred
 *
 * Usage:
 *   Server: ./tls_hw_offload server [OPTIONS]
 *   Client: ./tls_hw_offload client -s <ip> [OPTIONS]
 *
 * Options:
 *   -s <ip>    Server IP (client only, required)
 *   -p <port>  Port number (default: 4433)
 *   -c <128|256>  Cipher (default: 128)
 *   -v <1.2|1.3>  TLS version (default: 1.3)
 *   -b <size>  Fixed buffer size (default: 16384)
 *   -r <max>   Random buffer sizes from 1 to max
 *   --rekey[=N]   Enable rekey testing (default: 1, max: 4)
 *
 * Example:
 *   Node A: ./tls_hw_offload server
 *   Node B: ./tls_hw_offload client -s 192.168.20.2
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <linux/tls.h>

/* TLS record types for sendmsg/recvmsg with kTLS */
#define TLS_RECORD_TYPE_HANDSHAKE		22
#define TLS_RECORD_TYPE_APPLICATION_DATA	23

/* TLS 1.3 KeyUpdate handshake message type (RFC 8446) */
#define TLS_HANDSHAKE_KEY_UPDATE	0x18
#define KEY_UPDATE_NOT_REQUESTED	0
#define KEY_UPDATE_REQUESTED		1

/* Number of messages to send in the test loop */
#define TEST_ITERATIONS	10

/*
 * Maximum number of rekeys allowed per test run.
 * With TEST_ITERATIONS=10, this ensures at least 2 messages between rekeys.
 */
#define MAX_REKEYS	4

/* TLS 1.3 AES-GCM-128 key material - initial key (generation 0) */
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

/* TLS 1.3 AES-GCM-256 key material - initial key (generation 0) */
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

static int do_rekey;           /* Set via command line */
static int num_rekeys = 1;     /* Number of rekeys to perform */
static int rekeys_done;        /* Counter for completed rekeys */

/* Cipher selection: 128 or 256 */
static int cipher_type = 128;

/* TLS version: 12 for TLS 1.2, 13 for TLS 1.3 (default) */
static int tls_version = 13;

/* Server port (default: 4433) */
static int server_port = 4433;

/* Server IP for client to connect to */
static char *server_ip;

/* Send buffer size (default: 16384) */
static int send_size = 16384;

/* Random send size max (0 = disabled, use fixed send_size) */
static int random_size_max;

/* Structure to hold TLS statistics for verification */
struct tls_stats {
	long curr_tx_sw;
	long curr_rx_sw;
	long curr_tx_device;
	long curr_rx_device;
	long tx_sw;
	long rx_sw;
	long tx_device;
	long rx_device;
	long decrypt_error;
	long rx_device_resync;
	long decrypt_retry;
	long rx_no_pad_violation;
	long rx_rekey_ok;
	long rx_rekey_error;
	long tx_rekey_ok;
	long tx_rekey_error;
	long rx_rekey_received;
};

static int read_tls_stats(struct tls_stats *stats)
{
	char line[256];
	char name[64];
	long value;
	FILE *f;

	memset(stats, 0, sizeof(*stats));

	f = fopen("/proc/net/tls_stat", "r");
	if (!f)
		return -1;

	while (fgets(line, sizeof(line), f)) {
		if (sscanf(line, "%63s %ld", name, &value) == 2) {
			if (strcmp(name, "TlsCurrTxSw") == 0)
				stats->curr_tx_sw = value;
			else if (strcmp(name, "TlsCurrRxSw") == 0)
				stats->curr_rx_sw = value;
			else if (strcmp(name, "TlsCurrTxDevice") == 0)
				stats->curr_tx_device = value;
			else if (strcmp(name, "TlsCurrRxDevice") == 0)
				stats->curr_rx_device = value;
			else if (strcmp(name, "TlsTxSw") == 0)
				stats->tx_sw = value;
			else if (strcmp(name, "TlsRxSw") == 0)
				stats->rx_sw = value;
			else if (strcmp(name, "TlsTxDevice") == 0)
				stats->tx_device = value;
			else if (strcmp(name, "TlsRxDevice") == 0)
				stats->rx_device = value;
			else if (strcmp(name, "TlsDecryptError") == 0)
				stats->decrypt_error = value;
			else if (strcmp(name, "TlsRxDeviceResync") == 0)
				stats->rx_device_resync = value;
			else if (strcmp(name, "TlsDecryptRetry") == 0)
				stats->decrypt_retry = value;
			else if (strcmp(name, "TlsRxNoPadViolation") == 0)
				stats->rx_no_pad_violation = value;
			else if (strcmp(name, "TlsRxRekeyOk") == 0)
				stats->rx_rekey_ok = value;
			else if (strcmp(name, "TlsRxRekeyError") == 0)
				stats->rx_rekey_error = value;
			else if (strcmp(name, "TlsTxRekeyOk") == 0)
				stats->tx_rekey_ok = value;
			else if (strcmp(name, "TlsTxRekeyError") == 0)
				stats->tx_rekey_error = value;
			else if (strcmp(name, "TlsRxRekeyReceived") == 0)
				stats->rx_rekey_received = value;
		}
	}
	fclose(f);
	return 0;
}

/*
 * Verify TLS counters after test completion.
 * Returns 0 on success, -1 on failure.
 * expected_rekeys: number of rekeys that were expected
 * actual_rekeys: number of rekeys that were actually completed
 * test_failed: whether the data exchange test already failed
 */
static int verify_tls_counters(struct tls_stats *before,
			       struct tls_stats *after,
			       int expected_rekeys, int actual_rekeys,
			       int is_server, int test_failed)
{
	long decrypt_err_diff = after->decrypt_error - before->decrypt_error;
	long tx_device_diff = after->tx_device - before->tx_device;
	long rx_device_diff = after->rx_device - before->rx_device;
	long tx_sw_diff = after->tx_sw - before->tx_sw;
	long rx_sw_diff = after->rx_sw - before->rx_sw;
	int used_tx_hw = (tx_device_diff >= 1);
	int used_rx_hw = (rx_device_diff >= 1);
	int used_tx_sw = (tx_sw_diff >= 1);
	int used_rx_sw = (rx_sw_diff >= 1);
	int errors = test_failed ? 1 : 0;

	printf("\n=== Counter Verification (%s) ===\n",
	       is_server ? "Server" : "Client");

	if (test_failed)
		printf("Data exchange: FAILED\n");

	/* Check that TLS was used (either HW or SW path) */
	printf("TlsTxDevice: %ld -> %ld (diff: %ld)\n",
	       before->tx_device, after->tx_device, tx_device_diff);
	printf("TlsTxSw: %ld -> %ld (diff: %ld)\n",
	       before->tx_sw, after->tx_sw, tx_sw_diff);
	if (used_tx_hw) {
		printf("TX Path: HARDWARE OFFLOAD\n");
	} else if (used_tx_sw) {
		printf("TX Path: SOFTWARE\n");
	} else {
		printf("TX Path: FAIL (no TLS TX activity detected)\n");
		errors++;
	}

	printf("TlsRxDevice: %ld -> %ld (diff: %ld)\n",
	       before->rx_device, after->rx_device, rx_device_diff);
	printf("TlsRxSw: %ld -> %ld (diff: %ld)\n",
	       before->rx_sw, after->rx_sw, rx_sw_diff);
	if (used_rx_hw) {
		printf("RX Path: HARDWARE OFFLOAD\n");
	} else if (used_rx_sw) {
		printf("RX Path: SOFTWARE\n");
	} else {
		printf("RX Path: FAIL (no TLS RX activity detected)\n");
		errors++;
	}

	/* Check rekey counters if rekeys were expected */
	if (expected_rekeys > 0) {
		long rx_rekey_recv_diff;
		long tx_rekey_err_diff;
		long rx_rekey_err_diff;
		long tx_rekey_diff;
		long rx_rekey_diff;

		tx_rekey_diff = after->tx_rekey_ok - before->tx_rekey_ok;
		rx_rekey_diff = after->rx_rekey_ok - before->rx_rekey_ok;
		rx_rekey_recv_diff = after->rx_rekey_received -
				     before->rx_rekey_received;
		tx_rekey_err_diff = after->tx_rekey_error -
				    before->tx_rekey_error;
		rx_rekey_err_diff = after->rx_rekey_error -
				    before->rx_rekey_error;

		/* Check that we completed the expected number of rekeys */
		printf("Rekeys completed: %d/%d ",
		       actual_rekeys, expected_rekeys);
		if (actual_rekeys >= expected_rekeys) {
			printf("PASS\n");
		} else {
			printf("FAIL\n");
			errors++;
		}

		printf("TlsTxRekeyOk: %ld -> %ld (diff: %ld) ",
		       before->tx_rekey_ok, after->tx_rekey_ok, tx_rekey_diff);
		if (tx_rekey_diff >= expected_rekeys) {
			printf("PASS\n");
		} else {
			printf("FAIL (expected >= %d)\n", expected_rekeys);
			errors++;
		}

		printf("TlsRxRekeyOk: %ld -> %ld (diff: %ld) ",
		       before->rx_rekey_ok, after->rx_rekey_ok, rx_rekey_diff);
		if (rx_rekey_diff >= expected_rekeys) {
			printf("PASS\n");
		} else {
			printf("FAIL (expected >= %d)\n", expected_rekeys);
			errors++;
		}

		/* Server receives KeyUpdate messages from client */
		if (is_server) {
			printf("TlsRxRekeyReceived: %ld -> %ld (diff: %ld) ",
			       before->rx_rekey_received,
			       after->rx_rekey_received, rx_rekey_recv_diff);
			if (rx_rekey_recv_diff >= expected_rekeys) {
				printf("PASS\n");
			} else {
				printf("FAIL (expected >= %d)\n",
				       expected_rekeys);
				errors++;
			}
		}

		/* Check for rekey errors */
		if (tx_rekey_err_diff > 0) {
			printf("ERROR: TlsTxRekeyError increased by %ld\n",
			       tx_rekey_err_diff);
			errors++;
		}
		if (rx_rekey_err_diff > 0) {
			printf("ERROR: TlsRxRekeyError increased by %ld\n",
			       rx_rekey_err_diff);
			errors++;
		}
	}

	/* Check for decrypt errors */
	if (decrypt_err_diff > 0) {
		printf("ERROR: TlsDecryptError increased by %ld\n",
		       decrypt_err_diff);
		errors++;
	}

	printf("=== Verification %s ===\n\n",
	       errors == 0 ? "PASSED" : "FAILED");
	return errors == 0 ? 0 : -1;
}

/*
 * Derive AES-GCM-128 key for a given generation number.
 * Both sides use the same derivation so keys match.
 * Generation 0 = initial key, Generation N = Nth rekey.
 */
static void derive_key_128(struct tls12_crypto_info_aes_gcm_128 *key,
			   int generation)
{
	unsigned char pattern;
	int i;

	/* Start with initial key */
	memcpy(key, &tls_info_key0_128, sizeof(*key));

	/* Set TLS version based on global setting */
	if (tls_version == 12)
		key->info.version = TLS_1_2_VERSION;
	else
		key->info.version = TLS_1_3_VERSION;

	if (generation == 0)
		return;

	/* Derive new key by XORing with generation-based pattern */
	pattern = (unsigned char)((generation * 0x1B) ^ 0x63);

	for (i = 0; i < TLS_CIPHER_AES_GCM_128_KEY_SIZE; i++) {
		key->key[i] ^= pattern;
		pattern = (pattern << 1) | (pattern >> 7);  /* Rotate */
	}

	pattern = (unsigned char)((generation * 0x2D) ^ 0x7C);
	for (i = 0; i < TLS_CIPHER_AES_GCM_128_IV_SIZE; i++) {
		key->iv[i] ^= pattern;
		pattern = (pattern << 1) | (pattern >> 7);
	}

	for (i = 0; i < TLS_CIPHER_AES_GCM_128_SALT_SIZE; i++)
		key->salt[i] ^= (unsigned char)(generation & 0xFF);

	/* Reset record sequence for new key */
	memset(key->rec_seq, 0, TLS_CIPHER_AES_GCM_128_REC_SEQ_SIZE);
}

/*
 * Derive AES-GCM-256 key for a given generation number.
 */
static void derive_key_256(struct tls12_crypto_info_aes_gcm_256 *key,
			   int generation)
{
	unsigned char pattern;
	int i;

	/* Start with initial key */
	memcpy(key, &tls_info_key0_256, sizeof(*key));

	/* Set TLS version based on global setting */
	if (tls_version == 12)
		key->info.version = TLS_1_2_VERSION;
	else
		key->info.version = TLS_1_3_VERSION;

	if (generation == 0)
		return;

	/* Derive new key by XORing with generation-based pattern */
	pattern = (unsigned char)((generation * 0x1B) ^ 0x63);

	for (i = 0; i < TLS_CIPHER_AES_GCM_256_KEY_SIZE; i++) {
		key->key[i] ^= pattern;
		pattern = (pattern << 1) | (pattern >> 7);  /* Rotate */
	}

	pattern = (unsigned char)((generation * 0x2D) ^ 0x7C);
	for (i = 0; i < TLS_CIPHER_AES_GCM_256_IV_SIZE; i++) {
		key->iv[i] ^= pattern;
		pattern = (pattern << 1) | (pattern >> 7);
	}

	for (i = 0; i < TLS_CIPHER_AES_GCM_256_SALT_SIZE; i++)
		key->salt[i] ^= (unsigned char)(generation & 0xFF);

	/* Reset record sequence for new key */
	memset(key->rec_seq, 0, TLS_CIPHER_AES_GCM_256_REC_SEQ_SIZE);
}

/* Return human-readable cipher name for logging */
static const char *cipher_name(int cipher)
{
	switch (cipher) {
	case 128: return "AES-GCM-128";
	case 256: return "AES-GCM-256";
	default: return "unknown";
	}
}

/* Return human-readable TLS version name for logging */
static const char *version_name(int version)
{
	switch (version) {
	case 12: return "TLS 1.2";
	case 13: return "TLS 1.3";
	default: return "unknown";
	}
}

/* Print all TLS statistics from /proc/net/tls_stat */
static void print_tls_stats(const char *label)
{
	char line[256];
	FILE *f;

	printf("=== %s ===\n", label);
	f = fopen("/proc/net/tls_stat", "r");
	if (!f) {
		printf("Cannot read /proc/net/tls_stat\n");
		return;
	}
	while (fgets(line, sizeof(line), f))
		printf("%s", line);
	fclose(f);
	printf("\n");
}

/* Enable kTLS by setting TCP Upper Layer Protocol to "tls" */
static int setup_tls_ulp(int fd)
{
	int ret;

	ret = setsockopt(fd, IPPROTO_TCP, TCP_ULP, "tls", sizeof("tls"));
	if (ret < 0) {
		printf("TCP_ULP failed: %s\n", strerror(errno));
		return -1;
	}
	return 0;
}

/*
 * Install TLS key for TX or RX direction.
 * Derives key material for the given generation and installs it via setsockopt.
 */
static int setup_tls_key(int fd, int is_tx, int generation, int cipher)
{
	int ret;

	if (cipher == 256) {
		struct tls12_crypto_info_aes_gcm_256 key;

		derive_key_256(&key, generation);
		ret = setsockopt(fd, SOL_TLS, is_tx ? TLS_TX : TLS_RX,
				 &key, sizeof(key));
	} else {
		struct tls12_crypto_info_aes_gcm_128 key;

		derive_key_128(&key, generation);
		ret = setsockopt(fd, SOL_TLS, is_tx ? TLS_TX : TLS_RX,
				 &key, sizeof(key));
	}

	if (ret < 0) {
		printf("TLS_%s %s (gen %d) failed: %s\n",
		       is_tx ? "TX" : "RX", cipher_name(cipher),
		       generation, strerror(errno));
		return -1;
	}

	printf("TLS_%s %s gen %d installed\n",
	       is_tx ? "TX" : "RX", cipher_name(cipher), generation);
	return 0;
}

/*
 * Send a TLS 1.3 KeyUpdate handshake message via kTLS.
 *
 * This signals to the peer's kernel kTLS layer that we are updating
 * our TX key. The peer must receive this before updating their RX key.
 *
 * KeyUpdate message format (RFC 8446):
 *   HandshakeType: key_update (24/0x18)  - 1 byte
 *   Length: 1                            - 3 bytes (24-bit)
 *   KeyUpdateRequest: 0 or 1             - 1 byte
 * Total: 5 bytes
 */
static int send_tls_key_update(int fd, int request_update)
{
	char cmsg_buf[CMSG_SPACE(sizeof(unsigned char))];
	unsigned char key_update_msg[5];
	struct msghdr msg = {0};
	struct cmsghdr *cmsg;
	struct iovec iov;

	/* Build TLS 1.3 KeyUpdate handshake message */
	key_update_msg[0] = TLS_HANDSHAKE_KEY_UPDATE;  /* HandshakeType */
	key_update_msg[1] = 0;                         /* Length (24-bit) */
	key_update_msg[2] = 0;
	key_update_msg[3] = 1;                         /* Length = 1 */
	key_update_msg[4] = request_update ? KEY_UPDATE_REQUESTED
					   : KEY_UPDATE_NOT_REQUESTED;

	iov.iov_base = key_update_msg;
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

/*
 * Receive a TLS message and get its record type via cmsg.
 * Returns bytes received, or -1 on error.
 */
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

	*record_type = TLS_RECORD_TYPE_APPLICATION_DATA;  /* default */

	cmsg = CMSG_FIRSTHDR(&msg);
	if (cmsg && cmsg->cmsg_level == SOL_TLS &&
	    cmsg->cmsg_type == TLS_GET_RECORD_TYPE)
		*record_type = *((unsigned char *)CMSG_DATA(cmsg));

	return ret;
}

/*
 * Receive and verify a TLS KeyUpdate handshake message.
 * Returns 0 on success, -1 on error.
 */
static int recv_tls_keyupdate(int fd)
{
	int record_type;
	char buf[16];
	int ret;

	ret = recv_tls_message(fd, buf, sizeof(buf), &record_type);
	if (ret < 0) {
		printf("recv_tls_message failed: %s\n", strerror(errno));
		return -1;
	}

	if (record_type != TLS_RECORD_TYPE_HANDSHAKE) {
		printf("Expected handshake record (0x%02x), got 0x%02x\n",
		       TLS_RECORD_TYPE_HANDSHAKE, record_type);
		return -1;
	}

	if (ret >= 1 && buf[0] == TLS_HANDSHAKE_KEY_UPDATE) {
		printf("Received TLS KeyUpdate handshake (%d bytes)\n", ret);
		return 0;
	}

	printf("Expected KeyUpdate (0x%02x), got 0x%02x\n",
	       TLS_HANDSHAKE_KEY_UPDATE, (unsigned char)buf[0]);
	return -1;
}

/*
 * Check for EKEYEXPIRED after receiving KeyUpdate.
 * The kernel returns this to signal it's waiting for RX key update.
 */
static void check_ekeyexpired(int fd)
{
	char buf[16];
	int ret;

	ret = recv(fd, buf, sizeof(buf), MSG_DONTWAIT);
	if (ret == -1 && errno == EKEYEXPIRED)
		printf("recv() returned EKEYEXPIRED as expected\n");
	else if (ret == -1 && errno == EAGAIN)
		printf("recv() returned EAGAIN (no pending data)\n");
	else if (ret == -1)
		printf("recv() returned error: %s\n", strerror(errno));
}

/*
 * Update kTLS key (TX or RX direction) for a given generation.
 */
static int do_tls_rekey(int fd, int is_tx, int generation, int cipher)
{
	int ret;

	printf("Performing TLS_%s %s rekey to generation %d...\n",
	       is_tx ? "TX" : "RX", cipher_name(cipher), generation);

	if (cipher == 256) {
		struct tls12_crypto_info_aes_gcm_256 key;

		derive_key_256(&key, generation);
		ret = setsockopt(fd, SOL_TLS, is_tx ? TLS_TX : TLS_RX,
				 &key, sizeof(key));
	} else {
		struct tls12_crypto_info_aes_gcm_128 key;

		derive_key_128(&key, generation);
		ret = setsockopt(fd, SOL_TLS, is_tx ? TLS_TX : TLS_RX,
				 &key, sizeof(key));
	}

	if (ret < 0) {
		printf("TLS_%s %s rekey failed: %s\n", is_tx ? "TX" : "RX",
		       cipher_name(cipher), strerror(errno));
		return -1;
	}
	printf("TLS_%s %s rekey to gen %d successful!\n",
	       is_tx ? "TX" : "RX", cipher_name(cipher), generation);
	return 0;
}

static int do_client(void)
{
	struct tls_stats stats_before, stats_after;
	char *buf = NULL, *echo_buf = NULL;
	int max_size, rekey_interval;
	ssize_t echo_total, echo_n;
	int csk = -1, ret, i, j;
	struct sockaddr_in sa;
	int test_result = 0;
	int current_gen = 0;
	int next_rekey_at;
	ssize_t n;

	if (!server_ip) {
		printf("ERROR: Client requires -s <ip> option\n");
		return -1;
	}

	/* Allocate buffers based on max possible size */
	max_size = random_size_max > 0 ? random_size_max : send_size;
	buf = malloc(max_size);
	echo_buf = malloc(max_size);
	if (!buf || !echo_buf) {
		printf("failed to allocate buffers\n");
		test_result = -1;
		goto out;
	}

	/* Capture counters before test */
	if (read_tls_stats(&stats_before) < 0) {
		printf("ERROR: Cannot read TLS stats for verification\n");
		test_result = -1;
		goto out;
	}

	print_tls_stats("Client Before");

	csk = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (csk < 0) {
		printf("failed to create socket: %s\n", strerror(errno));
		test_result = -1;
		goto out;
	}

	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_addr.s_addr = inet_addr(server_ip);
	sa.sin_port = htons(server_port);
	printf("Connecting to %s:%d...\n", server_ip, server_port);

	ret = connect(csk, (struct sockaddr *)&sa, sizeof(sa));
	if (ret < 0) {
		printf("connect failed: %s\n", strerror(errno));
		test_result = -1;
		goto out;
	}
	printf("Connected!\n");

	/* Setup TLS ULP first */
	if (setup_tls_ulp(csk) < 0) {
		test_result = -1;
		goto out;
	}

	/* Setup TLS TX and RX with initial key (generation 0) */
	if (setup_tls_key(csk, 1, 0, cipher_type) < 0) {  /* TLS_TX, key0 */
		test_result = -1;
		goto out;
	}
	if (setup_tls_key(csk, 0, 0, cipher_type) < 0) {  /* TLS_RX, key0 */
		test_result = -1;
		goto out;
	}

	if (do_rekey)
		printf("TLS %s setup complete. Will perform %d rekey(s).\n",
		       cipher_name(cipher_type), num_rekeys);
	else
		printf("TLS setup complete.\n");

	if (random_size_max > 0)
		printf("Sending %d messages of random size (1..%d bytes)...\n",
		       TEST_ITERATIONS, random_size_max);
	else
		printf("Sending %d messages of %d bytes...\n",
		       TEST_ITERATIONS, send_size);

	/*
	 * Calculate rekey interval to spread rekeys evenly across messages.
	 * With N rekeys and M messages, rekey every M/(N+1) messages.
	 */
	rekey_interval = TEST_ITERATIONS / (num_rekeys + 1);
	if (rekey_interval < 1)
		rekey_interval = 1;
	next_rekey_at = rekey_interval;

	/* Send test data */
	for (i = 0; i < TEST_ITERATIONS; i++) {
		int this_size;

		/* Determine size for this message */
		if (random_size_max > 0)
			this_size = (rand() % random_size_max) + 1;
		else
			this_size = send_size;

		/* Fill buffer with random data */
		for (j = 0; j < this_size; j++)
			buf[j] = rand() & 0xFF;

		n = send(csk, buf, this_size, 0);
		if (n != this_size) {
			printf("FAIL: send failed: %s\n", strerror(errno));
			test_result = -1;
			break;
		}
		printf("Sent %zd bytes (iteration %d)\n", n, i + 1);

		/* Wait for echo from server - may need multiple recv() calls */
		echo_total = 0;
		while (echo_total < n) {
			echo_n = recv(csk, echo_buf + echo_total,
				      n - echo_total, 0);
			if (echo_n < 0) {
				printf("FAIL: Echo recv failed: %s\n",
				       strerror(errno));
				test_result = -1;
				break;
			}
			if (echo_n == 0) {
				printf("FAIL: Connection closed during echo\n");
				test_result = -1;
				break;
			}
			echo_total += echo_n;
		}
		if (test_result != 0)
			break;
		/* Verify echo data matches what we sent */
		if (memcmp(buf, echo_buf, n) != 0) {
			printf("FAIL: Echo data mismatch!\n");
			test_result = -1;
			break;
		}
		printf("Received echo %zd bytes (ok)\n", echo_total);

		/*
		 * Perform rekey at intervals if enabled.
		 *
		 * kTLS Rekey Protocol (client side):
		 * 1. Send TLS KeyUpdate handshake message (with OLD TX key)
		 * 2. Update TX key via setsockopt
		 * 3. Wait for server's KeyUpdate response
		 * 4. Update RX key via setsockopt
		 */
		if (do_rekey && rekeys_done < num_rekeys &&
		    (i + 1) == next_rekey_at) {
			current_gen++;
			printf("\n=== Client Rekey #%d (gen %d) ===\n",
			       rekeys_done + 1, current_gen);
			print_tls_stats("Before Rekey");

			/* Step 1: Send KeyUpdate to server */
			printf("Step 1: Sending TLS KeyUpdate to server\n");
			ret = send_tls_key_update(csk, KEY_UPDATE_REQUESTED);
			if (ret < 0) {
				printf("FAIL: send KeyUpdate\n");
				test_result = -1;
				break;
			}

			/* Step 2: Update client TX key */
			printf("Step 2: Updating client TX key\n");
			ret = do_tls_rekey(csk, 1, current_gen, cipher_type);
			if (ret < 0) {
				test_result = -1;
				break;
			}

			/* Step 3: Wait for server's KeyUpdate */
			printf("Step 3: Waiting for server's KeyUpdate\n");
			if (recv_tls_keyupdate(csk) < 0) {
				printf("FAIL: recv KeyUpdate from server\n");
				test_result = -1;
				break;
			}

			/* Check for EKEYEXPIRED */
			check_ekeyexpired(csk);

			/* Step 4: Update client RX key */
			printf("Step 4: Updating client RX key\n");
			ret = do_tls_rekey(csk, 0, current_gen, cipher_type);
			if (ret < 0) {
				test_result = -1;
				break;
			}

			rekeys_done++;
			next_rekey_at += rekey_interval;
			print_tls_stats("After Rekey");
			printf("=== Client Rekey #%d Complete ===\n\n",
			       rekeys_done);
		}
	}

	/* Check that all iterations completed */
	if (i < TEST_ITERATIONS && test_result == 0) {
		printf("FAIL: Only %d of %d iterations\n", i, TEST_ITERATIONS);
		test_result = -1;
	}

	close(csk);
	csk = -1;
	print_tls_stats("Client After");
	if (do_rekey)
		printf("Rekeys completed: %d/%d\n", rekeys_done, num_rekeys);

	/* Capture counters after test and verify */
	if (read_tls_stats(&stats_after) < 0) {
		printf("ERROR: Cannot read TLS stats after test\n");
		test_result = -1;
	} else {
		if (verify_tls_counters(&stats_before, &stats_after,
					do_rekey ? num_rekeys : 0,
					rekeys_done, 0, test_result != 0) < 0)
			test_result = -1;
	}

out:
	if (csk >= 0)
		close(csk);
	free(buf);
	free(echo_buf);
	return test_result;
}

static int do_server(void)
{
	struct tls_stats stats_before, stats_after;
	int lsk = -1, csk = -1, ret;
	ssize_t n, total = 0, sent;
	struct sockaddr_in sa;
	int current_gen = 0;
	int test_result = 0;
	int recv_count = 0;
	char *buf = NULL;
	int record_type;
	int max_size;
	int one = 1;

	/* Allocate buffer based on max possible size */
	max_size = random_size_max > 0 ? random_size_max : send_size;
	buf = malloc(max_size);
	if (!buf) {
		printf("failed to allocate buffer\n");
		test_result = -1;
		goto out;
	}

	/* Capture counters before test */
	if (read_tls_stats(&stats_before) < 0) {
		printf("ERROR: Cannot read TLS stats for verification\n");
		test_result = -1;
		goto out;
	}

	print_tls_stats("Server Before");

	lsk = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (lsk < 0) {
		printf("failed to create socket: %s\n", strerror(errno));
		test_result = -1;
		goto out;
	}

	setsockopt(lsk, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

	/* Bind to INADDR_ANY:PORT */
	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_addr.s_addr = INADDR_ANY;
	sa.sin_port = htons(server_port);

	ret = bind(lsk, (struct sockaddr *)&sa, sizeof(sa));
	if (ret < 0) {
		printf("bind failed: %s\n", strerror(errno));
		test_result = -1;
		goto out;
	}

	ret = listen(lsk, 5);
	if (ret < 0) {
		printf("listen failed: %s\n", strerror(errno));
		test_result = -1;
		goto out;
	}

	printf("Server listening on port %d\n", server_port);
	printf("Waiting for client connection...\n");

	csk = accept(lsk, (struct sockaddr *)NULL, (socklen_t *)NULL);
	if (csk < 0) {
		printf("accept failed: %s\n", strerror(errno));
		test_result = -1;
		goto out;
	}
	printf("Client connected!\n");

	/* Setup TLS ULP first */
	if (setup_tls_ulp(csk) < 0) {
		test_result = -1;
		goto out;
	}

	/* Setup TLS TX and RX with initial key (generation 0) */
	if (setup_tls_key(csk, 1, 0, cipher_type) < 0) {  /* TLS_TX, key0 */
		test_result = -1;
		goto out;
	}
	if (setup_tls_key(csk, 0, 0, cipher_type) < 0) {  /* TLS_RX, key0 */
		test_result = -1;
		goto out;
	}

	printf("TLS %s setup complete. Receiving...\n",
	       cipher_name(cipher_type));

	/*
	 * Main receive loop using recvmsg to detect KeyUpdate messages.
	 *
	 * kTLS Rekey Protocol (server side):
	 * 1. Receive TLS KeyUpdate handshake from client
	 * 2. Check for EKEYEXPIRED
	 * 3. Update RX key via setsockopt
	 * 4. Send TLS KeyUpdate back to client
	 * 5. Update TX key via setsockopt
	 *
	 * Per kernel kTLS test pattern (from selftests/net/tls.c):
	 * - First try plain recv with MSG_PEEK | MSG_DONTWAIT
	 * - If it fails with EIO/ENOMSG, a handshake record is pending
	 * - Use recvmsg with cmsg to get the actual record type
	 */
	while (1) {
		/*
		 * First try plain recv - this fails for non-data records.
		 * This pattern is from tls_keyupdate_test.c which works.
		 */
		n = recv(csk, buf, max_size, MSG_PEEK | MSG_DONTWAIT);
		if (n < 0 &&
		    (errno == EIO || errno == ENOMSG || errno == EAGAIN)) {
			/* Handshake record or no data - use recvmsg */
			if (errno != EAGAIN)
				printf("DEBUG: recv -1 (errno=%d: %s)\n",
				       errno, strerror(errno));
			n = recv_tls_message(csk, buf, max_size, &record_type);
		} else if (n > 0) {
			/* Application data - receive it properly */
			n = recv_tls_message(csk, buf, max_size, &record_type);
		} else if (n == 0) {
			printf("Connection closed by client\n");
			break;
		}

		/* Other error from MSG_PEEK recv */
		if (n < 0) {
			printf("recv failed: %s\n", strerror(errno));
			break;
		}

		if (n <= 0) {
			if (n == 0)
				printf("Connection closed by client\n");
			else
				printf("recv_tls_message: %s\n",
				       strerror(errno));
			break;
		}

		/* Check if we received a TLS KeyUpdate handshake message */
		if (record_type == TLS_RECORD_TYPE_HANDSHAKE &&
		    n >= 1 && buf[0] == TLS_HANDSHAKE_KEY_UPDATE) {
			current_gen++;
			printf("\n=== Server Rekey #%d (gen %d) ===\n",
			       rekeys_done + 1, current_gen);
			printf("Received KeyUpdate from client (%zd bytes)\n",
			       n);
			print_tls_stats("Before Rekey");

			/* Step 1: Check for EKEYEXPIRED */
			printf("Step 1: Checking for EKEYEXPIRED\n");
			check_ekeyexpired(csk);

			/* Step 2: Update server RX key */
			printf("Step 2: Updating server RX key\n");
			ret = do_tls_rekey(csk, 0, current_gen, cipher_type);
			if (ret < 0) {
				test_result = -1;
				break;
			}

			/* Step 3: Send KeyUpdate back to client */
			printf("Step 3: Sending TLS KeyUpdate to client\n");
			ret = send_tls_key_update(csk,
						  KEY_UPDATE_NOT_REQUESTED);
			if (ret < 0) {
				printf("Failed to send KeyUpdate\n");
				test_result = -1;
				break;
			}

			/* Step 4: Update server TX key */
			printf("Step 4: Updating server TX key\n");
			ret = do_tls_rekey(csk, 1, current_gen, cipher_type);
			if (ret < 0) {
				test_result = -1;
				break;
			}

			rekeys_done++;
			print_tls_stats("After Rekey");
			printf("=== Server Rekey #%d Complete ===\n\n",
			       rekeys_done);
			continue;
		}

		/* Application data */
		total += n;
		recv_count++;
		printf("Received %zd bytes (total: %zd, count: %d)\n",
		       n, total, recv_count);

		/* Echo data back to client */
		sent = send(csk, buf, n, 0);
		if (sent < 0) {
			printf("Echo send failed: %s\n", strerror(errno));
			break;
		}
		if (sent != n)
			printf("Echo partial: %zd of %zd bytes\n", sent, n);
		printf("Echoed %zd bytes back to client\n", sent);
	}

	printf("Connection closed. Total received: %zd bytes\n", total);
	if (do_rekey)
		printf("Rekeys completed: %d\n", rekeys_done);

	close(csk);
	csk = -1;
	close(lsk);
	lsk = -1;
	print_tls_stats("Server After");

	/* Capture counters after test and verify */
	if (read_tls_stats(&stats_after) < 0) {
		printf("ERROR: Cannot read TLS stats after test\n");
		test_result = -1;
	} else {
		if (verify_tls_counters(&stats_before, &stats_after,
					do_rekey ? num_rekeys : 0,
					rekeys_done, 1, test_result != 0) < 0)
			test_result = -1;
	}

out:
	if (csk >= 0)
		close(csk);
	if (lsk >= 0)
		close(lsk);
	free(buf);
	return test_result;
}

static void parse_rekey_option(const char *arg)
{
	int requested;

	/* Parse --rekey or --rekey=N */
	if (strncmp(arg, "--rekey=", 8) == 0) {
		requested = atoi(arg + 8);
		if (requested < 1) {
			printf("WARNING: Invalid rekey count, using 1\n");
			num_rekeys = 1;
		} else if (requested > MAX_REKEYS) {
			printf("WARNING: Rekey count %d > max %d, using %d\n",
			       requested, MAX_REKEYS, MAX_REKEYS);
			num_rekeys = MAX_REKEYS;
		} else {
			num_rekeys = requested;
		}
		do_rekey = 1;
	} else if (strcmp(arg, "--rekey") == 0) {
		do_rekey = 1;
		num_rekeys = 1;
	}
}

static int parse_cipher_option(const char *arg)
{
	/* Parse -c <cipher> where cipher is 128 or 256 */
	if (strcmp(arg, "128") == 0) {
		cipher_type = 128;
		return 0;
	} else if (strcmp(arg, "256") == 0) {
		cipher_type = 256;
		return 0;
	}
	printf("ERROR: Invalid cipher '%s'. Must be 128 or 256.\n", arg);
	return -1;
}

static int parse_version_option(const char *arg)
{
	/* Parse -v <version> where version is 1.2 or 1.3 */
	if (strcmp(arg, "1.2") == 0) {
		tls_version = 12;
		return 0;
	} else if (strcmp(arg, "1.3") == 0) {
		tls_version = 13;
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
	printf("  -s <ip>       Server IP to connect (client, required)\n");
	printf("  -p <port>     Server port (default: 4433)\n");
	printf("  -b <size>     Send buffer (record) size (default: 16384)\n");
	printf("  -r <max>      Use random send buffer sizes (1..<max>)\n");
	printf("  -v <version>  TLS version: 1.2 or 1.3 (default: 1.3)\n");
	printf("  -c <cipher>   Cipher: 128 or 256 (default: 128)\n");
	printf("  --rekey[=N]   Enable rekey (default: 1, TLS 1.3 only)\n");
	printf("  --help        Show this help message\n");
	printf("\nExample:\n");
	printf("  Node A: %s server\n", prog);
	printf("  Node B: %s client -s 192.168.20.2\n", prog);
	printf("\nRekey Example (3 rekeys, TLS 1.3 only):\n");
	printf("  Node A: %s server --rekey=3\n", prog);
	printf("  Node B: %s client -s 192.168.20.2 --rekey=3\n", prog);
}

int main(int argc, char *argv[])
{
	int i;

	/* Check for --help first */
	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--help") == 0 ||
		    strcmp(argv[i], "-h") == 0) {
			print_usage(argv[0]);
			return 0;
		}
	}

	/* Parse options anywhere in args */
	for (i = 1; i < argc; i++) {
		parse_rekey_option(argv[i]);
		if (strcmp(argv[i], "-s") == 0 && i + 1 < argc)
			server_ip = argv[i + 1];
		if (strcmp(argv[i], "-p") == 0 && i + 1 < argc)
			server_port = atoi(argv[i + 1]);
		if (strcmp(argv[i], "-b") == 0 && i + 1 < argc) {
			send_size = atoi(argv[i + 1]);
			if (send_size < 1)
				send_size = 1;
		}
		if (strcmp(argv[i], "-r") == 0 && i + 1 < argc) {
			random_size_max = atoi(argv[i + 1]);
			if (random_size_max < 1)
				random_size_max = 1;
		}
		if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
			if (parse_cipher_option(argv[i + 1]) < 0)
				return -1;
		}
		if (strcmp(argv[i], "-v") == 0 && i + 1 < argc) {
			if (parse_version_option(argv[i + 1]) < 0)
				return -1;
		}
	}

	/* TLS 1.2 does not support rekey - warn and disable */
	if (tls_version == 12 && do_rekey) {
		printf("WARNING: TLS 1.2 does not support rekey\n");
		printf("         (KeyUpdate is TLS 1.3 only)\n");
		do_rekey = 0;
	}

	printf("TLS Version: %s\n", version_name(tls_version));
	printf("Cipher: %s\n", cipher_name(cipher_type));
	if (random_size_max > 0)
		printf("Buffer size: random (1..%d)\n", random_size_max);
	else
		printf("Buffer size: %d\n", send_size);

	if (do_rekey)
		printf("Rekey testing ENABLED: %d rekey(s)\n", num_rekeys);

	/* Initialize random seed for random data and buffer sizes */
	srand(time(NULL));

	if (argc < 2 ||
	    (strcmp(argv[1], "server") && strcmp(argv[1], "client"))) {
		print_usage(argv[0]);
		return -1;
	}

	if (!strcmp(argv[1], "client"))
		return do_client();

	return do_server();
}
