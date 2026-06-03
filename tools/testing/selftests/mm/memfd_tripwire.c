// SPDX-License-Identifier: GPL-2.0
/*
 * memfd_tripwire selftest
 *
 * Tests for the memfd_tripwire() syscall which creates a shared memory region
 * with write notification support via poll() and ioctl-based re-arming.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <linux/memfd_tripwire.h>

#include "kselftest.h"

#ifdef __NR_memfd_tripwire

static int sys_memfd_tripwire(unsigned int flags)
{
	return syscall(__NR_memfd_tripwire, flags);
}

/*
 * Poll the fd for POLLIN with the given timeout in milliseconds.
 * Returns 1 if POLLIN is set, 0 on timeout, -1 on error.
 */
static int poll_check(int fd, int timeout_ms)
{
	struct pollfd pfd = { .fd = fd, .events = POLLIN };
	int ret;

	ret = poll(&pfd, 1, timeout_ms);
	if (ret < 0)
		return -1;
	if (ret == 0)
		return 0;
	return (pfd.revents & POLLIN) ? 1 : 0;
}

static int tripwire_ack(int fd)
{
	return ioctl(fd, MEMFD_TRIPWIRE_ACK, 0);
}

static long page_size;

static int setup_tripwire(size_t size, int *fd, char **mem)
{
	*fd = sys_memfd_tripwire(0);
	if (*fd < 0) {
		ksft_test_result_fail("memfd_tripwire: %m\n");
		return -1;
	}

	if (ftruncate(*fd, size) < 0) {
		ksft_test_result_fail("ftruncate: %m\n");
		goto out;
	}

	*mem = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, *fd, 0);
	if (*mem == MAP_FAILED) {
		*mem = NULL;
		ksft_test_result_fail("mmap: %m\n");
		goto out;
	}

	return 0;

out:
	close(*fd);
	return -1;
}

static double time_elapsed(struct timespec *start)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	return (now.tv_sec - start->tv_sec) +
	       (now.tv_nsec - start->tv_nsec) / 1e9;
}

/*
 * Test 1: Sequential single-process semantics.
 *
 * Verifies the complete life cycle:
 *   create -> poll (clean) -> write -> poll (dirty) -> ACK -> poll (clean)
 *          -> write again -> poll (dirty)
 */
static void test_sequential(void)
{
	int fd;
	char *mem;

	if (setup_tripwire(page_size, &fd, &mem) != 0)
		return;

	/* Freshly created: should not have POLLIN */
	if (poll_check(fd, 0) != 0) {
		ksft_test_result_fail("POLLIN set on clean fd\n");
		goto out;
	}

	/* Write to mapped memory */
	mem[0] = 'A';

	/* Should now report POLLIN */
	if (poll_check(fd, 100) != 1) {
		ksft_test_result_fail("POLLIN not set after write\n");
		goto out;
	}

	/* Polling again without ACK should still show POLLIN */
	if (poll_check(fd, 0) != 1) {
		ksft_test_result_fail("POLLIN lost without ACK\n");
		goto out;
	}

	/* ACK re-arms the tripwire */
	if (tripwire_ack(fd) < 0) {
		ksft_test_result_fail("ioctl ACK: %m\n");
		goto out;
	}

	/* After ACK: should be clean again */
	if (poll_check(fd, 0) != 0) {
		ksft_test_result_fail("POLLIN set after ACK\n");
		goto out;
	}

	/* Write again: must re-trigger */
	mem[0] = 'B';

	if (poll_check(fd, 100) != 1) {
		ksft_test_result_fail("POLLIN not set after second write\n");
		goto out;
	}

	ksft_test_result_pass("sequential\n");
out:
	munmap((void *)mem, page_size);
	close(fd);
}

/*
 * Test 2: Multi-page, writes to different pages trigger after re-arm.
 */
