// SPDX-License-Identifier: GPL-2.0
/*
 *
 * sched-pipe.c
 *
 * pipe: Benchmark for pipe()
 *
 * Based on pipe-test-1m.c by Ingo Molnar <mingo@redhat.com>
 *  http://people.redhat.com/mingo/cfs-scheduler/tools/pipe-test-1m.c
 * Ported to perf by Hitoshi Mitake <mitake@dcl.info.waseda.ac.jp>
 */
#include <subcmd/parse-options.h>
#include <api/fs/fs.h>
#include "bench.h"
#include "util/cgroup.h"

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <sys/wait.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <assert.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/syscall.h>
#include <linux/time64.h>

#include <pthread.h>

struct thread_data {
	int			nr;
	int			pipe_read;
	int			pipe_write;
	bool			cgroup_failed;
	pthread_t		pthread;
};

#define LOOPS_DEFAULT 1000000
static	int			loops = LOOPS_DEFAULT;

/* Use processes by default: */
static bool			threaded;

static struct cgroup *cgrp_send = NULL;
static struct cgroup *cgrp_recv = NULL;

static int parse_two_cgroups(const struct option *opt __maybe_unused,
			     const char *str, int unset __maybe_unused)
{
	char *p = strdup(str);
	char *q;
	int ret = -1;

	if (p == NULL) {
		fprintf(stderr, "memory allocation failure");
		return -1;
	}

	q = strchr(p, ',');
	if (q == NULL) {
		fprintf(stderr, "it should have two cgroup names: %s", p);
		goto out;
	}
	*q = '\0';

	cgrp_send = cgroup__new(p, /*do_open=*/true);
	if (cgrp_send == NULL) {
		fprintf(stderr, "cannot open sender cgroup: %s", p);
		goto out;
	}

	/* skip ',' */
	q++;

	cgrp_recv = cgroup__new(q, /*do_open=*/true);
	if (cgrp_recv == NULL) {
		fprintf(stderr, "cannot open receiver cgroup: %s", q);
		goto out;
	}
	ret = 0;

out:
	free(p);
	return ret;
}

static const struct option options[] = {
	OPT_INTEGER('l', "loop",	&loops,		"Specify number of loops"),
	OPT_BOOLEAN('T', "threaded",	&threaded,	"Specify threads/process based task setup"),
	OPT_CALLBACK('G', "cgroups", NULL, "SEND,RECV",
		     "Put sender and receivers in given cgroups",
		     parse_two_cgroups),
	OPT_END()
};

static const char * const bench_sched_pipe_usage[] = {
	"perf bench sched pipe <options>",
	NULL
};

static int enter_cgroup(struct cgroup *cgrp)
{
	char buf[32];
	int fd, len, ret;
	pid_t pid;

	if (cgrp == NULL)
		return 0;

	if (threaded)
		pid = syscall(__NR_gettid);
	else
		pid = getpid();

	snprintf(buf, sizeof(buf), "%d\n", pid);
	len = strlen(buf);

	/* try cgroup v2 interface first */
	if (threaded)
		fd = openat(cgrp->fd, "cgroup.threads", O_WRONLY);
	else
		fd = openat(cgrp->fd, "cgroup.procs", O_WRONLY);

	/* try cgroup v1 if failed */
	if (fd < 0)
		fd = openat(cgrp->fd, "tasks", O_WRONLY);

	if (fd < 0) {
		char mnt[PATH_MAX];

		printf("Failed to open cgroup file in %s\n", cgrp->name);

		if (cgroupfs_find_mountpoint(mnt, sizeof(mnt), "perf_event") == 0)
			printf(" Hint: create the cgroup first, like 'mkdir %s/%s'\n",
			       mnt, cgrp->name);
		return -1;
	}

	ret = write(fd, buf, len);
	close(fd);

	if (ret != len) {
		printf("Cannot enter to cgroup: %s\n", cgrp->name);
		return -1;
	}
	return 0;
}

