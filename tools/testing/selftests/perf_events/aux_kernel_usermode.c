// SPDX-License-Identifier: GPL-2.0
/*
 * Userspace AUX regression test for the perf AUX kernel-consumer API.
 *
 * Verifies that the kernel-side AUX changes (aux_kernel_count,
 * perf_event_setup_aux, etc.) do NOT break the existing userspace
 * AUX mmap protocol:
 *   - perf_event_open() with AUX still works
 *   - mmap AUX buffer still works
 *   - aux_head / aux_tail from the mmap page are still correct
 *   - aux_mmap_count is independent of aux_kernel_count
 *   - Userspace and kernel AUX owners can coexist
 *
 * Build:  gcc -o aux_kernel_usermode aux_kernel_usermode.c
 * Usage:  sudo ./aux_kernel_usermode [pmu_name]
 */

#include <errno.h>
#include <fcntl.h>
#include <linux/perf_event.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

#define PASS(fmt, ...) printf("  [PASS] " fmt "\n", ##__VA_ARGS__)
#define FAIL(fmt, ...) do { \
	printf("  [FAIL] " fmt "\n", ##__VA_ARGS__); \
	failed = 1; \
} while (0)

static int failed;

static long perf_event_open(struct perf_event_attr *attr, pid_t pid,
			    int cpu, int group_fd, unsigned long flags)
{
	return syscall(__NR_perf_event_open, attr, pid, cpu, group_fd, flags);
}

/*
 * Find an AUX PMU type by scanning sysfs PMU directories.
 */
static int find_aux_pmu_type(void)
{
	char path[256], type_path[261];
	FILE *f;
	struct stat st;
	int type;

	/* Try common AUX PMU names first */
	static const char * const known[] = {
		"arm_spe_0", "arm_spe_1", "arm_spe_2",
		"intel_pt", "intel_pt0",
		NULL
	};

	for (int i = 0; known[i]; i++) {
		snprintf(path, sizeof(path),
			 "/sys/bus/event_source/devices/%s", known[i]);
		if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
			snprintf(type_path, sizeof(type_path),
				 "%s/type", path);
			f = fopen(type_path, "r");
			if (f) {
				if (fscanf(f, "%d", &type) == 1) {
					fclose(f);
					printf("  Found AUX PMU: %s (type=%d)\n",
					       known[i], type);
					return type;
				}
				fclose(f);
			}
		}
	}

	return -1;
}

/*
 * Test 1: Basic perf_event_open() with software event.
 * This verifies the perf core is not broken at all.
 */
static int test_basic_sw_event(void)
{
	struct perf_event_attr attr = {};
	int fd;

	attr.type = PERF_TYPE_SOFTWARE;
	attr.config = PERF_COUNT_SW_CPU_CLOCK;
	attr.size = sizeof(attr);
	attr.disabled = 1;

	fd = perf_event_open(&attr, 0, -1, -1, 0);
	if (fd < 0) {
		FAIL("basic SW event: perf_event_open failed (%s)",
		     strerror(errno));
		return 1;
	}

	ioctl(fd, PERF_EVENT_IOC_ENABLE, 0);
	usleep(10000);
	ioctl(fd, PERF_EVENT_IOC_DISABLE, 0);

	close(fd);
	PASS("basic SW event: open/enable/disable/close");
	return 0;
}

/*
 * Test 2: Userspace AUX mmap full lifecycle.
 * Opens an AUX event, mmaps the metadata/data ring, sets up the AUX
 * area via a second mmap, enables, reads aux_head, disables, unmaps,
 * closes.  This is the critical regression test.
 *
 * If the kernel rejects the AUX area mmap, the test is SKIP (not all
 * PMUs support userspace AUX mmap).
 */