static void test_multi_page(void)
{
	int fd;
	char *mem;
	size_t size = page_size * 4;

	if (setup_tripwire(size, &fd, &mem) != 0)
		return;

	/* Write page 0, observe, ACK */
	mem[0] = 'A';
	if (poll_check(fd, 100) != 1) {
		ksft_test_result_fail("POLLIN not set after page 0 write\n");
		goto out;
	}
	tripwire_ack(fd);

	/* Write page 2: A different page must also trigger */
	mem[page_size * 2] = 'B';
	if (poll_check(fd, 100) != 1) {
		ksft_test_result_fail("POLLIN not set after page 2 write\n");
		goto out;
	}
	tripwire_ack(fd);

	/* Write page 3 */
	mem[page_size * 3] = 'C';
	if (poll_check(fd, 100) != 1) {
		ksft_test_result_fail("POLLIN not set after page 3 write\n");
		goto out;
	}

	ksft_test_result_pass("multi_page\n");
out:
	munmap((void *)mem, size);
	close(fd);
}

/*
 * Test 3: Cross-process producer/consumer.
 *
 * Parent is the consumer (polls for events), child is the producer (writes
 * to the shared mapping). Verifies that writes in the child process are
 * visible to the parent via poll().
 */
static void test_cross_process(void)
{
	int fd, status;
	char *mem;
	pid_t pid;

	if (setup_tripwire(page_size, &fd, &mem) != 0)
		return;

	pid = fork();
	if (pid < 0) {
		ksft_test_result_fail("fork: %m\n");
		goto out;
	}

	if (pid == 0) {
		/*
		 * Child: wait a bit then write. The parent should be blocked
		 * in poll() and wake up when we write.
		 */
		usleep(50000);
		mem[0] = 'P';
		_exit(0);
	}

	/* Parent: wait for the child's write */
	if (poll_check(fd, 2000) != 1) {
		ksft_test_result_fail("POLLIN not set after child write\n");
		kill(pid, SIGKILL);
		waitpid(pid, &status, 0);
		goto out;
	}

	waitpid(pid, &status, 0);

	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		ksft_test_result_fail("child exited abnormally\n");
		goto out;
	}

	/* Verify data written by child */
	if (mem[0] != 'P') {
		ksft_test_result_fail("data mismatch: got 0x%02x\n", mem[0]);
		goto out;
	}

	ksft_test_result_pass("cross_process\n");
out:
	munmap((void *)mem, page_size);
	close(fd);
}

/*
 * Test 4: Multi-round cross-process producer/consumer.
 *
 * Uses a pipe to synchronize rounds between parent (consumer) and child
 * (producer). Each round: child writes, parent detects via poll, parent
 * ACKs, repeat. Verifies that the full cycle works reliably across
 * process boundaries.
 */
static void test_cross_process_multi_round(void)
{
	int fd, status;
	int pipe_to_child[2], pipe_to_parent[2];
	char *mem;
	pid_t pid;
	int rounds = 1000;
	char sync;

	if (setup_tripwire(page_size, &fd, &mem) != 0)
		return;

	if (pipe(pipe_to_child) < 0 || pipe(pipe_to_parent) < 0) {
		ksft_test_result_fail("pipe: %m\n");
		goto out;
	}

	pid = fork();
	if (pid < 0) {
		ksft_test_result_fail("fork: %m\n");
		goto out;
	}

	if (pid == 0) {
		/* Child: producer */
		close(pipe_to_child[1]);
		close(pipe_to_parent[0]);

		for (int i = 0; i < rounds; i++) {
			/* Wait for parent to signal us to write */
			if (read(pipe_to_child[0], &sync, 1) != 1)
				_exit(1);
			mem[0] = 'A' + i;
			/* Tell parent we wrote */
			if (write(pipe_to_parent[1], &sync, 1) != 1)
				_exit(1);
		}
		_exit(0);
	}

	/* Parent: consumer */
	close(pipe_to_child[0]);
	close(pipe_to_parent[1]);

	for (int i = 0; i < rounds; i++) {
		/* Tell child to write */
		sync = 'G';
		if (write(pipe_to_child[1], &sync, 1) != 1) {
			ksft_test_result_fail("write to pipe: %m\n");
			goto reap;
		}

		/* Wait for child to confirm write */
		if (read(pipe_to_parent[0], &sync, 1) != 1) {
			ksft_test_result_fail("read from pipe: %m\n");
			goto reap;
		}

		/* Detect the write */
		if (poll_check(fd, 1000) != 1) {
			ksft_test_result_fail("round %d: POLLIN not set\n", i);
			goto reap;
		}

		/* Verify data */
		if (mem[0] != (char)('A' + i)) {
			ksft_test_result_fail("round %d: data 0x%02x\n", i,
					      mem[0]);
			goto reap;
		}

		/* Re-arm for next round */
		if (tripwire_ack(fd) < 0) {
			ksft_test_result_fail("round %d: ACK: %m\n", i);
			goto reap;
		}

		/* Confirm clean after ACK */
		if (poll_check(fd, 0) != 0) {
			ksft_test_result_fail("round %d: POLLIN after ACK\n",
					      i);
			goto reap;
		}
	}

	close(pipe_to_child[1]);
	close(pipe_to_parent[0]);

	waitpid(pid, &status, 0);
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		ksft_test_result_fail("child exited abnormally\n");
		goto out;
	}

	ksft_test_result_pass("cross_process_multi_round\n");
	munmap((void *)mem, page_size);
	close(fd);
	return;

