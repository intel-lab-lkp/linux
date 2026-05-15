// SPDX-License-Identifier: GPL-2.0
/*
 * pipe_bench - exercise pipe->mutex contention under concurrent writers.
 *
 * N writer threads hammer a single pipe with multi-page writes; M reader
 * threads drain it. Each writer records its own write() latency histogram.
 * Multi-page writes (msgsize >= PAGE_SIZE) force the loop in
 * anon_pipe_write() to call alloc_page(GFP_HIGHUSER | __GFP_ACCOUNT) under
 * pipe->mutex, which is the critical section the patch shrinks.
 *
 * By default the benchmark sweeps writers in {1, 2, 5} x readers in
 * {1, 5, 10} and prints one block per configuration so two runs (e.g.
 * baseline vs patched) can be diffed directly. Pass -w and -r to run a
 * single configuration instead. Pass --memory-pressure to spawn stress-ng
 * alongside the sweep so the per-page alloc_page() path under pipe->mutex
 * has to dip into reclaim.
 *
 * Copyright (c) 2026 Meta Platforms, Inc. and affiliates
 * Copyright (c) 2026 Breno Leitao <leitao@debian.org>
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define ARRAY_SIZE(a)	(sizeof(a) / sizeof((a)[0]))
#define HIST_BUCKETS	32

static size_t g_msgsize = 16 * 4096;
static int g_duration = 3;
static int g_pipe_size = 1024 * 1024;
static int g_memory_pressure;

static atomic_int g_stop;
static int g_pipe[2];

struct wstats {
	uint64_t writes;
	uint64_t bytes;
	uint64_t lat_sum_ns;
	uint64_t lat_max_ns;
	uint64_t lat_hist[HIST_BUCKETS];
};

static inline uint64_t now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

static inline int log2_bucket(uint64_t v)
{
	int b = 0;

	if (!v)
		return 0;
	while (v >>= 1)
		b++;
	return b < HIST_BUCKETS ? b : HIST_BUCKETS - 1;
}

static void *writer(void *arg)
{
	struct wstats *s = arg;
	char *buf = aligned_alloc(4096, g_msgsize);

	if (!buf)
		return NULL;
	memset(buf, 0xAA, g_msgsize);

	while (!atomic_load_explicit(&g_stop, memory_order_relaxed)) {
		uint64_t t0 = now_ns();
		ssize_t n = write(g_pipe[1], buf, g_msgsize);
		uint64_t dt = now_ns() - t0;

		if (n > 0) {
			s->writes++;
			s->bytes += n;
			s->lat_sum_ns += dt;
			if (dt > s->lat_max_ns)
				s->lat_max_ns = dt;
			s->lat_hist[log2_bucket(dt)]++;
		} else if (n < 0 && errno == EPIPE) {
			break;
		}
	}
	free(buf);
	return NULL;
}

static void *reader(void *arg)
{
	char *buf = aligned_alloc(4096, g_msgsize);

	(void)arg;
	if (!buf)
		return NULL;
	/*
	 * Drain until EOF (write end closed by main). g_stop is not checked
	 * here on purpose: writers may be blocked in write() with the pipe
	 * full when g_stop is set, so the reader must keep draining until
	 * main closes the write end.
	 */
	for (;;) {
		ssize_t n = read(g_pipe[0], buf, g_msgsize);

		if (n <= 0)
			break;
	}
	free(buf);
	return NULL;
}

static void summarize(struct wstats *all, int nw, int nr)
{
	uint64_t total_writes = 0, total_bytes = 0, total_lat = 0;
	uint64_t max_lat = 0;
	uint64_t agg[HIST_BUCKETS] = {0};
	uint64_t p50_target, p99_target, p999_target;
	uint64_t cum = 0, p50 = 0, p99 = 0, p999 = 0;
	uint64_t avg_ns;
	double sec;

	for (int i = 0; i < nw; i++) {
		total_writes += all[i].writes;
		total_bytes += all[i].bytes;
		total_lat += all[i].lat_sum_ns;
		if (all[i].lat_max_ns > max_lat)
			max_lat = all[i].lat_max_ns;
		for (int b = 0; b < HIST_BUCKETS; b++)
			agg[b] += all[i].lat_hist[b];
	}

	p50_target = total_writes * 50 / 100;
	p99_target = total_writes * 99 / 100;
	p999_target = total_writes * 999 / 1000;

	for (int b = 0; b < HIST_BUCKETS; b++) {
		cum += agg[b];
		if (!p50 && cum >= p50_target)
			p50 = 1ULL << b;
		if (!p99 && cum >= p99_target)
			p99 = 1ULL << b;
		if (!p999 && cum >= p999_target)
			p999 = 1ULL << b;
	}

	sec = g_duration;
	avg_ns = total_writes ? total_lat / total_writes : 0;

	printf("config: writers=%d readers=%d msgsize=%zu duration=%d pipe_size=%d memory_pressure=%s\n",
	       nw, nr, g_msgsize, g_duration, g_pipe_size,
	       g_memory_pressure ? "yes" : "no");
	printf("writes: total=%llu rate=%.0f/s\n",
	       (unsigned long long)total_writes, total_writes / sec);
	printf("throughput_MBps: %.2f\n",
	       (total_bytes / sec) / (1024.0 * 1024.0));
	printf("lat_avg_ns: %llu\n", (unsigned long long)avg_ns);
	printf("lat_p50_ns_upper: %llu\n", (unsigned long long)p50);
	printf("lat_p99_ns_upper: %llu\n", (unsigned long long)p99);
	printf("lat_p999_ns_upper: %llu\n", (unsigned long long)p999);
	printf("lat_max_ns: %llu\n", (unsigned long long)max_lat);
}

