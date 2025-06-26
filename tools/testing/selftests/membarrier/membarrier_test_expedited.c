// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE
#include <linux/membarrier.h>
#include <syscall.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <stdatomic.h>

#include "membarrier_test_impl.h"

struct thread_state {
	atomic_int thread_cpu;
	atomic_bool end_thread;
	pthread_mutex_t mutex;
};

void *test_membarrier_thread(void *arg)
{
	struct thread_state *ts = (struct thread_state *)arg;

	ts->thread_cpu = sched_getcpu();
	pthread_mutex_unlock(&ts->mutex);
	if (ts->thread_cpu < 0)
		return 0;
	while (!ts->end_thread)
		ts->thread_cpu = sched_getcpu();
	return NULL;
}

static long read_interrupts(int cpu)
{
	char line[4096];
	FILE *fp = fopen("/proc/interrupts", "r");
	long res = 0;

	if (!fp)
		ksft_exit_fail_msg("unable to open /proc/interrupts\n");

	fgets(line, sizeof(line), fp); /* skip first line */
	while (fgets(line, sizeof(line), fp) != NULL) {
		char *save;
		int next_cpu = 0;

		for (char *token = strtok_r(line, " ", &save); token;
		     token = strtok_r(NULL, " ", &save)) {
			if (*token < '0' || *token > '9')
				continue;
			if (next_cpu++ == cpu)
				res += atol(token);
		}
	}
	fclose(fp);
	return res;
}

static int test_membarrier(const char *name, int cmd, int register_cmd)
{
	int runs = 0;
	long irq = 0;
	pthread_t test_thread;
	int ret = 0;

	struct thread_state ts = { .thread_cpu = -1,
				   .end_thread = 0,
				   .mutex = PTHREAD_MUTEX_INITIALIZER };
	if (sys_membarrier_cpu(cmd, 0) == 0)
		ksft_exit_fail_msg("%s: expected failure before register\n",
				   name);
	if (sys_membarrier(register_cmd, 0) != 0)
		ksft_exit_fail_msg("%s: unable to register\n", name);

	/* nothing interesting in single processor machines */
	if (sysconf(_SC_NPROCESSORS_ONLN) == 1)
		goto success;

	pthread_mutex_lock(&ts.mutex);
	pthread_create(&test_thread, NULL, test_membarrier_thread, &ts);

	/* wait for thread to start */
	pthread_mutex_lock(&ts.mutex);
	pthread_mutex_unlock(&ts.mutex);

	for (int i = 0; i < 1000; i++) {
		int cpu_start, cpu_end, cpu_this;
		long irq_start, irq_end;

		cpu_start = ts.thread_cpu;
		if (cpu_start < 0)
			ksft_exit_fail_msg("sched_getcpu() failed\n");

		irq_start = read_interrupts(cpu_start);
		if (sys_membarrier_cpu(cmd, cpu_start))
			ksft_exit_fail_msg("%s: sys_membarrier failed\n", name);
		cpu_end = ts.thread_cpu;
		cpu_this = sched_getcpu();

		/* maybe it was moved to a different cpu, so we cannot trust the irq count */
		/* If we are on the same cpu we wouldnt expect an interrupt */
		if (cpu_end != cpu_start || cpu_this == cpu_end)
			continue;
		irq_end = read_interrupts(cpu_end);
		irq += (irq_end - irq_start);
		runs++;
	}
	ts.end_thread = 1;
	pthread_join(test_thread, NULL);

	if (!runs)
		ksft_exit_fail_msg("%s: no successful runs\n", name);

	/* Every run should probably have had an interrupt, but use at least half
	 * to be safe.
	 */
	if (irq < runs / 2)
		ksft_exit_fail_msg("%s: only had %d / %d irqs\n", name, irq,
				   runs);
success:
	ksft_test_result_pass("expedited %s\n", name);
	return 0;
}

int main(int argc, char **argv)
{
	ksft_print_header();
	ksft_set_plan(3);

	test_membarrier("EXPEDITED", MEMBARRIER_CMD_PRIVATE_EXPEDITED,
			MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED);
	test_membarrier("RSEQ", MEMBARRIER_CMD_PRIVATE_EXPEDITED_RSEQ,
			MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED_RSEQ);
	test_membarrier("SYNC_CORE", MEMBARRIER_CMD_PRIVATE_EXPEDITED_SYNC_CORE,
			MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED_SYNC_CORE);
	ksft_exit_pass();
}
