// SPDX-License-Identifier: GPL-2.0
/*
 * tlob_helper.c - test helper and ELF utility for tlob selftests
 *
 * Called by test_tlob.sh to exercise the /dev/rv ioctl interface and to
 * resolve ELF symbol offsets for uprobe bindings.  One subcommand per
 * invocation so the shell script can report each as an independent TAP
 * test case.
 *
 * Usage: tlob_helper <subcommand> [args...]
 *
 * Synchronous TRACE_START / TRACE_STOP tests:
 *   not_enabled        - TRACE_START without tlob enabled -> ENODEV (no kernel crash)
 *   within_budget      - start(50000 us), sleep 10 ms, stop -> expect 0
 *   over_budget_cpu    - start(5000 us), busyspin 100 ms, stop -> EOVERFLOW
 *   over_budget_sleep  - start(3000 us), sleep 50 ms, stop -> EOVERFLOW
 *
 * Error-handling tests:
 *   double_start       - two starts without stop -> EEXIST on second
 *   stop_no_start      - stop without start -> ESRCH
 *
 * Per-thread isolation test:
 *   multi_thread       - two threads share one fd; one within budget, one over
 *
 * Asynchronous notification test (notify_fd + read()):
 *   self_watch         - one worker exceeds budget; monitor fd receives one ntf via read()
 *
 * Input-validation tests (TRACE_START error paths):
 *   invalid_flags      - TRACE_START with flags != 0 -> EINVAL
 *   notify_fd_bad      - TRACE_START with notify_fd = stdout (non-rv fd) -> EINVAL
 *
 * mmap ring buffer tests (Scenario D):
 *   mmap_basic         - mmap succeeds; verify tlob_mmap_page fields
 *                        (version, capacity, data_offset, record_size)
 *   mmap_errors        - MAP_PRIVATE, wrong size, and non-zero pgoff all
 *                        return EINVAL
 *   mmap_consume       - trigger a real violation via self-notification and
 *                        consume the event through the mmap'd ring
 *
 * ELF utility (does not require /dev/rv):
 *   sym_offset <binary> <symbol>
 *                      - print the ELF file offset of <symbol> in <binary>
 *                        (used by the shell script to build uprobe bindings)
 *
 * Exit code: 0 = pass, 1 = fail, 2 = skip (device not available).
 */
#define _GNU_SOURCE
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <linux/rv.h>

/* Default ring capacity allocated at open(); matches TLOB_RING_DEFAULT_CAP. */
#define TLOB_RING_DEFAULT_CAP	64U

static int rv_fd = -1;

static int open_rv(void)
{
	rv_fd = open("/dev/rv", O_RDWR);
	if (rv_fd < 0) {
		fprintf(stderr, "open /dev/rv: %s\n", strerror(errno));
		return -1;
	}
	return 0;
}

static void busy_spin_us(unsigned long us)
{
	struct timespec start, now;
	unsigned long elapsed;

	clock_gettime(CLOCK_MONOTONIC, &start);
	do {
		clock_gettime(CLOCK_MONOTONIC, &now);
		elapsed = (unsigned long)(now.tv_sec - start.tv_sec)
			  * 1000000000UL
			+ (unsigned long)(now.tv_nsec - start.tv_nsec);
	} while (elapsed < us * 1000UL);
}

static int do_start(uint64_t threshold_us)
{
	struct tlob_start_args args = {
		.threshold_us = threshold_us,
		.notify_fd    = -1,
	};

	return ioctl(rv_fd, TLOB_IOCTL_TRACE_START, &args);
}

static int do_stop(void)
{
	return ioctl(rv_fd, TLOB_IOCTL_TRACE_STOP, NULL);
}

/* -----------------------------------------------------------------------
 * Synchronous TRACE_START / TRACE_STOP tests
 * -----------------------------------------------------------------------
 */

/*
 * test_not_enabled - TRACE_START must return ENODEV when the tlob monitor
 * has not been enabled (tlob_state_cache is NULL).
 *
 * The shell wrapper deliberately does NOT call tlob_enable before invoking
 * this subcommand, so the ioctl is expected to fail with ENODEV rather than
 * crashing the kernel with a NULL pointer dereference in kmem_cache_alloc.
 */
