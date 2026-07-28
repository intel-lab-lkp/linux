// SPDX-License-Identifier: GPL-2.0
/*
 * kTLS device offload data path exercise, driven by tls.sh.
 *
 * One instance runs as the server and one as the client, each in its own
 * network namespace, connected back to back by a linked netdevsim pair.
 * Both ends enable kTLS and rely on netdevsim's emulated TLS offload, so
 * every record travels through net/tls/tls_device.c rather than the
 * software path.
 *
 * The two processes rendezvous through a shared directory so that neither
 * side sends before the other has installed its RX offload.
 */

#define _GNU_SOURCE

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <linux/tls.h>

#ifndef SOL_TLS
#define SOL_TLS			282
#endif

#ifndef TCP_ULP
#define TCP_ULP			31
#endif

#define BULK_LEN		(200 * 1024)
#define MORE_FRAGS		64
#define SPLICE_FRAG_LEN		4096
#define SPLICE_FRAGS		8

/* TLS_MIN_RECORD_SIZE_LIM and TLS_MAX_PAYLOAD_SIZE, which are not uapi. */
#define REC_LIM_MIN		64
#define REC_LIM_MAX		16384

#define SMALL_RECS		100
#define SMALL_LEN		(REC_LIM_MIN * SMALL_RECS)

#define SYNC_TIMEOUT_MS		20000
#define CONNECT_TIMEOUT_MS	20000

static const char *role;

static void die(const char *what)
{
	fprintf(stderr, "%s: %s: %s\n", role, what, strerror(errno));
	exit(1);
}

static void fail(const char *fmt, ...)
{
	va_list ap;

	fprintf(stderr, "%s: ", role);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fprintf(stderr, "\n");
	exit(1);
}

static void msleep(unsigned int ms)
{
	struct timespec ts = {
		.tv_sec = ms / 1000,
		.tv_nsec = (ms % 1000) * 1000000L,
	};

	nanosleep(&ts, NULL);
}

/* /proc/net/tls_stat is per netns, so both ends can check that their own
 * connection really landed on the device path.
 */
static unsigned long read_tls_stat(const char *name)
{
	char line[256];
	unsigned long val;
	FILE *f;

	f = fopen("/proc/net/tls_stat", "r");
	if (!f)
		die("open /proc/net/tls_stat");

	while (fgets(line, sizeof(line), f)) {
		char key[64];

		if (sscanf(line, "%63s %lu", key, &val) != 2)
			continue;
		if (!strcmp(key, name)) {
			fclose(f);
			return val;
		}
	}

	fclose(f);
	fail("%s not found in /proc/net/tls_stat", name);
	return 0;
}

static void fill_pattern(char *buf, size_t len, unsigned int seed)
{
	size_t i;

	for (i = 0; i < len; i++)
		buf[i] = (char)(seed + i * 31 + (i >> 8) * 7);
}

static void check_pattern(const char *buf, size_t len, unsigned int seed,
			  const char *what)
{
	char *want = malloc(len);
	size_t i;

	if (!want)
		die("malloc");

	fill_pattern(want, len, seed);
	for (i = 0; i < len; i++) {
		if (buf[i] != want[i])
			fail("%s: payload mismatch at byte %zu: got 0x%02x want 0x%02x",
			     what, i, (unsigned char)buf[i],
			     (unsigned char)want[i]);
	}

	free(want);
}

static void write_all(int fd, const char *buf, size_t len)
{
	size_t done = 0;

	while (done < len) {
		ssize_t n = send(fd, buf + done, len - done, 0);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			die("send");
		}
		done += n;
	}
}

static void read_all(int fd, char *buf, size_t len)
{
	size_t done = 0;

	while (done < len) {
		ssize_t n = recv(fd, buf + done, len - done, 0);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			die("recv");
		}
		if (n == 0)
			fail("peer closed after %zu of %zu bytes", done, len);
		done += n;
	}
}

static void enable_ktls(int fd)
{
	struct tls12_crypto_info_aes_gcm_128 ci = {};
	unsigned long tx_before, rx_before;

	tx_before = read_tls_stat("TlsTxDevice");
	rx_before = read_tls_stat("TlsRxDevice");

	if (setsockopt(fd, IPPROTO_TCP, TCP_ULP, "tls", sizeof("tls")))
		die("setsockopt(TCP_ULP, tls)");

	ci.info.version = TLS_1_2_VERSION;
	ci.info.cipher_type = TLS_CIPHER_AES_GCM_128;
	memset(ci.iv, 'i', sizeof(ci.iv));
	memset(ci.key, 'k', sizeof(ci.key));
	memset(ci.salt, 's', sizeof(ci.salt));
	memset(ci.rec_seq, 0, sizeof(ci.rec_seq));

	if (setsockopt(fd, SOL_TLS, TLS_TX, &ci, sizeof(ci)))
		die("setsockopt(TLS_TX)");
	if (setsockopt(fd, SOL_TLS, TLS_RX, &ci, sizeof(ci)))
		die("setsockopt(TLS_RX)");

	/* The whole point of the exercise: refuse to silently fall back to
	 * the software path, otherwise the test would pass without ever
	 * touching tls_device.c.
	 */
	if (read_tls_stat("TlsTxDevice") != tx_before + 1)
		fail("TX did not land on the device path (TlsTxDevice %lu -> %lu)",
		     tx_before, read_tls_stat("TlsTxDevice"));
	if (read_tls_stat("TlsRxDevice") != rx_before + 1)
		fail("RX did not land on the device path (TlsRxDevice %lu -> %lu)",
		     rx_before, read_tls_stat("TlsRxDevice"));
}

