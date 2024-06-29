// SPDX-License-Identifier: GPL-2.0-only

#define _GNU_SOURCE
#include <stdlib.h>
#include <sys/types.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>

/*
 * Create multiple infinite loop threads based on the passed parameters
 * Usage: deadloop num policy prio
 *	num: the number of child threads
 *	policy: the scheduling policy of the child threads, 0-fair, 1-fifo, 2-rr
 *	prio: the priority
 * If this process is killed, it will print the loop count of all child threads
 * to the OUTPUT_FILE
 *
 * Date: June 27, 2024
 * Author: Xavier <xavier_qy@163.com>
 */

#define OUTPUT_FILE "rt_group_sched_test.log"

#if __GLIBC_PREREQ(2, 30) == 0
#include <sys/syscall.h>
static pid_t gettid(void)
{
	return syscall(SYS_gettid);
}
#endif

#define do_err(x) \
do { \
	if ((x) < 0) {  \
		printf("test BUG_ON func %s, line %d %ld\n", \
			__func__, __LINE__, (long)(x) \
		); \
		while (1) \
			sleep(1); \
	} \
} while (0)

#define do_false(x) \
do { \
	if ((x) == 1) { \
		printf("test BUG_ON func %s, line %d %d\n", \
			__func__, __LINE__, (x) \
		); \
		while (1) \
			sleep(1); \
	} \
} while (0)


struct thread_data {
	pthread_t thread;
	int index;
	int pid;
	unsigned long cnt;
};

static struct thread_data *pdata;
static int thread_num = 1;

static void create_thread_posix(void *entry, pthread_t *thread, int *para,
								 int policy, int prio)
{
	int					ret;
	struct sched_param	param;
	pthread_attr_t		attr;

	memset(&param, 0, sizeof(param));
	ret = pthread_attr_init(&attr);
	do_err(ret);

	ret = pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
	do_err(ret);

	param.sched_priority = prio;

	ret = pthread_attr_setschedpolicy(&attr, policy);
	do_err(ret);

	ret = pthread_attr_setschedparam(&attr, &param);
	do_err(ret);

	ret = pthread_create(thread, &attr, entry, para);
	do_err(ret);
}

static void *dead_loop_entry(void *arg)
{
	int index = *(int *)arg;
	struct sched_param param;
	int cur = gettid();

	sched_getparam(cur, &param);
	pdata[index].pid = cur;
	printf("cur:%d prio:%d\n", cur, param.sched_priority);

	while (1) {
		asm volatile("" ::: "memory");
		pdata[index].cnt++;
	}
	return NULL;
}

static void handle_signal(int signal)
{
	int cnt = 0;

	if (signal == SIGTERM) {
		FILE *file = freopen(OUTPUT_FILE, "a", stdout);

		if (file == NULL) {
			perror("freopen");
			exit(0);
		}

		while (cnt < thread_num) {
			printf("pid:%d cnt:%ld\n", pdata[cnt].pid, pdata[cnt].cnt);
			cnt++;
		}
		fclose(file);
		exit(0);
	}
}

static int dead_loop_create(int policy, int prio)
{
	int cnt = 0;
	int ret;
	void *status;
	struct sched_param param;

	param.sched_priority = prio;
	pdata = malloc(thread_num * sizeof(struct thread_data));
	do_false(!pdata);

	if (policy) {
		ret = sched_setscheduler(0, policy, &param);
		do_err(ret);
	}

	while (cnt < thread_num) {
		pdata[cnt].index = cnt;
		create_thread_posix(dead_loop_entry, &pdata[cnt].thread,
								 &pdata[cnt].index, policy, prio);
		cnt++;
	}

	signal(SIGTERM, handle_signal);

	cnt = 0;
	while (cnt < thread_num) {
		pthread_join(pdata[cnt].thread, &status);
		cnt++;
	}

	free(pdata);
	return 0;
}

int main(int argc, char **argv)
{
	int policy = 2;
	int prio = 50;

	if (argc == 2)
		thread_num = atoi(argv[1]);

	if (argc == 3) {
		thread_num = atoi(argv[1]);
		policy = atoi(argv[2]);
		if (policy > 0)
			prio = 50;
	}

	if (argc == 4) {
		thread_num = atoi(argv[1]);
		policy = atoi(argv[2]);
		prio = atoi(argv[3]);
	}

	dead_loop_create(policy, prio);

	return 0;
}