static int test_not_enabled(void)
{
	int ret;

	ret = do_start(1000);
	if (ret == 0) {
		fprintf(stderr, "TRACE_START: expected ENODEV, got success\n");
		do_stop();
		return 1;
	}
	if (errno != ENODEV) {
		fprintf(stderr, "TRACE_START: expected ENODEV, got %s\n",
			strerror(errno));
		return 1;
	}
	return 0;
}

static int test_within_budget(void)
{
	int ret;

	if (do_start(50000) < 0) {
		fprintf(stderr, "TRACE_START: %s\n", strerror(errno));
		return 1;
	}
	usleep(10000); /* 10 ms < 50 ms budget */
	ret = do_stop();
	if (ret != 0) {
		fprintf(stderr, "TRACE_STOP: expected 0, got %d errno=%s\n",
			ret, strerror(errno));
		return 1;
	}
	return 0;
}

static int test_over_budget_cpu(void)
{
	int ret;

	if (do_start(5000) < 0) {
		fprintf(stderr, "TRACE_START: %s\n", strerror(errno));
		return 1;
	}
	busy_spin_us(100000); /* 100 ms >> 5 ms budget */
	ret = do_stop();
	if (ret == 0) {
		fprintf(stderr, "TRACE_STOP: expected EOVERFLOW, got 0\n");
		return 1;
	}
	if (errno != EOVERFLOW) {
		fprintf(stderr, "TRACE_STOP: expected EOVERFLOW, got %s\n",
			strerror(errno));
		return 1;
	}
	return 0;
}

static int test_over_budget_sleep(void)
{
	int ret;

	if (do_start(3000) < 0) {
		fprintf(stderr, "TRACE_START: %s\n", strerror(errno));
		return 1;
	}
	usleep(50000); /* 50 ms >> 3 ms budget, off-CPU time counts */
	ret = do_stop();
	if (ret == 0) {
		fprintf(stderr, "TRACE_STOP: expected EOVERFLOW, got 0\n");
		return 1;
	}
	if (errno != EOVERFLOW) {
		fprintf(stderr, "TRACE_STOP: expected EOVERFLOW, got %s\n",
			strerror(errno));
		return 1;
	}
	return 0;
}

/* -----------------------------------------------------------------------
 * Error-handling tests
 * -----------------------------------------------------------------------
 */

static int test_double_start(void)
{
	int ret;

	if (do_start(10000000) < 0) {
		fprintf(stderr, "first TRACE_START: %s\n", strerror(errno));
		return 1;
	}
	ret = do_start(10000000);
	if (ret == 0) {
		fprintf(stderr, "second TRACE_START: expected EEXIST, got 0\n");
		do_stop();
		return 1;
	}
	if (errno != EEXIST) {
		fprintf(stderr, "second TRACE_START: expected EEXIST, got %s\n",
			strerror(errno));
		do_stop();
		return 1;
	}
	do_stop(); /* clean up */
	return 0;
}

static int test_stop_no_start(void)
{
	int ret;

	/* Ensure clean state: ignore error from a stale entry */
	do_stop();

	ret = do_stop();
	if (ret == 0) {
		fprintf(stderr, "TRACE_STOP: expected ESRCH, got 0\n");
		return 1;
	}
	if (errno != ESRCH) {
		fprintf(stderr, "TRACE_STOP: expected ESRCH, got %s\n",
			strerror(errno));
		return 1;
	}
	return 0;
}

/* -----------------------------------------------------------------------
 * Per-thread isolation test
 *
 * Two threads share a single /dev/rv fd.  The monitor uses task_struct *
 * as the key, so each thread gets an independent slot regardless of the
 * shared fd.
 * -----------------------------------------------------------------------
 */

struct mt_thread_args {
	uint64_t      threshold_us;
	unsigned long workload_us;
	int           busy;
	int           expect_eoverflow;
	int           result;
};

static void *mt_thread_fn(void *arg)
{
	struct mt_thread_args *a = arg;
	int ret;

	if (do_start(a->threshold_us) < 0) {
		fprintf(stderr, "thread TRACE_START: %s\n", strerror(errno));
		a->result = 1;
		return NULL;
	}

	if (a->busy)
		busy_spin_us(a->workload_us);
	else
		usleep(a->workload_us);

	ret = do_stop();
	if (a->expect_eoverflow) {
		if (ret == 0 || errno != EOVERFLOW) {
			fprintf(stderr, "thread: expected EOVERFLOW, got ret=%d errno=%s\n",
				ret, strerror(errno));
			a->result = 1;
			return NULL;
		}
	} else {
		if (ret != 0) {
			fprintf(stderr, "thread: expected 0, got ret=%d errno=%s\n",
				ret, strerror(errno));
			a->result = 1;
			return NULL;
		}
	}
	a->result = 0;
	return NULL;
}