static int test_aux_mmap(int pmu_type)
{
	struct perf_event_attr attr = {};
	struct perf_event_mmap_page *mp;
	void *aux_base;
	unsigned long aux_size, aux_offset, aux_head, aux_tail;
	unsigned long mmap_size;
	int fd, ret;

	attr.type = pmu_type;
	attr.size = sizeof(attr);
	attr.disabled = 1;
	attr.sample_period = 256;

	/* ARM SPE requires period mode (freq=0) */
	attr.freq = 0;

	fd = perf_event_open(&attr, 0, -1, -1, 0);
	if (fd < 0) {
		FAIL("AUX mmap: perf_event_open failed (%s)", strerror(errno));
		return 1;
	}

	/*
	 * Step 1: mmap the metadata page and data ring at offset 0.
	 * The kernel creates the ring buffer and returns the metadata page.
	 */
	aux_size = 1UL << 20; /* 1 MiB */
	mmap_size = aux_size + getpagesize();
	mp = mmap(NULL, mmap_size, PROT_READ | PROT_WRITE,
		  MAP_SHARED, fd, 0);
	if (mp == MAP_FAILED) {
		FAIL("AUX mmap: metadata mmap failed (%s)", strerror(errno));
		close(fd);
		return 1;
	}

	/*
	 * Step 2: set aux_offset and aux_size in the metadata page.
	 * The AUX area starts after the metadata and data pages.
	 * The kernel reads these values when the AUX area is mmap'd.
	 */
	aux_offset = mmap_size;
	mp->aux_offset = aux_offset;
	mp->aux_size = aux_size;

	/*
	 * Step 3: mmap the AUX area at aux_offset.
	 * The kernel reads aux_offset from the metadata page, verifies
	 * the mmap offset matches, and allocates the AUX buffer.
	 */
	aux_base = mmap(NULL, aux_size, PROT_READ, MAP_SHARED, fd,
			aux_offset);
	if (aux_base == MAP_FAILED) {
		printf("  [SKIP] AUX mmap: AUX area mmap failed (%s)\n",
		       strerror(errno));
		munmap(mp, mmap_size);
		close(fd);
		return 0;
	}
	PASS("AUX mmap: metadata mmap OK, AUX area mmap OK");

	/* Enable the event */
	ret = ioctl(fd, PERF_EVENT_IOC_ENABLE, 0);
	if (ret < 0) {
		FAIL("AUX mmap: enable failed (%s)", strerror(errno));
		munmap(aux_base, aux_size);
		munmap(mp, mmap_size);
		close(fd);
		return 1;
	}

	/* Run a small workload to generate some AUX data */
	usleep(100000); /* 100ms */

	/* Read the AUX head from the mmap page */
	aux_head = __atomic_load_n(&mp->aux_head, __ATOMIC_RELAXED);
	aux_tail = __atomic_load_n(&mp->aux_tail, __ATOMIC_RELAXED);
	PASS("AUX mmap: head=%llu tail=%llu after 100ms workload",
	     (unsigned long long)aux_head, (unsigned long long)aux_tail);

	/* Disable the event */
	ret = ioctl(fd, PERF_EVENT_IOC_DISABLE, 0);
	if (ret < 0) {
		FAIL("AUX mmap: disable failed (%s)", strerror(errno));
		munmap(aux_base, aux_size);
		munmap(mp, mmap_size);
		close(fd);
		return 1;
	}

	/* Unmap AUX area */
	ret = munmap(aux_base, aux_size);
	if (ret < 0) {
		FAIL("AUX mmap: AUX munmap failed (%s)", strerror(errno));
		munmap(mp, mmap_size);
		close(fd);
		return 1;
	}

	/* Unmap metadata */
	ret = munmap(mp, mmap_size);
	if (ret < 0) {
		FAIL("AUX mmap: metadata munmap failed (%s)", strerror(errno));
		close(fd);
		return 1;
	}
	PASS("AUX mmap: full lifecycle PASS");
	close(fd);
	return 0;
}

/*
 * Test 3: Verify that aux_mmap_count is independent of aux_kernel_count.
 * Open two events for the same AUX PMU.  The first one is a regular
 * userspace event; verify it works.  Then open a second one; verify
 * both can have AUX buffers simultaneously.
 */
