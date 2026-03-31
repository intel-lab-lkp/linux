// SPDX-License-Identifier: GPL-2.0
/*
 * Fast UDP sender/sink for ipv4_addr_lookup benchmarking.
 *
 * Sender mode: sends unconnected UDP packets from many source addresses
 * to stress __ip_dev_find -> inet_lookup_ifaddr_rcu (rhltable_lookup).
 * Each sendto() triggers: ip_route_output_key -> __ip_dev_find -> hash lookup.
 *
 * Sink mode (--sink): minimal C UDP receiver that counts packets received.
 * Not used by default -- the test script uses an iptables DROP rule instead
 * to avoid polluting perf profiles with recv() overhead.  Enable with
 * --sink on the test script command line for packet drop verification.
 *
 * Sender design for low-noise measurement:
 *  - Pre-create all sockets during setup (not timed)
 *  - Tight sendto() loop during measurement (no socket lifecycle overhead)
 *  - Clock check only every 1024 packets (avoid paravirt clock overhead)
 *  - 1 second warm-up to stabilize caches and hash table
 *  - Multiple rounds with per-round statistics (median, min, max, stdev)
 *
 * Usage:
 *   ipv4_addr_lookup_udp_sender <num_addrs> <rounds> <duration_sec>
 *   ipv4_addr_lookup_udp_sender --sink [port]
 *
 * Example: ipv4_addr_lookup_udp_sender 1000 10 3
 *   -> 10 rounds of 3s each (+ 1s warm-up) = ~31s total
 */

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define DST_ADDR	"192.168.1.2"
#define DST_PORT	9000
#define SINK_PORT	DST_PORT
#define SINK_BUF	4096
#define WARMUP_SEC	1
#define CLOCK_INTERVAL	1024	/* check clock every N packets */
#define MAX_ROUNDS	100
#define PAYLOAD_LEN	64

static double ts_diff(struct timespec *a, struct timespec *b)
{
	return (b->tv_sec - a->tv_sec) +
	       (b->tv_nsec - a->tv_nsec) * 1e-9;
}

static int cmp_double(const void *a, const void *b)
{
	double da = *(const double *)a;
	double db = *(const double *)b;

	return (da > db) - (da < db);
}

static void run_round(int *fds, int num_addrs, int duration,
		      struct sockaddr_in *dst, char *payload, int payload_len,
		      long long *out_sent, long long *out_errors,
		      double *out_rate)
{
	struct timespec ts_start, ts_now;
	long long sent = 0, errors = 0;
	double elapsed;
	int i = 0;

	clock_gettime(CLOCK_MONOTONIC, &ts_start);
	for (;;) {
		if (fds[i] >= 0) {
			if (sendto(fds[i], payload, payload_len, 0,
				   (struct sockaddr *)dst,
				   sizeof(*dst)) < 0)
				errors++;
			else
				sent++;
		}
		i = (i + 1) % num_addrs;
		if ((sent & (CLOCK_INTERVAL - 1)) == 0) {
			clock_gettime(CLOCK_MONOTONIC, &ts_now);
			if (ts_diff(&ts_start, &ts_now) >= duration)
				break;
		}
	}

	clock_gettime(CLOCK_MONOTONIC, &ts_now);
	elapsed = ts_diff(&ts_start, &ts_now);

	*out_sent = sent;
	*out_errors = errors;
	*out_rate = elapsed > 0 ? sent / elapsed : 0;
}

static volatile int sink_running = 1;

static void sink_stop(int sig)
{
	sink_running = 0;
}

/* Not used by default -- the test script uses iptables DROP instead to keep
 * perf profiles clean.  Enable with: test_script --sink
 */