static int test_multi_thread(void)
{
	pthread_t ta, tb;
	struct mt_thread_args a = {
		.threshold_us     = 20000,  /* 20 ms */
		.workload_us      = 5000,   /* 5 ms sleep -> within budget */
		.busy             = 0,
		.expect_eoverflow = 0,
	};
	struct mt_thread_args b = {
		.threshold_us     = 3000,   /* 3 ms */
		.workload_us      = 30000,  /* 30 ms spin -> over budget */
		.busy             = 1,
		.expect_eoverflow = 1,
	};

	pthread_create(&ta, NULL, mt_thread_fn, &a);
	pthread_create(&tb, NULL, mt_thread_fn, &b);
	pthread_join(ta, NULL);
	pthread_join(tb, NULL);

	return (a.result || b.result) ? 1 : 0;
}

/* -----------------------------------------------------------------------
 * Asynchronous notification test (notify_fd + read())
 *
 * A dedicated monitor_fd is opened by the main thread.  Two worker threads
 * each open their own work_fd and call TLOB_IOCTL_TRACE_START with
 * notify_fd = monitor_fd, nominating it as the violation target.  Worker A
 * stays within budget; worker B exceeds it.  The main thread reads from
 * monitor_fd and expects exactly one tlob_event record.
 * -----------------------------------------------------------------------
 */

struct sw_worker_args {
	int           monitor_fd;
	uint64_t      threshold_us;
	unsigned long workload_us;
	int           busy;
	int           result;
};

static void *sw_worker_fn(void *arg)
{
	struct sw_worker_args *a = arg;
	struct tlob_start_args args = {
		.threshold_us = a->threshold_us,
		.notify_fd    = a->monitor_fd,
	};
	int work_fd;
	int ret;

	work_fd = open("/dev/rv", O_RDWR);
	if (work_fd < 0) {
		fprintf(stderr, "worker open /dev/rv: %s\n", strerror(errno));
		a->result = 1;
		return NULL;
	}

	ret = ioctl(work_fd, TLOB_IOCTL_TRACE_START, &args);
	if (ret < 0) {
		fprintf(stderr, "TRACE_START (notify): %s\n", strerror(errno));
		close(work_fd);
		a->result = 1;
		return NULL;
	}

	if (a->busy)
		busy_spin_us(a->workload_us);
	else
		usleep(a->workload_us);

	ioctl(work_fd, TLOB_IOCTL_TRACE_STOP, NULL);
	close(work_fd);
	a->result = 0;
	return NULL;
}

static int test_self_watch(void)
{
	int monitor_fd;
	pthread_t ta, tb;
	struct sw_worker_args a = {
		.threshold_us = 50000,  /* 50 ms */
		.workload_us  = 5000,   /* 5 ms sleep -> no violation */
		.busy         = 0,
	};
	struct sw_worker_args b = {
		.threshold_us = 3000,   /* 3 ms */
		.workload_us  = 30000,  /* 30 ms spin -> violation */
		.busy         = 1,
	};
	struct tlob_event ntfs[8];
	int violations = 0;
	ssize_t n;

	/*
	 * Open monitor_fd with O_NONBLOCK so read() after the workers finish
	 * returns immediately rather than blocking forever.
	 */
	monitor_fd = open("/dev/rv", O_RDWR | O_NONBLOCK);
	if (monitor_fd < 0) {
		fprintf(stderr, "open /dev/rv (monitor_fd): %s\n", strerror(errno));
		return 1;
	}
	a.monitor_fd = monitor_fd;
	b.monitor_fd = monitor_fd;

	pthread_create(&ta, NULL, sw_worker_fn, &a);
	pthread_create(&tb, NULL, sw_worker_fn, &b);
	pthread_join(ta, NULL);
	pthread_join(tb, NULL);

	if (a.result || b.result) {
		close(monitor_fd);
		return 1;
	}

	/*
	 * Drain all available tlob_event records.  With O_NONBLOCK the final
	 * read() returns -EAGAIN when the buffer is empty.
	 */
	while ((n = read(monitor_fd, ntfs, sizeof(ntfs))) > 0)
		violations += (int)(n / sizeof(struct tlob_event));

	close(monitor_fd);

	if (violations != 1) {
		fprintf(stderr, "self_watch: expected 1 violation, got %d\n",
			violations);
		return 1;
	}
	return 0;
}

