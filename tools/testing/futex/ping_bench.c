// SPDX-License-Identifier: GPL-2.0-only

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <string.h>
#include <stdbool.h>
#include <err.h>
#include <unistd.h>
#include <stdint.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <time.h>
#include <stdatomic.h>
#include <stdarg.h>
#include <pthread.h>
#include <limits.h>
#include <linux/futex.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/time.h>

#define	MAX_THR 512

#define	FUTEX_LOCK_PING		14
#define	FUTEX_UNLOCK_PING	15

#define	READ_ONCE(x) (*(volatile typeof(x) *)&(x))

struct thread {
	pthread_t pthr;
	uint64_t *dur;
	int id;
};

static struct thread bthr[MAX_THR];
static struct thread thr[MAX_THR];
static pthread_barrier_t bar;

uint32_t _lock;
pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;
void *lock = &_lock;

static long counter;
static int num_thr;
static int num_rt;
static int busy_thr;
static uint64_t num_iter = 10000;
static  __thread pid_t tid;
static int work_times = 100000;
static uint64_t sleep_dur = 1000;
static bool do_sleep;
static bool print_all;
static bool quiet;
static bool bias;

enum futex_type {
	FUTEX,
	FUTEX_PI,
	FUTEX_PING,
	FUTEX_PING_USTEAL,
	MUTEX,
};
static enum futex_type futex_type = FUTEX_PING;

extern char *optarg;
extern int optind;

int trace_marker_fd;

static void
init_trace_marker(void)
{
	trace_marker_fd = open("/sys/kernel/tracing/trace_marker", O_WRONLY);
	if (trace_marker_fd < 0)
		perror("Failed to open trace_marker");
}

static int
write_trace_marker(const char *format, ...)
{
	char buffer[256];
	va_list args;
	int len;

	if (trace_marker_fd <= 0)
		return -1;

	va_start(args, format);
	len = vsnprintf(buffer, sizeof(buffer), format, args);
	va_end(args);

	if (len > 0)
		write(trace_marker_fd, buffer, len);

	return (0);
}

static inline uint64_t
now_ns(void)
{
	struct timespec ts;

	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
		err(1, "clock_gettime");

	return (ts.tv_sec * 1000000000UL + ts.tv_nsec);
}

static int
futex(uint32_t *uaddr, int op, uint32_t val, struct timespec *to)
{
	return (syscall(SYS_futex, uaddr, op, val, to));
}

static void
futex_lock(void *lock)
{
	pthread_mutex_t *mu;
	uint32_t *fu, old;
	int ret;

	fu = lock;
	mu = lock;
	switch (futex_type) {
	case FUTEX:
		while (1) {
			old = 0;
			if (atomic_compare_exchange_strong(fu, &old, 1))
				return;
			if (futex(fu, FUTEX_WAIT, 1, NULL) != 0 && errno !=
			    EAGAIN)
				err(1, "FUTEX_WAIT");
		}
		break;
	case FUTEX_PI:
		old = 0;
		if (atomic_compare_exchange_strong(fu, &old, tid))
			return;
		if ((ret = futex(fu, FUTEX_LOCK_PI, 0, NULL)) != 0)
			errx(1, "FUTEX_LOCK_PI %s", strerror(ret));
		break;
	case FUTEX_PING:
	case FUTEX_PING_USTEAL:
		old = 0;
		if (atomic_compare_exchange_strong(fu, &old, tid))
			return;
		old = FUTEX_WAITERS;
		if (futex_type == FUTEX_PING_USTEAL &&
		    atomic_compare_exchange_strong(fu, &old, tid |
		    FUTEX_WAITERS))
			return;
		if ((ret = futex(fu, FUTEX_LOCK_PING, 0, NULL)) != 0)
			err(1, "FUTEX_LOCK_PING");
		break;
	case MUTEX:
		if (pthread_mutex_lock(mu) < 0)
			err(1, "pthread_mutex_lock");
		break;
	}
}

static void
futex_unlock(void *lock)
{
	pthread_mutex_t *mu;
	uint32_t *fu, old;

	fu = lock;
	mu = lock;
	switch (futex_type) {
	case FUTEX:
		old = 1;
		if (atomic_compare_exchange_strong(fu, &old, 0))
			if (futex(fu, FUTEX_WAKE, 1, NULL) < 0)
				err(1, "FUTEX_WAKE");
		break;
	case FUTEX_PI:
		old = tid;
		if (atomic_compare_exchange_strong(fu, &old, 0))
			return;
		if (futex(fu, FUTEX_UNLOCK_PI, 0, NULL) != 0)
			err(1, "FUTEX_UNLOCK_PI t %x old %x", tid,
			    READ_ONCE(*fu));
		break;
	case FUTEX_PING:
	case FUTEX_PING_USTEAL:
		old = tid;
		if (atomic_compare_exchange_strong(fu, &old, 0))
			return;
		if (futex(fu, FUTEX_UNLOCK_PING, 0, NULL) != 0)
			err(1, "FUTEX_UNLOCK_PING t %x old %x", tid,
			    READ_ONCE(*fu));
		break;
	case MUTEX:
		if (pthread_mutex_unlock(mu) < 0)
			err(1, "pthread_mutex_unlock");
		break;
	}
}

atomic_int stop_spinners = 0;

