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
#include <sys/epoll.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/syscall.h>
#include <linux/time64.h>

#include <pthread.h>

struct thread_data {
	unsigned int		nr;		/* index of this worker */
	pid_t			pid;
	int			pipe_read;
	int			pipe_write;
	struct epoll_event      epoll_ev;
	int			epoll_fd;
	bool			cgroup_failed;
	pthread_t		pthread;
};

#define LOOPS_DEFAULT 1000000
static	unsigned int		loops = LOOPS_DEFAULT;

/* Use processes by default: */
static bool			threaded;
static unsigned int		nr_threads = 2;

static bool			nonblocking;
static bool			Kn_mode;	/* Toggle for ring mode -> complete graph mode */

static char			*cgrp_names[2];
static struct cgroup		*cgrps[2];

static int parse_two_cgroups(const struct option *opt __maybe_unused,
			     const char *str, int unset __maybe_unused)
{
	char *p = strdup(str);
	char *q;
	int ret = -1;

	if (p == NULL) {
		fprintf(stderr, "memory allocation failure\n");
		return -1;
	}

	q = strchr(p, ',');
	if (q == NULL) {
		fprintf(stderr, "it should have two cgroup names: %s\n", p);
		goto out;
	}
	*q = '\0';

	cgrp_names[0] = strdup(p);
	cgrp_names[1] = strdup(q + 1);

	if (cgrp_names[0] == NULL || cgrp_names[1] == NULL) {
		fprintf(stderr, "memory allocation failure\n");
		goto out;
	}
	ret = 0;

out:
	free(p);
	return ret;
}