/* -----------------------------------------------------------------------
 * Input-validation tests (TRACE_START error paths)
 * -----------------------------------------------------------------------
 */

/*
 * test_invalid_flags - TRACE_START with flags != 0 must return EINVAL.
 *
 * The flags field is reserved for future extensions and must be zero.
 * Callers that set it to a non-zero value are rejected early so that a
 * future kernel can assign meaning to those bits without silently
 * ignoring them.
 */
static int test_invalid_flags(void)
{
	struct tlob_start_args args = {
		.threshold_us = 1000,
		.notify_fd    = -1,
		.flags        = 1,   /* non-zero: must be rejected */
	};
	int ret;

	ret = ioctl(rv_fd, TLOB_IOCTL_TRACE_START, &args);
	if (ret == 0) {
		fprintf(stderr, "TRACE_START(flags=1): expected EINVAL, got success\n");
		do_stop();
		return 1;
	}
	if (errno != EINVAL) {
		fprintf(stderr, "TRACE_START(flags=1): expected EINVAL, got %s\n",
			strerror(errno));
		return 1;
	}
	return 0;
}

/*
 * test_notify_fd_bad - TRACE_START with a non-/dev/rv notify_fd must return
 * EINVAL.
 *
 * When notify_fd >= 0, the kernel resolves it to a struct file and checks
 * that its private_data is non-NULL (i.e. it is a /dev/rv file descriptor).
 * Passing stdout (fd 1) supplies a real, open fd whose private_data is NULL,
 * so the kernel must reject it with EINVAL.
 */
static int test_notify_fd_bad(void)
{
	struct tlob_start_args args = {
		.threshold_us = 1000,
		.notify_fd    = STDOUT_FILENO,   /* open but not a /dev/rv fd */
		.flags        = 0,
	};
	int ret;

	ret = ioctl(rv_fd, TLOB_IOCTL_TRACE_START, &args);
	if (ret == 0) {
		fprintf(stderr,
			"TRACE_START(notify_fd=stdout): expected EINVAL, got success\n");
		do_stop();
		return 1;
	}
	if (errno != EINVAL) {
		fprintf(stderr,
			"TRACE_START(notify_fd=stdout): expected EINVAL, got %s\n",
			strerror(errno));
		return 1;
	}
	return 0;
}

/* -----------------------------------------------------------------------
 * mmap ring buffer tests (Scenario D)
 * -----------------------------------------------------------------------
 */

/*
 * test_mmap_basic - mmap the ring buffer and verify the control page fields.
 *
 * The kernel allocates TLOB_RING_DEFAULT_CAP records at open().  A shared
 * mmap of PAGE_SIZE + cap * record_size must succeed and the tlob_mmap_page
 * header must contain consistent values.
 */
static int test_mmap_basic(void)
{
	long pagesize = sysconf(_SC_PAGESIZE);
	size_t mmap_len = (size_t)pagesize +
			  TLOB_RING_DEFAULT_CAP * sizeof(struct tlob_event);
	/* rv_mmap requires a page-aligned length */
	mmap_len = (mmap_len + (size_t)(pagesize - 1)) & ~(size_t)(pagesize - 1);
	struct tlob_mmap_page *page;
	struct tlob_event *data;
	void *map;
	int ret = 0;

	map = mmap(NULL, mmap_len, PROT_READ | PROT_WRITE, MAP_SHARED, rv_fd, 0);
	if (map == MAP_FAILED) {
		fprintf(stderr, "mmap_basic: mmap: %s\n", strerror(errno));
		return 1;
	}

	page = (struct tlob_mmap_page *)map;
	data = (struct tlob_event *)((char *)map + page->data_offset);

	if (page->version != 1) {
		fprintf(stderr, "mmap_basic: expected version=1, got %u\n",
			page->version);
		ret = 1;
		goto out;
	}
	if (page->capacity != TLOB_RING_DEFAULT_CAP) {
		fprintf(stderr, "mmap_basic: expected capacity=%u, got %u\n",
			TLOB_RING_DEFAULT_CAP, page->capacity);
		ret = 1;
		goto out;
	}
	if (page->data_offset != (uint32_t)pagesize) {
		fprintf(stderr, "mmap_basic: expected data_offset=%ld, got %u\n",
			pagesize, page->data_offset);
		ret = 1;
		goto out;
	}
	if (page->record_size != sizeof(struct tlob_event)) {
		fprintf(stderr, "mmap_basic: expected record_size=%zu, got %u\n",
			sizeof(struct tlob_event), page->record_size);
		ret = 1;
		goto out;
	}
	if (page->data_head != 0 || page->data_tail != 0) {
		fprintf(stderr, "mmap_basic: ring not empty at open: head=%u tail=%u\n",
			page->data_head, page->data_tail);
		ret = 1;
		goto out;
	}
	/* Touch the data array to confirm it is accessible. */
	(void)data[0].tid;
out:
	munmap(map, mmap_len);
	return ret;
}