static void sync_path(char *out, size_t len, const char *dir, const char *who)
{
	if ((size_t)snprintf(out, len, "%s/%s.ready", dir, who) >= len)
		fail("sync dir path too long");
}

static void rendezvous(const char *dir, const char *me, const char *peer)
{
	char mine[PATH_MAX], theirs[PATH_MAX];
	unsigned int waited = 0;
	int fd;

	sync_path(mine, sizeof(mine), dir, me);
	sync_path(theirs, sizeof(theirs), dir, peer);

	fd = open(mine, O_CREAT | O_WRONLY, 0600);
	if (fd < 0)
		die("create sync file");
	close(fd);

	while (access(theirs, F_OK)) {
		if (waited >= SYNC_TIMEOUT_MS)
			fail("timed out waiting for %s", peer);
		msleep(20);
		waited += 20;
	}
}

/* Both ends stop here with their offload installed and no data sent yet,
 * so that the driver state can be inspected from the outside.
 */
static void wait_for_go(const char *dir)
{
	unsigned int waited = 0;
	char go[PATH_MAX];

	if ((size_t)snprintf(go, sizeof(go), "%s/go", dir) >= sizeof(go))
		fail("sync dir path too long");

	while (access(go, F_OK)) {
		if (waited >= SYNC_TIMEOUT_MS)
			fail("timed out waiting for go");
		msleep(20);
		waited += 20;
	}
}

/* Small writes with MSG_MORE accumulate into one open record before it is
 * pushed, which is the interesting part of tls_push_data().
 */
static void send_msg_more(int fd, unsigned int seed)
{
	char buf[MORE_FRAGS + 1];
	int i;

	fill_pattern(buf, sizeof(buf), seed);

	for (i = 0; i < MORE_FRAGS; i++) {
		if (send(fd, buf + i, 1, MSG_MORE) != 1)
			die("send(MSG_MORE)");
	}
	if (send(fd, buf + MORE_FRAGS, 1, 0) != 1)
		die("send(last)");
}

/* splice() reaches tls_push_data() with MSG_SPLICE_PAGES once
 * TLS_TX_ZEROCOPY_RO is enabled, which is a distinct fragment path.
 */
static void send_splice(int fd, unsigned int seed)
{
	char buf[SPLICE_FRAG_LEN];
	int val = 1;
	int i;

	if (setsockopt(fd, SOL_TLS, TLS_TX_ZEROCOPY_RO, &val, sizeof(val)))
		die("setsockopt(TLS_TX_ZEROCOPY_RO)");

	for (i = 0; i < SPLICE_FRAGS; i++) {
		int p[2];

		fill_pattern(buf, sizeof(buf), seed + i * SPLICE_FRAG_LEN);

		if (pipe(p))
			die("pipe");
		if (write(p[1], buf, sizeof(buf)) != sizeof(buf))
			die("write to pipe");
		if (splice(p[0], NULL, fd, NULL, sizeof(buf),
			   i == SPLICE_FRAGS - 1 ? 0 : SPLICE_F_MORE) !=
		    sizeof(buf))
			die("splice");
		close(p[0]);
		close(p[1]);
	}

	val = 0;
	if (setsockopt(fd, SOL_TLS, TLS_TX_ZEROCOPY_RO, &val, sizeof(val)))
		die("setsockopt(TLS_TX_ZEROCOPY_RO off)");
}

/* A record can be up to 16K, so normally one segment carries a piece of a
 * single record.  Shrinking the limit puts a dozen or so whole records in
 * every segment instead, which is the multi-record path through the driver.
 */
static void send_small_records(int fd, unsigned int seed)
{
	char buf[SMALL_LEN];
	uint16_t limit;

	limit = REC_LIM_MIN;
	if (setsockopt(fd, SOL_TLS, TLS_TX_MAX_PAYLOAD_LEN, &limit,
		       sizeof(limit)))
		die("setsockopt(TLS_TX_MAX_PAYLOAD_LEN)");

	fill_pattern(buf, sizeof(buf), seed);
	write_all(fd, buf, sizeof(buf));

	limit = REC_LIM_MAX;
	if (setsockopt(fd, SOL_TLS, TLS_TX_MAX_PAYLOAD_LEN, &limit,
		       sizeof(limit)))
		die("setsockopt(TLS_TX_MAX_PAYLOAD_LEN restore)");
}