static const struct option options[] = {
	OPT_BOOLEAN('n', "nonblocking",	&nonblocking,	"Use non-blocking operations"),
	OPT_UINTEGER('p', "nprocs",	&nr_threads,    "Number of processes"),
	OPT_UINTEGER('l', "loop",	&loops,		"Specify number of loops"),
	OPT_BOOLEAN('K', "Kn",		&Kn_mode,	"Send tokens in a complete graph instead of a ring."),
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

static int enter_cgroup(int nr)
{
	char buf[32];
	int fd, len, ret;
	int saved_errno;
	struct cgroup *cgrp;
	pid_t pid;

	if (cgrp_names[nr % 2] == NULL)
		return 0;

	if (cgrps[nr % 2] == NULL) {
		cgrps[nr % 2] = cgroup__new(cgrp_names[nr % 2], /*do_open=*/true);
		if (cgrps[nr % 2] == NULL)
			goto err;
	}
	cgrp = cgrps[nr % 2];

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
	if (fd < 0 && errno == ENOENT)
		fd = openat(cgrp->fd, "tasks", O_WRONLY);

	if (fd < 0)
		goto err;

	ret = write(fd, buf, len);
	close(fd);

	if (ret != len) {
		printf("Cannot enter to cgroup: %s\n", cgrp->name);
		return -1;
	}
	return 0;

err:
	saved_errno = errno;
	printf("Failed to open cgroup file in %s\n", cgrp_names[nr % 2]);

	if (saved_errno == ENOENT) {
		char mnt[PATH_MAX];

		if (cgroupfs_find_mountpoint(mnt, sizeof(mnt), "perf_event") == 0)
			printf(" Hint: create the cgroup first, like 'mkdir %s/%s'\n",
			       mnt, cgrp_names[nr % 2]);
	} else if (saved_errno == EACCES && geteuid() > 0) {
		printf(" Hint: try to run as root\n");
	}

	return -1;
}

static void exit_cgroup(int nr)
{
	cgroup__put(cgrps[nr % 2]);
	free(cgrp_names[nr % 2]);
}

static inline int read_pipe(struct thread_data *td)
{
	int ret, m;
retry:
	if (nonblocking) {
		ret = epoll_wait(td->epoll_fd, &td->epoll_ev, 1, -1);
		if (ret < 0)
			return ret;
	}
	ret = read(td->pipe_read, &m, sizeof(int));
	if (nonblocking && ret < 0 && errno == EWOULDBLOCK)
		goto retry;
	return ret;
}

/*
 * Worker thread for processes forming a complete graph,
 * sending tokens one to each other.
 */
static void *worker_thread_kn(void *__tdata)
{
	struct thread_data *this_thread = __tdata;
	struct thread_data *all_threads = this_thread - this_thread->nr;

	int ret, m = 0;
	unsigned int i;
	unsigned int t;

	ret = enter_cgroup(this_thread->nr);
	if (ret < 0) {
		this_thread->cgroup_failed = true;
		return NULL;
	}

	if (nonblocking) {
		this_thread->epoll_ev.events = EPOLLIN;
		this_thread->epoll_fd = epoll_create(1);
		BUG_ON(this_thread->epoll_fd < 0);
		BUG_ON(epoll_ctl(this_thread->epoll_fd, EPOLL_CTL_ADD, this_thread->pipe_read, &this_thread->epoll_ev) < 0);
	}

	for (i = 0; i < loops; i++) {
		/* First: feed all other workers. */
		for (t = 0; t < nr_threads; t++)
			if (t != this_thread->nr) {
				ret = write(all_threads[t].pipe_write, &m, sizeof(int));
				BUG_ON(ret != sizeof(int));
			}

		/* Read a token from all other workers. */
		for (t = 1; t < nr_threads; t++) {
			ret = read_pipe(this_thread);
			BUG_ON(ret != sizeof(int));
		}
	}

	return NULL;
}

/*
 * Worker thread for nodes forming a ring, receiving tokens from the left
 * neighbor and sending them to the right one.
 */
static void *worker_thread_ring(void *__tdata)
{
	struct thread_data *this_thread = __tdata;
	struct thread_data *first_thread = this_thread - this_thread->nr;

	unsigned int i;
	int ret, m = 0;
	int write_fd;

	ret = enter_cgroup(this_thread->nr);
	if (ret < 0) {
		this_thread->cgroup_failed = true;
		return NULL;
	}

	if (nonblocking) {
		this_thread->epoll_ev.events = EPOLLIN;
		this_thread->epoll_fd = epoll_create(1);
		BUG_ON(this_thread->epoll_fd < 0);
		BUG_ON(epoll_ctl(this_thread->epoll_fd, EPOLL_CTL_ADD, this_thread->pipe_read, &this_thread->epoll_ev) < 0);
	}

	/* Find write_fd of right peer in the ring. */
	if ((this_thread->nr + 1) == nr_threads)
		write_fd = first_thread->pipe_write;
	else
		write_fd = (this_thread + 1)->pipe_write;


	for (i = 0; i < loops; i++) {
		ret = write(write_fd, &m, sizeof(int));
		BUG_ON(ret != sizeof(int));
		ret = read_pipe(this_thread);
		BUG_ON(ret != sizeof(int));
	}

	return NULL;
}

/* Ring mode is the default. */
void * (*worker_thread)(void *) = worker_thread_ring;

static struct thread_data *create_thread_data(void)
{
	struct thread_data *threads;
	int __maybe_unused flags = 0;
	int pipe_fds[2];
	unsigned int i;

	if (nonblocking)
		flags |= O_NONBLOCK;

	threads = malloc(nr_threads * sizeof(struct thread_data));

	if (!threads) {
		fprintf(stderr, "Allocation of thread data memory failed.");
		exit(1);
	}

	for (i = 0; i < nr_threads; i++) {
		threads[i].nr = i;

		BUG_ON(pipe2(pipe_fds, flags));

		threads[i].pipe_read = pipe_fds[0];
		threads[i].pipe_write = pipe_fds[1];
	}

	return threads;
}

int bench_sched_pipe(int argc, const char **argv)
{
	struct thread_data *threads;
	struct thread_data *td;

	struct timeval start, stop, diff;
	unsigned long long result_usec = 0;
	unsigned int t;

	/*
	 * why does "ret" exist?
	 * discarding returned value of read(), write()
	 * causes error in building environment for perf
	 */
	int __maybe_unused ret, wait_stat;
	pid_t retpid __maybe_unused;

	argc = parse_options(argc, argv, options, bench_sched_pipe_usage, 0);

	if (Kn_mode)
		worker_thread = worker_thread_kn;

	threads = create_thread_data();

	gettimeofday(&start, NULL);

	if (threaded) {
		for (t = 0; t < nr_threads; t++) {
			td = threads + t;

			ret = pthread_create(&td->pthread, NULL, worker_thread, threads + t);
			BUG_ON(ret);
		}

		for (t = 0; t < nr_threads; t++) {
			td = threads + t;

			ret = pthread_join(td->pthread, NULL);
			BUG_ON(ret);
		}
	} else {
		/*
		 * Start at '1', because the parent eventually also becomes a
		 * worker.
		 */
		for (t = 1; t < nr_threads; t++) {
			threads[t].pid = fork();
			assert(threads[t].pid >= 0);

			if (!threads[t].pid) {
				worker_thread(threads + t);
				exit(0);
			}
		}

		worker_thread(threads);

		for (t = 1; t < nr_threads; t++) {
			retpid = waitpid(threads[t].pid, &wait_stat, 0);
			assert((retpid == threads[t].pid) && WIFEXITED(wait_stat));
		}
	}

	gettimeofday(&stop, NULL);
	timersub(&stop, &start, &diff);

	exit_cgroup(0);
	exit_cgroup(1);

	if (threads[0].cgroup_failed || threads[1].cgroup_failed)
		return 0;

	switch (bench_format) {
	case BENCH_FORMAT_DEFAULT:
		printf("# Executed %d pipe operations (%s) between %u %s\n\n", loops,
		       Kn_mode ? "Kn" : "ring", nr_threads,
		       threaded ? "threads" : "processes");

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