/*
 * test_mmap_errors - verify that rv_mmap() rejects invalid mmap parameters.
 *
 * Four cases are tested, each must return MAP_FAILED with errno == EINVAL:
 *   1. size one page short of the correct ring length
 *   2. size one page larger than the correct ring length
 *   3. MAP_PRIVATE (only MAP_SHARED is permitted)
 *   4. non-zero vm_pgoff (offset must be 0)
 */
static int test_mmap_errors(void)
{
	long pagesize = sysconf(_SC_PAGESIZE);
	size_t correct_len = (size_t)pagesize +
			     TLOB_RING_DEFAULT_CAP * sizeof(struct tlob_event);
	/* rv_mmap requires a page-aligned length */
	correct_len = (correct_len + (size_t)(pagesize - 1)) & ~(size_t)(pagesize - 1);
	void *map;
	int ret = 0;

	/* Case 1: size one page short (correct_len - 1 still rounds up to correct_len) */
	map = mmap(NULL, correct_len - (size_t)pagesize, PROT_READ | PROT_WRITE,
		   MAP_SHARED, rv_fd, 0);
	if (map != MAP_FAILED) {
		fprintf(stderr, "mmap_errors: short-size mmap succeeded (expected EINVAL)\n");
		munmap(map, correct_len - (size_t)pagesize);
		ret = 1;
	} else if (errno != EINVAL) {
		fprintf(stderr, "mmap_errors: short-size: expected EINVAL, got %s\n",
			strerror(errno));
		ret = 1;
	}

	/* Case 2: size one page too large */
	map = mmap(NULL, correct_len + (size_t)pagesize, PROT_READ | PROT_WRITE,
		   MAP_SHARED, rv_fd, 0);
	if (map != MAP_FAILED) {
		fprintf(stderr, "mmap_errors: oversized mmap succeeded (expected EINVAL)\n");
		munmap(map, correct_len + (size_t)pagesize);
		ret = 1;
	} else if (errno != EINVAL) {
		fprintf(stderr, "mmap_errors: oversized: expected EINVAL, got %s\n",
			strerror(errno));
		ret = 1;
	}

	/* Case 3: MAP_PRIVATE instead of MAP_SHARED */
	map = mmap(NULL, correct_len, PROT_READ | PROT_WRITE,
		   MAP_PRIVATE, rv_fd, 0);
	if (map != MAP_FAILED) {
		fprintf(stderr, "mmap_errors: MAP_PRIVATE succeeded (expected EINVAL)\n");
		munmap(map, correct_len);
		ret = 1;
	} else if (errno != EINVAL) {
		fprintf(stderr, "mmap_errors: MAP_PRIVATE: expected EINVAL, got %s\n",
			strerror(errno));
		ret = 1;
	}

	/* Case 4: non-zero file offset (pgoff = 1) */
	map = mmap(NULL, correct_len, PROT_READ | PROT_WRITE,
		   MAP_SHARED, rv_fd, (off_t)pagesize);
	if (map != MAP_FAILED) {
		fprintf(stderr, "mmap_errors: non-zero pgoff mmap succeeded (expected EINVAL)\n");
		munmap(map, correct_len);
		ret = 1;
	} else if (errno != EINVAL) {
		fprintf(stderr, "mmap_errors: non-zero pgoff: expected EINVAL, got %s\n",
			strerror(errno));
		ret = 1;
	}

	return ret;
}