static void *worker_thread(void *__tdata)
{
	struct thread_data *td = __tdata;
	int m = 0, i;
	int ret;

	if (td->nr)
		ret = enter_cgroup(cgrp_send);
	else
		ret = enter_cgroup(cgrp_recv);

	if (ret < 0) {
		td->cgroup_failed = true;
		return NULL;
	}
	td->cgroup_failed = false;

	for (i = 0; i < loops; i++) {
		if (!td->nr) {
			ret = read(td->pipe_read, &m, sizeof(int));
			BUG_ON(ret != sizeof(int));
			ret = write(td->pipe_write, &m, sizeof(int));
			BUG_ON(ret != sizeof(int));
		} else {
			ret = write(td->pipe_write, &m, sizeof(int));
			BUG_ON(ret != sizeof(int));
			ret = read(td->pipe_read, &m, sizeof(int));
			BUG_ON(ret != sizeof(int));
		}
	}

	return NULL;
}

int bench_sched_pipe(int argc, const char **argv)
{
	struct thread_data threads[2], *td;
	int pipe_1[2], pipe_2[2];
	struct timeval start, stop, diff;
	unsigned long long result_usec = 0;
	int nr_threads = 2;
	int t;

	/*
	 * why does "ret" exist?
	 * discarding returned value of read(), write()
	 * causes error in building environment for perf
	 */
	int __maybe_unused ret, wait_stat;
	pid_t pid, retpid __maybe_unused;

	argc = parse_options(argc, argv, options, bench_sched_pipe_usage, 0);

	BUG_ON(pipe(pipe_1));
	BUG_ON(pipe(pipe_2));

	gettimeofday(&start, NULL);

	for (t = 0; t < nr_threads; t++) {
		td = threads + t;

		td->nr = t;

		if (t == 0) {
			td->pipe_read = pipe_1[0];
			td->pipe_write = pipe_2[1];
		} else {
			td->pipe_write = pipe_1[1];
			td->pipe_read = pipe_2[0];
		}
	}

	if (threaded) {
		for (t = 0; t < nr_threads; t++) {
			td = threads + t;

			ret = pthread_create(&td->pthread, NULL, worker_thread, td);
			BUG_ON(ret);
		}

		for (t = 0; t < nr_threads; t++) {
			td = threads + t;

			ret = pthread_join(td->pthread, NULL);
			BUG_ON(ret);
		}
	} else {
		pid = fork();
		assert(pid >= 0);

		if (!pid) {
			worker_thread(threads + 0);
			exit(0);
		} else {
			worker_thread(threads + 1);
		}

		retpid = waitpid(pid, &wait_stat, 0);
		assert((retpid == pid) && WIFEXITED(wait_stat));
	}

	gettimeofday(&stop, NULL);
	timersub(&stop, &start, &diff);

	cgroup__put(cgrp_send);
	cgroup__put(cgrp_recv);

	if (threads[0].cgroup_failed || threads[1].cgroup_failed)
		return 0;

	switch (bench_format) {
	case BENCH_FORMAT_DEFAULT:
		printf("# Executed %d pipe operations between two %s\n\n",
			loops, threaded ? "threads" : "processes");

		result_usec = diff.tv_sec * USEC_PER_SEC;
		result_usec += diff.tv_usec;

		printf(" %14s: %lu.%03lu [sec]\n\n", "Total time",
		       (unsigned long) diff.tv_sec,
		       (unsigned long) (diff.tv_usec / USEC_PER_MSEC));

		printf(" %14lf usecs/op\n",
		       (double)result_usec / (double)loops);
		printf(" %14d ops/sec\n",
		       (int)((double)loops /
			     ((double)result_usec / (double)USEC_PER_SEC)));
		break;

	case BENCH_FORMAT_SIMPLE:
		printf("%lu.%03lu\n",
		       (unsigned long) diff.tv_sec,
		       (unsigned long) (diff.tv_usec / USEC_PER_MSEC));
		break;

	default:
		/* reaching here is something disaster */
		fprintf(stderr, "Unknown format:%d\n", bench_format);
		exit(1);
		break;
	}

	return 0;
}