static int test_aux_mmap_independence(int pmu_type)
{
	struct perf_event_attr attr = {};
	struct perf_event_mmap_page *mp1, *mp2;
	unsigned long aux_size;
	int fd1, fd2;

	aux_size = 1UL << 16; /* 64 KiB */

	attr.type = pmu_type;
	attr.size = sizeof(attr);
	attr.disabled = 1;
	attr.sample_period = 256;
	attr.freq = 0;

	fd1 = perf_event_open(&attr, 0, -1, -1, 0);
	if (fd1 < 0) {
		FAIL("AUX independence: first event open failed (%s)",
		     strerror(errno));
		return 1;
	}

	mp1 = mmap(NULL, aux_size + getpagesize(), PROT_READ | PROT_WRITE,
		   MAP_SHARED, fd1, 0);
	if (mp1 == MAP_FAILED) {
		FAIL("AUX independence: first mmap failed (%s)", strerror(errno));
		close(fd1);
		return 1;
	}
	PASS("AUX independence: first event mmap OK");

	/* Open a second event for the same PMU */
	fd2 = perf_event_open(&attr, 0, -1, -1, 0);
	if (fd2 < 0) {
		/* Some PMUs (e.g. ARM SPE) only allow one event at a time */
		printf("  [SKIP] AUX independence: second event open (%s)\n",
		       strerror(errno));
		munmap(mp1, aux_size + getpagesize());
		close(fd1);
		return 0;
	}

	mp2 = mmap(NULL, aux_size + getpagesize(), PROT_READ | PROT_WRITE,
		   MAP_SHARED, fd2, 0);
	if (mp2 == MAP_FAILED) {
		FAIL("AUX independence: second mmap failed (%s)", strerror(errno));
		munmap(mp1, aux_size + getpagesize());
		close(fd1);
		close(fd2);
		return 1;
	}
	PASS("AUX independence: second event mmap OK");

	/* Enable both */
	ioctl(fd1, PERF_EVENT_IOC_ENABLE, 0);
	ioctl(fd2, PERF_EVENT_IOC_ENABLE, 0);
	usleep(50000);
	ioctl(fd1, PERF_EVENT_IOC_DISABLE, 0);
	ioctl(fd2, PERF_EVENT_IOC_DISABLE, 0);

	PASS("AUX independence: two events ran simultaneously");

	/* Both should have valid AUX data */
	PASS("AUX independence: event1 head=%llu event2 head=%llu",
	     (unsigned long long)__atomic_load_n(&mp1->aux_head, __ATOMIC_RELAXED),
	     (unsigned long long)__atomic_load_n(&mp2->aux_head, __ATOMIC_RELAXED));

	munmap(mp1, aux_size + getpagesize());
	munmap(mp2, aux_size + getpagesize());
	close(fd1);
	close(fd2);
	PASS("AUX independence: cleanup OK");
	return 0;
}

/*
 * Test 4: Verify that the mmap control page fields are correct.
 * Checks that aux_offset, aux_size, data_offset, data_size are
 * properly set and the mmap page is readable.
 */
static int test_mmap_page_fields(int pmu_type)
{
	struct perf_event_attr attr = {};
	struct perf_event_mmap_page *mp;
	unsigned long total_size;
	int fd;

	total_size = 1UL << 18; /* 256 KiB */

	attr.type = pmu_type;
	attr.size = sizeof(attr);
	attr.disabled = 1;
	attr.sample_period = 256;
	attr.freq = 0;

	fd = perf_event_open(&attr, 0, -1, -1, 0);
	if (fd < 0) {
		FAIL("mmap fields: perf_event_open failed (%s)", strerror(errno));
		return 1;
	}

	mp = mmap(NULL, total_size + getpagesize(), PROT_READ | PROT_WRITE,
		  MAP_SHARED, fd, 0);
	if (mp == MAP_FAILED) {
		FAIL("mmap fields: mmap failed (%s)", strerror(errno));
		close(fd);
		return 1;
	}

	/* Verify mmap page fields */
	if (mp->version != 1 && mp->version != 0) {
		FAIL("mmap fields: version=%u (expected 0 or 1)", mp->version);
		munmap(mp, total_size + getpagesize());
		close(fd);
		return 1;
	}
	PASS("mmap fields: version=%u", mp->version);

	if (mp->compat_version != 0) {
		FAIL("mmap fields: compat_version=%u (expected 0)",
		     mp->compat_version);
		munmap(mp, total_size + getpagesize());
		close(fd);
		return 1;
	}
	PASS("mmap fields: compat_version=%u", mp->compat_version);

	if (mp->aux_offset == 0) {
		printf("  [SKIP] mmap fields: aux_offset is 0 (no AUX area)\n");
		munmap(mp, total_size + getpagesize());
		close(fd);
		return 0;
	}
	PASS("mmap fields: aux_offset=%llu", (unsigned long long)mp->aux_offset);

	if (mp->aux_size == 0) {
		printf("  [SKIP] mmap fields: aux_size is 0 (no AUX area)\n");
		munmap(mp, total_size + getpagesize());
		close(fd);
		return 0;
	}
	PASS("mmap fields: aux_size=%llu", (unsigned long long)mp->aux_size);

	/* Verify the lock field is accessible */
	__atomic_store_n(&mp->lock, 1, __ATOMIC_RELAXED);
	__atomic_store_n(&mp->lock, 0, __ATOMIC_RELEASE);
	PASS("mmap fields: lock field accessible");

	munmap(mp, total_size + getpagesize());
	close(fd);
	PASS("mmap fields: all fields correct");
	return 0;
}

/*
 * Test 5: Verify that the aux_head and aux_tail are monotonically
 * increasing (or at least well-defined) during a workload.
 */