/*
 * test_mmap_consume - zero-copy consumption of a real violation event.
 *
 * Arms a 5 ms budget with self-notification (notify_fd = rv_fd), sleeps
 * 50 ms (off-CPU violation), then reads the pushed event through the mmap'd
 * ring without calling read().  Verifies:
 *   - TRACE_STOP returns EOVERFLOW (budget was exceeded)
 *   - data_head == 1 after the violation
 *   - the event fields (threshold_us, tag, tid) are correct
 *   - data_tail can be advanced to consume the record (ring empties)
 */
static int test_mmap_consume(void)
{
	long pagesize = sysconf(_SC_PAGESIZE);
	size_t mmap_len = (size_t)pagesize +
			  TLOB_RING_DEFAULT_CAP * sizeof(struct tlob_event);
	/* rv_mmap requires a page-aligned length */
	mmap_len = (mmap_len + (size_t)(pagesize - 1)) & ~(size_t)(pagesize - 1);
	struct tlob_start_args args = {
		.threshold_us = 5000,		/* 5 ms */
		.notify_fd    = rv_fd,		/* self-notification */
		.tag          = 0xdeadbeefULL,
		.flags        = 0,
	};
	struct tlob_mmap_page *page;
	struct tlob_event *data;
	void *map;
	int stop_ret;
	int ret = 0;

	map = mmap(NULL, mmap_len, PROT_READ | PROT_WRITE, MAP_SHARED, rv_fd, 0);
	if (map == MAP_FAILED) {
		fprintf(stderr, "mmap_consume: mmap: %s\n", strerror(errno));
		return 1;
	}

	page = (struct tlob_mmap_page *)map;
	data = (struct tlob_event *)((char *)map + page->data_offset);

	if (ioctl(rv_fd, TLOB_IOCTL_TRACE_START, &args) < 0) {
		fprintf(stderr, "mmap_consume: TRACE_START: %s\n", strerror(errno));
		ret = 1;
		goto out;
	}

	usleep(50000); /* 50 ms >> 5 ms budget -> off-CPU violation */

	stop_ret = ioctl(rv_fd, TLOB_IOCTL_TRACE_STOP, NULL);
	if (stop_ret == 0) {
		fprintf(stderr, "mmap_consume: TRACE_STOP returned 0, expected EOVERFLOW\n");
		ret = 1;
		goto out;
	}
	if (errno != EOVERFLOW) {
		fprintf(stderr, "mmap_consume: TRACE_STOP: expected EOVERFLOW, got %s\n",
			strerror(errno));
		ret = 1;
		goto out;
	}

	/* Pairs with smp_store_release in tlob_event_push. */
	if (__atomic_load_n(&page->data_head, __ATOMIC_ACQUIRE) != 1) {
		fprintf(stderr, "mmap_consume: expected data_head=1, got %u\n",
			page->data_head);
		ret = 1;
		goto out;
	}
	if (page->data_tail != 0) {
		fprintf(stderr, "mmap_consume: expected data_tail=0, got %u\n",
			page->data_tail);
		ret = 1;
		goto out;
	}

	/* Verify record content */
	if (data[0].threshold_us != 5000) {
		fprintf(stderr, "mmap_consume: expected threshold_us=5000, got %llu\n",
			(unsigned long long)data[0].threshold_us);
		ret = 1;
		goto out;
	}
	if (data[0].tag != 0xdeadbeefULL) {
		fprintf(stderr, "mmap_consume: expected tag=0xdeadbeef, got %llx\n",
			(unsigned long long)data[0].tag);
		ret = 1;
		goto out;
	}
	if (data[0].tid == 0) {
		fprintf(stderr, "mmap_consume: tid is 0\n");
		ret = 1;
		goto out;
	}

	/* Consume: advance data_tail and confirm ring is empty */
	__atomic_store_n(&page->data_tail, 1U, __ATOMIC_RELEASE);
	if (__atomic_load_n(&page->data_head, __ATOMIC_ACQUIRE) !=
	    __atomic_load_n(&page->data_tail, __ATOMIC_ACQUIRE)) {
		fprintf(stderr, "mmap_consume: ring not empty after consume\n");
		ret = 1;
	}

out:
	munmap(map, mmap_len);
	return ret;
}