static void *
func(void *p)
{
	struct thread *thr;
	uint64_t end, start;
	uint64_t i, j, _num_iter;
	uint64_t my_sleep_dur = sleep_dur;
	uint64_t my_work_times = work_times;
	struct timespec ts;

	tid = gettid();

	thr = p;
	thr->dur = malloc(num_iter * sizeof(uint64_t));
	_num_iter = num_iter;

	if (thr->id == 0) {
		prctl(PR_SET_NAME, "foreground", 0, 0, 0);
		if (bias) {
			nice(-5);
			my_sleep_dur *= 10;
			if (my_work_times)
				my_work_times /= 10;
		}
	} else {
		prctl(PR_SET_NAME, "background", 0, 0, 0);
		if (bias) {
			if (my_sleep_dur)
				my_sleep_dur /= 10;
			my_work_times *= 10;
			nice(19);
		}
	}

	ts.tv_sec  = my_sleep_dur / 1000000;
	ts.tv_nsec = (my_sleep_dur % 1000000) * 1000;


	if (thr->id < num_rt) {
		struct sched_param param = { .sched_priority = 10 };

		if (sched_setscheduler(0, SCHED_FIFO, &param) != 0)
			err(1, "sched_setscheduler");
	}

	pthread_barrier_wait(&bar);

	for (i = 0; i < _num_iter; i++) {
		if (thr->id == 0)
			write_trace_marker("B|%lu|Locking", (unsigned long)tid);

		start = now_ns();
		futex_lock(lock);
		end = now_ns();

		if (thr->id == 0)
			write_trace_marker("E|%lu|Locking", (unsigned long)tid);
		thr->dur[i] = end - start;

		for (j = 0; j < my_work_times; j++)
			__asm __volatile("" ::: "memory");

		counter++;
		futex_unlock(lock);

		if (atomic_load(&stop_spinners))
			break;

		if (sleep_dur) {
			if (do_sleep)
				clock_nanosleep(CLOCK_MONOTONIC, 0, &ts, 0);
			else
				for (j = 0; j < my_sleep_dur; j++)
					__asm __volatile("" ::: "memory");
		}
	}

	if (thr->id == 0)
		atomic_store(&stop_spinners, 1);
	return (NULL);
}

static void *
busy(void *p)
{
	prctl(PR_SET_NAME, "spinner", 0, 0, 0);
	if (0 &&  num_rt) {
		struct sched_param param = { .sched_priority = 1 };

		if (sched_setscheduler(0, SCHED_FIFO, &param) != 0)
			err(1, "sched_setscheduler");
	}

	pthread_barrier_wait(&bar);

	while (!atomic_load(&stop_spinners))
		__asm __volatile("" ::: "memory");

	return (NULL);
}


static void
usage(char *a)
{
	fprintf(stderr, "Usage: %s [-a] [-b n] [-c] [-f f/m/n/N/p] [-n n] [-q]"
	    " [-r n] [-s n] [-S n] [-t n] [-w n]\n", a);
	exit(1);
}

int
main(int argc, char **argv)
{
	int c, i, j, ret;

	while ((c = getopt(argc, argv, "ab:f:n:pqr:S:s:t:w:")) != -1) {
		switch (c) {
		case 'a':
			print_all = 1;
			break;
		case 'f':
			switch (*optarg) {
			case 'f':
				futex_type = FUTEX;
				break;
			case 'm':
				futex_type = MUTEX;
				lock = &mtx;
				break;
			case 'n':
				futex_type = FUTEX_PING;
				break;
			case 'N':
				futex_type = FUTEX_PING_USTEAL;
				break;
			case 'p':
				futex_type = FUTEX_PI;
				break;
			default:
				usage(argv[0]);
			}
			break;
		case 'n':
			num_iter = atoi(optarg);
			break;
		case 'q':
			quiet = 1;
			break;
		case 'p':
			bias = 1;
			break;
		case 'r':
			num_rt = atoi(optarg);
			break;
		case 'S':
			do_sleep = 1;
			/* Fallthrough */
		case 's':
			sleep_dur = atoi(optarg);
			break;
		case 't':
			num_thr = atoi(optarg);
			break;
		case 'b':
			busy_thr = atoi(optarg);
			break;
		case 'w':
			work_times = atoi(optarg);
			break;
		default:
			usage(argv[0]);
		}
	}

	init_trace_marker();

	if (num_thr < num_rt)
		num_thr = num_rt;

	num_thr -= num_rt;
	if (num_thr > MAX_THR)
		num_thr = MAX_THR;

	tid = gettid();

	ret = pthread_barrier_init(&bar, NULL, num_thr + num_rt + busy_thr);
	if (ret != 0)
		errx(1, "pthread_barrier_init %s", strerror(ret));

	for (i = 0; i < num_thr + num_rt; i++) {
		thr[i].id = i;
		if ((ret = pthread_create(&thr[i].pthr, NULL, func, &thr[i]))
		    != 0)
			errx(1, "pthread_create %d: %s", i, strerror(ret));
	}
	
	for (i = 0; i < busy_thr; i++) {
		if ((ret = pthread_create(&bthr[i].pthr, NULL, busy, &bthr[i]))
		    != 0)
			errx(1, "pthread_create %d: %s", i, strerror(ret));
	}

	for (i = 0; i < busy_thr; i++)
		if ((ret = pthread_join(bthr[i].pthr, NULL)) != 0)
			errx(1, "pthread_join: %s\n", strerror(ret));

	for (i = 0; i < num_thr + num_rt; i++)
		if ((ret = pthread_join(thr[i].pthr, NULL)) != 0)
			errx(1, "pthread_join: %s\n", strerror(ret));

	if (!quiet) {
		/* skip the first run */
		if (print_all)
			for (i = 0; i < num_thr; i++)
				for (j = 1; j < num_iter; j++)
					printf("%lu\n", thr[0].dur[j]);
		else
			for (j = 1; j < num_iter; j++)
				printf("%lu\n", thr[0].dur[j]);
	}

	return (0);
}