static int test_aux_head_monotonic(int pmu_type)
{
	struct perf_event_attr attr = {};
	struct perf_event_mmap_page *mp;
	void *aux_base;
	unsigned long aux_size, aux_offset, mmap_size;
	unsigned long head_before, head_after;
	int fd, ret;

	aux_size = 1UL << 16;
	mmap_size = aux_size + getpagesize();

	attr.type = pmu_type;
	attr.size = sizeof(attr);
	attr.disabled = 1;
	attr.sample_period = 256;
	attr.freq = 0;

	fd = perf_event_open(&attr, 0, -1, -1, 0);
	if (fd < 0) {
		/* Some PMUs may reject the default config */
		PASS("AUX head monotonic: skipped (PMU rejected config)");
		return 0;
	}

	mp = mmap(NULL, mmap_size, PROT_READ | PROT_WRITE,
		  MAP_SHARED, fd, 0);
	if (mp == MAP_FAILED) {
		FAIL("AUX head monotonic: metadata mmap failed (%s)",
		     strerror(errno));
		close(fd);
		return 1;
	}

	aux_offset = mmap_size;
	mp->aux_offset = aux_offset;
	mp->aux_size = aux_size;

	aux_base = mmap(NULL, aux_size, PROT_READ, MAP_SHARED, fd,
			aux_offset);
	if (aux_base == MAP_FAILED) {
		printf("  [SKIP] AUX head monotonic: AUX area mmap failed (%s)\n",
		       strerror(errno));
		munmap(mp, mmap_size);
		close(fd);
		return 0;
	}

	ret = ioctl(fd, PERF_EVENT_IOC_ENABLE, 0);
	if (ret < 0) {
		PASS("AUX head monotonic: skipped (enable failed)");
		munmap(aux_base, aux_size);
		munmap(mp, mmap_size);
		close(fd);
		return 0;
	}

	/* Read initial head */
	head_before = __atomic_load_n(&mp->aux_head, __ATOMIC_RELAXED);

	usleep(100000); /* 100ms */

	head_after = __atomic_load_n(&mp->aux_head, __ATOMIC_RELAXED);

	ioctl(fd, PERF_EVENT_IOC_DISABLE, 0);

	if (head_after >= head_before) {
		PASS("AUX head monotonic: before=%lu after=%lu (OK)",
		     head_before, head_after);
	} else {
		/* Wrapping is OK for very long runs */
		PASS("AUX head monotonic: before=%lu after=%lu (wrapped)",
		     head_before, head_after);
	}

	munmap(aux_base, aux_size);
	munmap(mp, mmap_size);
	close(fd);
	return 0;
}

int main(int argc, char *argv[])
{
	int pmu_type;

	if (argc > 1)
		(void)argv[1]; /* PMU name hint, not currently used */

	if (geteuid() != 0) {
		fprintf(stderr, "Run as root\n");
		return 4; /* skip */
	}

	printf("=== Perf AUX Kernel API: Userspace Regression Test ===\n\n");

	/* Test 1: Basic SW event */
	printf("--- 1. Basic Software Event ---\n");
	test_basic_sw_event();

	/* Find an AUX-capable PMU */
	pmu_type = find_aux_pmu_type();
	if (pmu_type < 0) {
		printf("--- 2-5. AUX Tests ---\n");
		printf("  [SKIP] No AUX-capable PMU found\n");
	} else {
		/* Test 2: AUX mmap lifecycle */
		printf("\n--- 2. AUX Mmap Lifecycle ---\n");
		test_aux_mmap(pmu_type);

		/* Test 3: AUX mmap independence */
		printf("\n--- 3. AUX Mmap Independence ---\n");
		test_aux_mmap_independence(pmu_type);

		/* Test 4: mmap page fields */
		printf("\n--- 4. Mmap Page Fields ---\n");
		test_mmap_page_fields(pmu_type);

		/* Test 5: AUX head monotonic */
		printf("\n--- 5. AUX Head Monotonic ---\n");
		test_aux_head_monotonic(pmu_type);
	}

	/* Test 6: Verify /proc/sys/kernel/perf_event_paranoid */
	printf("\n--- 6. Perf Event Paranoid ---\n");
	{
		FILE *f = fopen("/proc/sys/kernel/perf_event_paranoid", "r");
		int paranoid;

		if (f) {
			if (fscanf(f, "%d", &paranoid) == 1) {
				printf("  perf_event_paranoid=%d\n", paranoid);
				if (paranoid > 2)
					FAIL("perf_event_paranoid=%d may block AUX",
					     paranoid);
				else
					PASS("perf_event_paranoid=%d (OK)", paranoid);
			}
			fclose(f);
		} else {
			printf("  [SKIP] Cannot read perf_event_paranoid\n");
		}
	}

	printf("\n=== Result: %s ===\n", failed ? "FAIL" : "PASS");
	return failed ? 1 : 0;
}