/* -----------------------------------------------------------------------
 * ELF utility: sym_offset
 *
 * Print the ELF file offset of a symbol in a binary.  Supports 32- and
 * 64-bit ELF.  Walks the section headers to find .symtab (falling back to
 * .dynsym), then converts the symbol's virtual address to a file offset
 * via the PT_LOAD program headers.
 *
 * Does not require /dev/rv; used by the shell script to build uprobe
 * bindings of the form pid:threshold_us:offset_start:offset_stop:binary_path.
 *
 * Returns 0 on success (offset printed to stdout), 1 on failure.
 * -----------------------------------------------------------------------
 */
static int sym_offset(const char *binary, const char *symname)
{
	int fd;
	struct stat st;
	void *map;
	Elf64_Ehdr *ehdr;
	Elf32_Ehdr *ehdr32;
	int is64;
	uint64_t sym_vaddr = 0;
	int found = 0;
	uint64_t file_offset = 0;

	fd = open(binary, O_RDONLY);
	if (fd < 0) {
		fprintf(stderr, "open %s: %s\n", binary, strerror(errno));
		return 1;
	}
	if (fstat(fd, &st) < 0) {
		close(fd);
		return 1;
	}
	map = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
	close(fd);
	if (map == MAP_FAILED) {
		fprintf(stderr, "mmap: %s\n", strerror(errno));
		return 1;
	}

	/* Identify ELF class */
	ehdr = (Elf64_Ehdr *)map;
	ehdr32 = (Elf32_Ehdr *)map;
	if (st.st_size < 4 ||
	    ehdr->e_ident[EI_MAG0] != ELFMAG0 ||
	    ehdr->e_ident[EI_MAG1] != ELFMAG1 ||
	    ehdr->e_ident[EI_MAG2] != ELFMAG2 ||
	    ehdr->e_ident[EI_MAG3] != ELFMAG3) {
		fprintf(stderr, "%s: not an ELF file\n", binary);
		munmap(map, (size_t)st.st_size);
		return 1;
	}
	is64 = (ehdr->e_ident[EI_CLASS] == ELFCLASS64);

	if (is64) {
		/* Walk section headers to find .symtab or .dynsym */
		Elf64_Shdr *shdrs = (Elf64_Shdr *)((char *)map + ehdr->e_shoff);
		Elf64_Shdr *shstrtab_hdr = &shdrs[ehdr->e_shstrndx];
		const char *shstrtab = (char *)map + shstrtab_hdr->sh_offset;
		int si;

		/* Prefer .symtab; fall back to .dynsym */
		for (int pass = 0; pass < 2 && !found; pass++) {
			const char *target = pass ? ".dynsym" : ".symtab";

			for (si = 0; si < ehdr->e_shnum && !found; si++) {
				Elf64_Shdr *sh = &shdrs[si];
				const char *name = shstrtab + sh->sh_name;

				if (strcmp(name, target) != 0)
					continue;

				Elf64_Shdr *strtab_sh = &shdrs[sh->sh_link];
				const char *strtab = (char *)map + strtab_sh->sh_offset;
				Elf64_Sym *syms = (Elf64_Sym *)((char *)map + sh->sh_offset);
				uint64_t nsyms = sh->sh_size / sizeof(Elf64_Sym);
				uint64_t j;

				for (j = 0; j < nsyms; j++) {
					if (strcmp(strtab + syms[j].st_name, symname) == 0) {
						sym_vaddr = syms[j].st_value;
						found = 1;
						break;
					}
				}
			}
		}

		if (!found) {
			fprintf(stderr, "symbol '%s' not found in %s\n", symname, binary);
			munmap(map, (size_t)st.st_size);
			return 1;
		}

		/* Convert vaddr to file offset via PT_LOAD segments */
		Elf64_Phdr *phdrs = (Elf64_Phdr *)((char *)map + ehdr->e_phoff);
		int pi;

		for (pi = 0; pi < ehdr->e_phnum; pi++) {
			Elf64_Phdr *ph = &phdrs[pi];

			if (ph->p_type != PT_LOAD)
				continue;
			if (sym_vaddr >= ph->p_vaddr &&
			    sym_vaddr < ph->p_vaddr + ph->p_filesz) {
				file_offset = sym_vaddr - ph->p_vaddr + ph->p_offset;
				break;
			}
		}
	} else {
		/* 32-bit ELF */
		Elf32_Shdr *shdrs = (Elf32_Shdr *)((char *)map + ehdr32->e_shoff);
		Elf32_Shdr *shstrtab_hdr = &shdrs[ehdr32->e_shstrndx];
		const char *shstrtab = (char *)map + shstrtab_hdr->sh_offset;
		int si;
		uint32_t sym_vaddr32 = 0;

		for (int pass = 0; pass < 2 && !found; pass++) {
			const char *target = pass ? ".dynsym" : ".symtab";

			for (si = 0; si < ehdr32->e_shnum && !found; si++) {
				Elf32_Shdr *sh = &shdrs[si];
				const char *name = shstrtab + sh->sh_name;

				if (strcmp(name, target) != 0)
					continue;

				Elf32_Shdr *strtab_sh = &shdrs[sh->sh_link];
				const char *strtab = (char *)map + strtab_sh->sh_offset;
				Elf32_Sym *syms = (Elf32_Sym *)((char *)map + sh->sh_offset);
				uint32_t nsyms = sh->sh_size / sizeof(Elf32_Sym);
				uint32_t j;

				for (j = 0; j < nsyms; j++) {
					if (strcmp(strtab + syms[j].st_name, symname) == 0) {
						sym_vaddr32 = syms[j].st_value;
						found = 1;
						break;
					}
				}
			}
		}

		if (!found) {
			fprintf(stderr, "symbol '%s' not found in %s\n", symname, binary);
			munmap(map, (size_t)st.st_size);
			return 1;
		}

		Elf32_Phdr *phdrs = (Elf32_Phdr *)((char *)map + ehdr32->e_phoff);
		int pi;

		for (pi = 0; pi < ehdr32->e_phnum; pi++) {
			Elf32_Phdr *ph = &phdrs[pi];

			if (ph->p_type != PT_LOAD)
				continue;
			if (sym_vaddr32 >= ph->p_vaddr &&
			    sym_vaddr32 < ph->p_vaddr + ph->p_filesz) {
				file_offset = sym_vaddr32 - ph->p_vaddr + ph->p_offset;
				break;
			}
		}
		sym_vaddr = sym_vaddr32;
	}

	munmap(map, (size_t)st.st_size);

	if (!file_offset && sym_vaddr) {
		fprintf(stderr, "could not map vaddr 0x%lx to file offset\n",
			(unsigned long)sym_vaddr);
		return 1;
	}

	printf("0x%lx\n", (unsigned long)file_offset);
	return 0;
}