reap:
	close(pipe_to_child[1]);
	close(pipe_to_parent[0]);
	kill(pid, SIGKILL);
	waitpid(pid, &status, 0);
out:
	munmap((void *)mem, page_size);
	close(fd);
}

/*
 * Test 5: Producer/consumer race stress test.
 *
 * A producer thread writes continuously while the consumer thread polls and
 * ACKs. The invariant is: we must never "miss" an event. Specifically, after
 * the consumer ACKs and the producer writes at least once more, the consumer
 * must eventually see POLLIN again.
 *
 * We use an atomic generation counter to track this. The producer increments
 * the generation and writes to the mapping. The consumer records the
 * generation at ACK time and verifies that when it next sees POLLIN, the
 * generation has advanced.
 */
static atomic_int stress_gen;
static atomic_bool stress_stop;

static void *stress_producer(void *arg)
{
	char *mem = arg;

	while (!atomic_load(&stress_stop)) {
		atomic_fetch_add(&stress_gen, 1);
		mem[0]++;
		/* Intentionally no explicit delay to maximize contention */
	}

	return NULL;
}

static void test_stress_no_lost_events(void)
{
	int fd;
	char *mem;
	pthread_t producer;
	int ack_count = 0;
	int gen_at_ack, cur_gen;
	struct timespec start;

	if (setup_tripwire(page_size, &fd, &mem) != 0)
		return;

	atomic_store(&stress_gen, 0);
	atomic_store(&stress_stop, false);

	if (pthread_create(&producer, NULL, stress_producer, (void *)mem)) {
		ksft_test_result_fail("pthread_create: %m\n");
		goto out;
	}

	/*
	 * Consumer loop: run for ~2 seconds. In each iteration:
	 *   1. Wait for POLLIN (the producer is writing continuously)
	 *   2. Record the current generation
	 *   3. ACK (re-arm)
	 *   4. The producer is still writing, so POLLIN must come back
	 */
	clock_gettime(CLOCK_MONOTONIC, &start);
	while (time_elapsed(&start) <= 2.0) {
		/*
		 * Wait for a write notification. The producer is looping, so
		 * the timeout should be plenty.
		 */
		if (poll_check(fd, 1000) != 1) {
			ksft_test_result_fail(
				"POLLIN not set (ack_count=%d, gen=%d)\n",
				ack_count, atomic_load(&stress_gen));
			atomic_store(&stress_stop, true);
			pthread_join(producer, NULL);
			goto out;
		}

		/* Record generation and ACK */
		gen_at_ack = atomic_load(&stress_gen);
		if (tripwire_ack(fd) < 0) {
			ksft_test_result_fail("ACK failed: %m\n");
			atomic_store(&stress_stop, true);
			pthread_join(producer, NULL);
			goto out;
		}
		ack_count++;

		/*
		 * The producer is running concurrently. After ACK, it will
		 * write again and increment the generation. We don't need to
		 * check anything here because the next poll() iteration will
		 * verify that the write was detected.
		 *
		 * Spin briefly to let the producer get ahead, exercising the
		 * race between ACK (write-protect) and producer (write).
		 */
		cur_gen = atomic_load(&stress_gen);
		while (cur_gen == gen_at_ack) {
			sched_yield();
			cur_gen = atomic_load(&stress_gen);
		}
	}

	atomic_store(&stress_stop, true);
	pthread_join(producer, NULL);

	if (ack_count < 10) {
		ksft_test_result_fail("too few ACK cycles: %d\n", ack_count);
		goto out;
	}

	ksft_print_msg("stress: %d ACK cycles, final gen=%d\n", ack_count,
		       atomic_load(&stress_gen));
	ksft_test_result_pass("stress_no_lost_events\n");
out:
	munmap((void *)mem, page_size);
	close(fd);
}