static int run_sink(int port)
{
	struct timeval tv = { .tv_sec = 0, .tv_usec = 100000 }; /* 100ms */
	int rcvbuf = 4 * 1024 * 1024; /* 4 MB - prevent drops during bursts */
	struct sigaction sa = { };
	struct sockaddr_in addr;
	long long received = 0;
	char buf[SINK_BUF];
	int fd;

	fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0) {
		perror("socket");
		return 1;
	}

	/* SO_RCVBUFFORCE bypasses net.core.rmem_max (requires root) */
	if (setsockopt(fd, SOL_SOCKET, SO_RCVBUFFORCE, &rcvbuf, sizeof(rcvbuf)))
		setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	addr.sin_addr.s_addr = htonl(INADDR_ANY);

	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		perror("bind");
		close(fd);
		return 1;
	}

	/* Use sigaction without SA_RESTART so recv() returns -EINTR
	 * immediately on signal, rather than being silently restarted.
	 */
	sa.sa_handler = sink_stop;
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);

	fprintf(stderr, "sink: listening on port %d\n", port);

	while (sink_running) {
		if (recv(fd, buf, sizeof(buf), 0) > 0)
			received++;
	}

	/* Drain in-flight packets (e.g. still traversing veth pipe).
	 * SO_RCVTIMEO (100ms) ensures we exit once the queue is idle.
	 */
	while (recv(fd, buf, sizeof(buf), 0) > 0)
		received++;

	close(fd);

	fprintf(stderr, "sink: received %lld packets\n", received);
	/* Parseable output for test script */
	printf("received=%lld\n", received);
	fflush(stdout);
	return 0;
}

/* Create and bind one UDP socket per source address: 10.B2.B3.1
 * Returns the number of successfully bound sockets.
 */
static int setup_sockets(int *fds, int num_addrs, int sndbuf)
{
	struct sockaddr_in src;
	int i, n_ok = 0;

	for (i = 0; i < num_addrs; i++) {
		int idx = i + 1;

		fds[i] = -1;
		memset(&src, 0, sizeof(src));
		src.sin_family = AF_INET;
		/* 10.<high byte>.<low byte>.1 */
		src.sin_addr.s_addr = htonl(0x0a000001 |
					    ((idx & 0xff) << 8) |
					    (((idx >> 8) & 0xff) << 16));

		fds[i] = socket(AF_INET, SOCK_DGRAM, 0);
		if (fds[i] < 0)
			continue;
		if (sndbuf > 0) {
			if (setsockopt(fds[i], SOL_SOCKET, SO_SNDBUFFORCE,
				       &sndbuf, sizeof(sndbuf)))
				setsockopt(fds[i], SOL_SOCKET, SO_SNDBUF,
					   &sndbuf, sizeof(sndbuf));
		}
		if (bind(fds[i], (struct sockaddr *)&src, sizeof(src)) < 0) {
			close(fds[i]);
			fds[i] = -1;
			continue;
		}
		n_ok++;
	}
	return n_ok;
}

/* Warm-up: send for WARMUP_SEC to stabilize caches, hash table, softirq */
static long long run_warmup(int *fds, int num_addrs, struct sockaddr_in *dst,
			    char *payload)
{
	struct timespec ts_start, ts_now;
	long long sent = 0;
	int i = 0;

	clock_gettime(CLOCK_MONOTONIC, &ts_start);
	for (;;) {
		if (fds[i] >= 0) {
			if (sendto(fds[i], payload, PAYLOAD_LEN, 0,
				   (struct sockaddr *)dst, sizeof(*dst)) >= 0)
				sent++;
		}
		i = (i + 1) % num_addrs;
		if ((sent & (CLOCK_INTERVAL - 1)) == 0) {
			clock_gettime(CLOCK_MONOTONIC, &ts_now);
			if (ts_diff(&ts_start, &ts_now) >= WARMUP_SEC)
				break;
		}
	}
	return sent;
}

/* Compute and print summary statistics (parseable by test script).
 * sent= includes warmup so it matches the sink's received count.
 */
static void print_summary(double *rates, int rounds,
			  long long total_sent, long long warmup_sent,
			  long long total_errors)
{
	double median, mean, stdev, sum, sumsq;
	int i;

	qsort(rates, rounds, sizeof(double), cmp_double);

	if (rounds % 2 == 0)
		median = (rates[rounds / 2 - 1] + rates[rounds / 2]) / 2.0;
	else
		median = rates[rounds / 2];

	sum = 0;
	sumsq = 0;
	for (i = 0; i < rounds; i++) {
		sum += rates[i];
		sumsq += rates[i] * rates[i];
	}
	mean = sum / rounds;

	if (rounds > 1) {
		double variance = (sumsq - sum * sum / rounds) /
				  (rounds - 1);

		/* Sqrt via Newton's method (avoids -lm) */
		stdev = variance;
		if (stdev > 0) {
			double s = stdev / 2;

			for (i = 0; i < 20; i++)
				s = (s + variance / s) / 2;
			stdev = s;
		}
	} else {
		stdev = 0;
	}

	printf("sent=%lld warmup=%lld errors=%lld rounds=%d "
	       "rate=%.0f pkt/s median=%.0f min=%.0f max=%.0f stdev=%.0f\n",
	       total_sent + warmup_sent, warmup_sent, total_errors, rounds,
	       mean, median, rates[0], rates[rounds - 1], stdev);
}