#define SEED_C2S_BULK	0x11
#define SEED_S2C_BULK	0x22
#define SEED_C2S_MORE	0x33
#define SEED_C2S_SPLICE	0x44
#define SEED_C2S_SMALL	0x55

static void run_client(int fd)
{
	char *buf = malloc(BULK_LEN);

	if (!buf)
		die("malloc");

	fill_pattern(buf, BULK_LEN, SEED_C2S_BULK);
	write_all(fd, buf, BULK_LEN);

	read_all(fd, buf, BULK_LEN);
	check_pattern(buf, BULK_LEN, SEED_S2C_BULK, "server -> client bulk");

	send_msg_more(fd, SEED_C2S_MORE);
	send_splice(fd, SEED_C2S_SPLICE);
	send_small_records(fd, SEED_C2S_SMALL);

	/* Wait for the server's verdict before tearing anything down. */
	read_all(fd, buf, 1);
	if (buf[0] != 'k')
		fail("server reported a failure");

	free(buf);
}

static void run_server(int fd)
{
	size_t splice_len = (size_t)SPLICE_FRAG_LEN * SPLICE_FRAGS;
	char *buf = malloc(BULK_LEN);
	char more[MORE_FRAGS + 1];
	char *sbuf;
	char ok = 'k';
	int i;

	sbuf = malloc(splice_len);
	if (!buf || !sbuf)
		die("malloc");

	read_all(fd, buf, BULK_LEN);
	check_pattern(buf, BULK_LEN, SEED_C2S_BULK, "client -> server bulk");

	fill_pattern(buf, BULK_LEN, SEED_S2C_BULK);
	write_all(fd, buf, BULK_LEN);

	read_all(fd, more, sizeof(more));
	check_pattern(more, sizeof(more), SEED_C2S_MORE, "client -> server MSG_MORE");

	read_all(fd, sbuf, splice_len);
	for (i = 0; i < SPLICE_FRAGS; i++)
		check_pattern(sbuf + (size_t)i * SPLICE_FRAG_LEN,
			      SPLICE_FRAG_LEN, SEED_C2S_SPLICE +
			      i * SPLICE_FRAG_LEN, "client -> server splice");

	read_all(fd, buf, SMALL_LEN);
	check_pattern(buf, SMALL_LEN, SEED_C2S_SMALL,
		      "client -> server small records");

	write_all(fd, &ok, 1);

	free(sbuf);
	free(buf);
}

static int do_server(const char *ip, int port, const char *syncdir)
{
	struct sockaddr_in sa = {};
	int lfd, fd, one = 1;

	lfd = socket(AF_INET, SOCK_STREAM, 0);
	if (lfd < 0)
		die("socket");
	if (setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)))
		die("SO_REUSEADDR");

	sa.sin_family = AF_INET;
	sa.sin_port = htons(port);
	if (inet_pton(AF_INET, ip, &sa.sin_addr) != 1)
		fail("bad bind address %s", ip);

	if (bind(lfd, (struct sockaddr *)&sa, sizeof(sa)))
		die("bind");
	if (listen(lfd, 1))
		die("listen");

	fd = accept(lfd, NULL, NULL);
	if (fd < 0)
		die("accept");
	close(lfd);

	enable_ktls(fd);
	rendezvous(syncdir, "server", "client");
	wait_for_go(syncdir);

	run_server(fd);

	close(fd);
	return 0;
}

static int do_client(const char *ip, int port, const char *syncdir)
{
	struct sockaddr_in sa = {};
	unsigned int waited = 0;
	int fd;

	sa.sin_family = AF_INET;
	sa.sin_port = htons(port);
	if (inet_pton(AF_INET, ip, &sa.sin_addr) != 1)
		fail("bad server address %s", ip);

	for (;;) {
		fd = socket(AF_INET, SOCK_STREAM, 0);
		if (fd < 0)
			die("socket");
		if (!connect(fd, (struct sockaddr *)&sa, sizeof(sa)))
			break;
		close(fd);
		if (waited >= CONNECT_TIMEOUT_MS)
			die("connect");
		msleep(20);
		waited += 20;
	}

	enable_ktls(fd);
	rendezvous(syncdir, "client", "server");
	wait_for_go(syncdir);

	run_client(fd);

	close(fd);
	return 0;
}

int main(int argc, char **argv)
{
	int port;

	if (argc != 5) {
		fprintf(stderr,
			"usage: %s server|client <ip> <port> <syncdir>\n",
			argv[0]);
		return 2;
	}

	role = argv[1];
	port = atoi(argv[3]);

	if (!strcmp(role, "server"))
		return do_server(argv[2], port, argv[4]);
	if (!strcmp(role, "client"))
		return do_client(argv[2], port, argv[4]);

	fprintf(stderr, "unknown role %s\n", role);
	return 2;
}