/*
 * Test 6: Randomized multi-thread visibility stress test.
 *
 * N_VIS_WRITERS writer threads and N_VIS_READERS reader threads operate
 * concurrently on a shared memfd_tripwire mapping for VIS_DURATION_S seconds.
 * Each writer owns a uint32_t slot on its own page and writes monotonically
 * increasing values, interspersed with random sleeps. Each reader randomly
 * polls or sleeps; on POLLIN it snapshots committed values, ACKs, then reads
 * the mapped memory and validates visibility.
 *
 * The invariant under test: after a reader calls ACK, all writes that were
 * committed (writer_committed[i] updated with store-release) before the
 * reader's snapshot (loaded with load-acquire) must be visible when reading
 * the mapped memory. The ACK's TLB-flush IPI provides the cross-CPU memory
 * barrier that makes this guarantee hold.
 */

#define N_VIS_WRITERS 4
#define N_VIS_READERS 2
#define VIS_DURATION_S 2
#define VIS_WRITER_SLEEP_PCT 20
#define VIS_READER_SLEEP_PCT 30
#define VIS_MAX_WRITER_SLEEP_US 1000
#define VIS_MAX_READER_SLEEP_US 3000

struct vis_ctx {
	int fd;
	char *mem;
	atomic_uint writer_committed[N_VIS_WRITERS];
	atomic_bool stop;
	atomic_bool failed;
	char failure_msg[256];
};

struct vis_thread_arg {
	struct vis_ctx *ctx;
	int id;
};

static void *vis_writer_fn(void *arg)
{
	struct vis_thread_arg *ta = arg;
	struct vis_ctx *ctx = ta->ctx;
	int id = ta->id;
	uint32_t *slot = (uint32_t *)(ctx->mem + id * page_size);
	uint32_t value = 0;
	unsigned int seed = (unsigned int)(id * 7919 + 1);

	while (!atomic_load_explicit(&ctx->stop, memory_order_relaxed)) {
		if (rand_r(&seed) % 100 < VIS_WRITER_SLEEP_PCT) {
			usleep(rand_r(&seed) % VIS_MAX_WRITER_SLEEP_US);
			continue;
		}

		value++;
		*slot = value;
		atomic_store_explicit(&ctx->writer_committed[id], value,
				      memory_order_release);
	}

	return NULL;
}