int main(int argc, char *argv[])
{
	int rc;

	if (argc < 2) {
		fprintf(stderr, "Usage: %s <subcommand> [args...]\n", argv[0]);
		return 1;
	}

	/* sym_offset does not need /dev/rv */
	if (strcmp(argv[1], "sym_offset") == 0) {
		if (argc < 4) {
			fprintf(stderr, "Usage: %s sym_offset <binary> <symbol>\n",
				argv[0]);
			return 1;
		}
		return sym_offset(argv[2], argv[3]);
	}

	if (open_rv() < 0)
		return 2; /* skip */

	if (strcmp(argv[1], "not_enabled") == 0)
		rc = test_not_enabled();
	else if (strcmp(argv[1], "within_budget") == 0)
		rc = test_within_budget();
	else if (strcmp(argv[1], "over_budget_cpu") == 0)
		rc = test_over_budget_cpu();
	else if (strcmp(argv[1], "over_budget_sleep") == 0)
		rc = test_over_budget_sleep();
	else if (strcmp(argv[1], "double_start") == 0)
		rc = test_double_start();
	else if (strcmp(argv[1], "stop_no_start") == 0)
		rc = test_stop_no_start();
	else if (strcmp(argv[1], "multi_thread") == 0)
		rc = test_multi_thread();
	else if (strcmp(argv[1], "self_watch") == 0)
		rc = test_self_watch();
	else if (strcmp(argv[1], "invalid_flags") == 0)
		rc = test_invalid_flags();
	else if (strcmp(argv[1], "notify_fd_bad") == 0)
		rc = test_notify_fd_bad();
	else if (strcmp(argv[1], "mmap_basic") == 0)
		rc = test_mmap_basic();
	else if (strcmp(argv[1], "mmap_errors") == 0)
		rc = test_mmap_errors();
	else if (strcmp(argv[1], "mmap_consume") == 0)
		rc = test_mmap_consume();
	else {
		fprintf(stderr, "Unknown test: %s\n", argv[1]);
		rc = 1;
	}

	close(rv_fd);
	return rc;
}