/* Prevent CPU C-state transitions for stable benchmark results.
 * Holds /dev/cpu_dma_latency open with value 0 (lowest latency).
 * Returns fd (caller must close), or -1 on failure (non-fatal).
 */
static int set_cpu_dma_latency(void)
{
	int32_t lat = 0;
	int fd;

	fd = open("/dev/cpu_dma_latency", O_WRONLY);
	if (fd < 0)
		return -1;
	if (write(fd, &lat, sizeof(lat)) != sizeof(lat)) {
		close(fd);
		return -1;
	}
	return fd;
}

static int run_sender(int num_addrs, int rounds, int duration, int sndbuf)
{
	long long total_sent = 0, total_errors = 0, warmup_sent;
	long long round_sent, round_errors;
	int *fds, n_ok, i, dma_fd;
	double rates[MAX_ROUNDS];
	char payload[PAYLOAD_LEN];
	struct sockaddr_in dst;
	double round_rate;
	struct rlimit rl;

	if (rounds < 1)
		rounds = 1;
	if (rounds > MAX_ROUNDS)
		rounds = MAX_ROUNDS;

	/* Raise fd limit for high address counts */
	if (num_addrs + 64 > 1024) {
		rl.rlim_cur = num_addrs + 256;
		rl.rlim_max = num_addrs + 256;
		setrlimit(RLIMIT_NOFILE, &rl);
	}

	memset(payload, 'X', sizeof(payload));
	memset(&dst, 0, sizeof(dst));
	dst.sin_family = AF_INET;
	dst.sin_port = htons(DST_PORT);
	inet_pton(AF_INET, DST_ADDR, &dst.sin_addr);

	/* Phase 1: Pre-create and bind all sockets (not timed) */
	fds = calloc(num_addrs, sizeof(int));
	if (!fds) {
		perror("calloc");
		return 1;
	}

	n_ok = setup_sockets(fds, num_addrs, sndbuf);
	fprintf(stderr, "setup: %d/%d sockets bound\n", n_ok, num_addrs);

	dma_fd = set_cpu_dma_latency();
	if (dma_fd >= 0)
		fprintf(stderr, "setup: cpu_dma_latency=0 (C-states disabled)\n");
	if (n_ok == 0) {
		fprintf(stderr, "no sockets created\n");
		free(fds);
		return 1;
	}

	/* Phase 2: Warm-up */
	warmup_sent = run_warmup(fds, num_addrs, &dst, payload);

	/* Phase 3: Measurement rounds */
	for (i = 0; i < rounds; i++) {
		run_round(fds, num_addrs, duration, &dst, payload,
			  PAYLOAD_LEN, &round_sent, &round_errors, &round_rate);
		rates[i] = round_rate;
		total_sent += round_sent;
		total_errors += round_errors;
		fprintf(stderr, "  round %2d: %8.0f pkt/s\n",
			i + 1, round_rate);
	}

	print_summary(rates, rounds, total_sent, warmup_sent, total_errors);

	/* Cleanup */
	if (dma_fd >= 0)
		close(dma_fd);
	for (i = 0; i < num_addrs; i++) {
		if (fds[i] >= 0)
			close(fds[i]);
	}
	free(fds);

	return (total_errors > num_addrs / 10) ? 1 : 0;
}

int main(int argc, char **argv)
{
	int sndbuf = 0;
	int port;

	if (argc >= 2 && strcmp(argv[1], "--sink") == 0) {
		port = (argc >= 3) ? atoi(argv[2]) : SINK_PORT;

		return run_sink(port);
	}

	if (argc < 4) {
		fprintf(stderr,
			"Usage: %s <num_addrs> <rounds> <duration_sec> [--sndbuf bytes]\n"
			"       %s --sink [port]\n",
			argv[0], argv[0]);
		return 1;
	}

	if (argc >= 6 && strcmp(argv[4], "--sndbuf") == 0)
		sndbuf = atoi(argv[5]);

	return run_sender(atoi(argv[1]), atoi(argv[2]), atoi(argv[3]), sndbuf);
}