static void *vis_reader_fn(void *arg)
{
	struct vis_thread_arg *ta = arg;
	struct vis_ctx *ctx = ta->ctx;
	uint32_t snapshot[N_VIS_WRITERS];
	unsigned int seed = (unsigned int)(ta->id * 6971 + 42);
	int i;

	while (!atomic_load_explicit(&ctx->stop, memory_order_relaxed)) {
		if (atomic_load_explicit(&ctx->failed, memory_order_relaxed))
			break;

		if (rand_r(&seed) % 100 < VIS_READER_SLEEP_PCT) {
			usleep(rand_r(&seed) % VIS_MAX_READER_SLEEP_US);
			continue;
		}

		if (poll_check(ctx->fd, 100) != 1)
			continue;

		for (i = 0; i < N_VIS_WRITERS; i++)
			snapshot[i] =
				atomic_load_explicit(&ctx->writer_committed[i],
						     memory_order_acquire);

		tripwire_ack(ctx->fd);

		for (i = 0; i < N_VIS_WRITERS; i++) {
			uint32_t *slot = (uint32_t *)(ctx->mem + i * page_size);
			uint32_t observed = *slot;

			if (observed < snapshot[i]) {
				snprintf(
					ctx->failure_msg,
					sizeof(ctx->failure_msg),
					"writer %d: observed %u < committed %u",
					i, observed, snapshot[i]);
				atomic_store(&ctx->failed, true);
				atomic_store(&ctx->stop, true);
				return NULL;
			}
		}
	}

	return NULL;
}

static void test_stress_visibility(void)
{
	struct vis_ctx ctx;
	struct vis_thread_arg wargs[N_VIS_WRITERS];
	struct vis_thread_arg rargs[N_VIS_READERS];
	pthread_t writers[N_VIS_WRITERS];
	pthread_t readers[N_VIS_READERS];
	size_t map_size = N_VIS_WRITERS * page_size;
	struct timespec start;
	int i, nw = 0, nr = 0;

	if (setup_tripwire(map_size, &ctx.fd, &ctx.mem) != 0)
		return;

	atomic_store(&ctx.stop, false);
	atomic_store(&ctx.failed, false);
	ctx.failure_msg[0] = '\0';
	for (i = 0; i < N_VIS_WRITERS; i++)
		atomic_store(&ctx.writer_committed[i], 0);

	for (i = 0; i < N_VIS_WRITERS; i++) {
		wargs[i].ctx = &ctx;
		wargs[i].id = i;
		if (pthread_create(&writers[i], NULL, vis_writer_fn, &wargs[i]))
			goto stop;
		nw++;
	}

	for (i = 0; i < N_VIS_READERS; i++) {
		rargs[i].ctx = &ctx;
		rargs[i].id = i;
		if (pthread_create(&readers[i], NULL, vis_reader_fn, &rargs[i]))
			goto stop;
		nr++;
	}

	clock_gettime(CLOCK_MONOTONIC, &start);
	do {
		usleep(100000);
	} while (time_elapsed(&start) <= VIS_DURATION_S &&
		 !atomic_load(&ctx.failed));

stop:
	atomic_store(&ctx.stop, true);
	for (i = 0; i < nr; i++)
		pthread_join(readers[i], NULL);
	for (i = 0; i < nw; i++)
		pthread_join(writers[i], NULL);

	if (atomic_load(&ctx.failed)) {
		ksft_test_result_fail("visibility: %s\n", ctx.failure_msg);
	} else {
		for (i = 0; i < N_VIS_WRITERS; i++) {
			ksft_print_msg("visibility: writer %d committed %u\n",
				       i,
				       atomic_load(&ctx.writer_committed[i]));
		}
		ksft_test_result_pass("stress_visibility\n");
	}

	munmap((void *)ctx.mem, map_size);
	close(ctx.fd);
}

#define NUM_TESTS 6

int main(void)
{
	page_size = sysconf(_SC_PAGE_SIZE);
	if (!page_size)
		ksft_exit_fail_msg("Failed to get page size %m\n");

	ksft_print_header();
	ksft_set_plan(NUM_TESTS);

	test_sequential();
	test_multi_page();
	test_cross_process();
	test_cross_process_multi_round();
	test_stress_no_lost_events();
	test_stress_visibility();

	ksft_finished();
	return 0;
}

#else /* __NR_memfd_tripwire */

int main(int argc, char *argv[])
{
	printf("skip: skipping memfd_tripwire test (missing __NR_memfd_tripwire)\n");
	return KSFT_SKIP;
}

#endif /* __NR_memfd_tripwire */