static pid_t spawn_stress_ng(void)
{
	pid_t pid = fork();

	if (pid < 0) {
		perror("fork");
		return -1;
	}
	if (pid == 0) {
		execlp("stress-ng", "stress-ng",
		       "--vm", "4", "--vm-bytes", "80%",
		       "--vm-method", "all",
		       (char *)NULL);
		fprintf(stderr, "exec stress-ng failed: %s\n",
			strerror(errno));
		_exit(127);
	}
	/* Give stress-ng a moment to map its VM regions before measuring. */
	sleep(1);
	return pid;
}

static void kill_stress_ng(pid_t pid)
{
	int status;

	if (pid <= 0)
		return;
	kill(pid, SIGTERM);
	for (int i = 0; i < 20; i++) {
		if (waitpid(pid, &status, WNOHANG) > 0)
			return;
		usleep(100 * 1000);
	}
	kill(pid, SIGKILL);
	waitpid(pid, &status, 0);
}

static int run_one(int nw, int nr)
{
	pthread_t *wt, *rt;
	struct wstats *ws;

	atomic_store(&g_stop, 0);

	if (pipe(g_pipe) < 0) {
		perror("pipe");
		return -1;
	}
	if (fcntl(g_pipe[1], F_SETPIPE_SZ, g_pipe_size) < 0)
		perror("F_SETPIPE_SZ (continuing)");

	wt = calloc(nw, sizeof(*wt));
	rt = calloc(nr, sizeof(*rt));
	ws = calloc(nw, sizeof(*ws));

	if (!wt || !rt || !ws) {
		fprintf(stderr, "alloc failed\n");
		free(wt);
		free(rt);
		free(ws);
		close(g_pipe[0]);
		close(g_pipe[1]);
		return -1;
	}

	for (int i = 0; i < nr; i++)
		pthread_create(&rt[i], NULL, reader, NULL);
	for (int i = 0; i < nw; i++)
		pthread_create(&wt[i], NULL, writer, &ws[i]);

	sleep(g_duration);
	atomic_store(&g_stop, 1);

	/*
	 * Close write end first so any writer blocked in write() gets EPIPE
	 * and exits, and so the readers see EOF after draining.
	 */
	close(g_pipe[1]);
	for (int i = 0; i < nw; i++)
		pthread_join(wt[i], NULL);
	for (int i = 0; i < nr; i++)
		pthread_join(rt[i], NULL);
	close(g_pipe[0]);

	summarize(ws, nw, nr);
	fflush(stdout);

	free(wt);
	free(rt);
	free(ws);
	return 0;
}

int main(int argc, char **argv)
{
	static const int writers_sweep[] = {1, 2, 5};
	static const int readers_sweep[] = {1, 5, 10};
	static const struct option long_opts[] = {
		{"memory-pressure", no_argument, NULL, 'M'},
		{0, 0, 0, 0},
	};
	int writers_override = 0, readers_override = 0;
	pid_t stress_pid = -1;
	int rc = 0, opt;

	while ((opt = getopt_long(argc, argv, "w:r:s:d:p:",
				  long_opts, NULL)) != -1) {
		switch (opt) {
		case 'w':
			writers_override = atoi(optarg);
			break;
		case 'r':
			readers_override = atoi(optarg);
			break;
		case 's':
			g_msgsize = atol(optarg);
			break;
		case 'd':
			g_duration = atoi(optarg);
			break;
		case 'p':
			g_pipe_size = atoi(optarg);
			break;
		case 'M':
			g_memory_pressure = 1;
			break;
		default:
			fprintf(stderr,
				"usage: %s [-w writers] [-r readers] [-s msgsize] [-d secs] [-p pipe_size] [--memory-pressure]\n"
				"  default: sweep writers={1,2,5} x readers={1,5,10}\n"
				"  --memory-pressure: spawn stress-ng (--vm 4 --vm-bytes 80%% --vm-method all) for the run\n",
				argv[0]);
			return 1;
		}
	}

	signal(SIGPIPE, SIG_IGN);
	setvbuf(stdout, NULL, _IOLBF, 0);
	setvbuf(stderr, NULL, _IOLBF, 0);

	fprintf(stderr, "pid=%d\n", getpid());
	fflush(stderr);

	if (g_memory_pressure) {
		stress_pid = spawn_stress_ng();
		if (stress_pid < 0) {
			fprintf(stderr,
				"memory_pressure requested but stress-ng could not be spawned\n");
			return 1;
		}
	}

	if (writers_override > 0 || readers_override > 0) {
		int nw = writers_override > 0 ? writers_override : 1;
		int nr = readers_override > 0 ? readers_override : 1;

		rc = run_one(nw, nr) < 0 ? 1 : 0;
		goto out;
	}

	for (size_t i = 0; i < ARRAY_SIZE(writers_sweep); i++) {
		for (size_t j = 0; j < ARRAY_SIZE(readers_sweep); j++) {
			printf("---\n");
			if (run_one(writers_sweep[i], readers_sweep[j]) < 0) {
				rc = 1;
				goto out;
			}
		}
	}
out:
	kill_stress_ng(stress_pid);
	return rc;
}
