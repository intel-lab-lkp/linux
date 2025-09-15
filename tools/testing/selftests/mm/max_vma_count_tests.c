// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2025 Google LLC
 */
#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <errno.h>
#include <stdbool.h>
#include <linux/prctl.h>  /* Definition of PR_* constants */
#include <sys/prctl.h>

#include "../kselftest.h"

static int get_max_vma_count(void);
static bool set_max_vma_count(int val);
static int get_current_vma_count(void);
static bool is_current_vma_count(const char *msg, int expected);
static bool is_test_area_mapped(const char *msg);
static void print_surrounding_maps(const char *msg);

/* Globals initialized in test_suite_setup() */
static int MAX_VMA_COUNT;
static int ORIGINAL_MAX_VMA_COUNT;
static int PAGE_SIZE;
static int GUARD_SIZE;
static int TEST_AREA_SIZE;
static int EXTRA_MAP_SIZE;

static int MAX_VMA_COUNT;

static int NR_EXTRA_MAPS;

static char *TEST_AREA;
static char *EXTRA_MAPS;

#define DEFAULT_MAX_MAP_COUNT	65530
#define TEST_AREA_NR_PAGES	3
/* 1 before test area + 1 after test area + 1 after extra mappings */
#define NR_GUARDS		3
#define TEST_AREA_PROT		(PROT_NONE)
#define EXTRA_MAP_PROT		(PROT_NONE)

/**
 * test_suite_setup - Set up the VMA layout for VMA count testing.
 *
 * Sets up the following VMA layout:
 *
 * +----- base_addr
 * |
 * V
 * +--------------+----------------------+--------------+----------------+--------------+----------------+--------------+-----+----------------+--------------+
 * |  Guard Page  |                      |  Guard Page  |  Extra Map 1   | Unmapped Gap |  Extra Map 2   | Unmapped Gap | ... |  Extra Map N   | Unmapped Gap |
 * |  (unmapped)  |      TEST_AREA       |  (unmapped)  | (mapped page)  |  (1 page)    | (mapped page)  |  (1 page)    | ... | (mapped page)  |  (1 page)    |
 * |   (1 page)   | (unmapped, 3 pages)  |   (1 page)   |    (1 page)    |              |    (1 page)    |              |     |    (1 page)    |              |
 * +--------------+----------------------+--------------+----------------+--------------+----------------+--------------+-----+----------------+--------------+
 * ^              ^                      ^              ^                                                                  ^
 * |              |                      |              |                                                                  |
 * +--GUARD_SIZE--+                      |              +-- EXTRA_MAPS points here             Sufficient EXTRA_MAPS to ---+
 *    (PAGE_SIZE) |                      |                                                         reach MAX_VMA_COUNT
 *                |                      |
 *                +--- TEST_AREA_SIZE ---+
 *                |   (3 * PAGE_SIZE)    |
 *                ^
 *                |
 *                +-- TEST_AREA starts here
 *
 * Populates TEST_AREA and other globals required for the tests.
 * If successful, the current VMA count will be MAX_VMA_COUNT - 1.
 *
 * Return: true on success, false on failure.
 */
static bool test_suite_setup(void)
{
	int initial_vma_count;
	size_t reservation_size;
	void *base_addr = NULL;
	char *ptr = NULL;

	ksft_print_msg("Setting up vma_max_count test suite...\n");

	/* Initialize globals */
	PAGE_SIZE = sysconf(_SC_PAGESIZE);
	TEST_AREA_SIZE = TEST_AREA_NR_PAGES * PAGE_SIZE;
	GUARD_SIZE = PAGE_SIZE;
	EXTRA_MAP_SIZE = PAGE_SIZE;
	MAX_VMA_COUNT = get_max_vma_count();

	MAX_VMA_COUNT = get_max_vma_count();
	if (MAX_VMA_COUNT < 0) {
		ksft_print_msg("Failed to read /proc/sys/vm/max_map_count\n");
		return false;
	}

	/*
	 * If the current limit is higher than the kernel default,
	 * we attempt to lower it to the default to ensure the test
	 * can run with a reliably known boundary.
	 */
	ORIGINAL_MAX_VMA_COUNT = 0;

	if (MAX_VMA_COUNT > DEFAULT_MAX_MAP_COUNT) {
		ORIGINAL_MAX_VMA_COUNT = MAX_VMA_COUNT;

		ksft_print_msg("Max VMA count is %d, lowering to default %d for test...\n",
				MAX_VMA_COUNT, DEFAULT_MAX_MAP_COUNT);

		if (!set_max_vma_count(DEFAULT_MAX_MAP_COUNT)) {
			ksft_print_msg("WARNING: Failed to lower max_map_count to %d (requires root)n",
					DEFAULT_MAX_MAP_COUNT);
			ksft_print_msg("Skipping test. Please run as root: limit needs adjustment\n");

			MAX_VMA_COUNT = ORIGINAL_MAX_VMA_COUNT;

			return false;
		}

		/* Update MAX_VMA_COUNT for the test run */
		MAX_VMA_COUNT = DEFAULT_MAX_MAP_COUNT;
	}

	initial_vma_count = get_current_vma_count();
	if (initial_vma_count < 0) {
		ksft_print_msg("Failed to read /proc/self/maps\n");
		return false;
	}

	/*
	 * Calculate how many extra mappings we need to create to reach
	 * MAX_VMA_COUNT - 1 (excluding test area).
	 */
	NR_EXTRA_MAPS = MAX_VMA_COUNT - 1 - initial_vma_count;

	if (NR_EXTRA_MAPS < 1) {
		ksft_print_msg("Not enough available maps to run test\n");
		ksft_print_msg("max_vma_count=%d, current_vma_count=%d\n",
				MAX_VMA_COUNT, initial_vma_count);
		return false;
	}

	/*
	 * Reserve space for:
	 * - Extra mappings with a 1-page gap after each (NR_EXTRA_MAPS * 2)
	 * - The test area itself (TEST_AREA_NR_PAGES)
	 * - The guard pages (NR_GUARDS)
	 */
	reservation_size = ((NR_EXTRA_MAPS * 2) +
				TEST_AREA_NR_PAGES + NR_GUARDS) * PAGE_SIZE;

	base_addr = mmap(NULL, reservation_size, PROT_NONE,
			MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
	if (base_addr == MAP_FAILED) {
		ksft_print_msg("Failed tommap initial reservation\n");
		return false;
	}

	if (munmap(base_addr, reservation_size) == -1) {
		ksft_print_msg("Failed to munmap initial reservation\n");
		return false;
	}

	/* Get the addr of the test area */
	TEST_AREA = (char *)base_addr + GUARD_SIZE;

	/*
	 * Get the addr of the region for extra mappings:
	 *     test area + 1 guard.
	 */
	EXTRA_MAPS = TEST_AREA + TEST_AREA_SIZE + GUARD_SIZE;

	/* Create single-page mappings separated by unmapped pages */
	ptr = EXTRA_MAPS;
	for (int i = 0; i < NR_EXTRA_MAPS; ++i) {
		if (mmap(ptr, PAGE_SIZE, EXTRA_MAP_PROT,
			MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,
			-1, 0) == MAP_FAILED) {
			perror("mmap in fill loop");
			ksft_print_msg("Failed on mapping #%d of %d\n", i + 1,
					NR_EXTRA_MAPS);
			return false;
		}

		/* Advance pointer by 2 to leave a gap */
		ptr += (2 * EXTRA_MAP_SIZE);
	}

	if (!is_current_vma_count("test_suite_setup", MAX_VMA_COUNT - 1))
		return false;

	ksft_print_msg("vma_max_count test suite setup done.\n");

	return true;
}

static void test_suite_teardown(void)
{
	if (ORIGINAL_MAX_VMA_COUNT && MAX_VMA_COUNT != ORIGINAL_MAX_VMA_COUNT) {
		if (!set_max_vma_count(ORIGINAL_MAX_VMA_COUNT))
			ksft_print_msg("Failed to restore max_map_count to %d\n",
					ORIGINAL_MAX_VMA_COUNT);
	}
}

/* --- Test Helper Functions --- */
static bool mmap_anon(void)
{
	void *addr =  mmap(NULL, PAGE_SIZE, PROT_READ,
			   MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);

	/*
	 * Handle cleanup here as the runner doesn't track where this,
	 *mapping is located.
	 */
	if (addr != MAP_FAILED)
		munmap(addr, PAGE_SIZE);

	return addr != MAP_FAILED;
}

static inline bool __mprotect(char *addr, int size)
{
	int new_prot = ~TEST_AREA_PROT & (PROT_READ | PROT_WRITE | PROT_EXEC);

	return mprotect(addr, size, new_prot) == 0;
}

static bool mprotect_nosplit(void)
{
	return __mprotect(TEST_AREA, TEST_AREA_SIZE);
}

static bool mprotect_2way_split(void)
{
	return __mprotect(TEST_AREA, TEST_AREA_SIZE - PAGE_SIZE);
}

static bool mprotect_3way_split(void)
{
	return __mprotect(TEST_AREA + PAGE_SIZE, PAGE_SIZE);
}

static inline bool __munmap(char *addr, int size)
{
	return munmap(addr, size) == 0;
}

static bool munmap_nosplit(void)
{
	return __munmap(TEST_AREA, TEST_AREA_SIZE);
}

static bool munmap_2way_split(void)
{
	return __munmap(TEST_AREA, TEST_AREA_SIZE - PAGE_SIZE);
}

static bool munmap_3way_split(void)
{
	return __munmap(TEST_AREA + PAGE_SIZE, PAGE_SIZE);
}

/* mremap accounts for the worst case to fail early */
static const int MREMAP_REQUIRED_VMA_SLOTS = 6;

static bool mremap_dontunmap(void)
{
	void *new_addr;

	/*
	 * Using MREMAP_DONTUNMAP will create a new mapping without
	 * removing the old one, consuming one VMA slot.
	 */
	new_addr = mremap(TEST_AREA, TEST_AREA_SIZE, TEST_AREA_SIZE,
			  MREMAP_MAYMOVE | MREMAP_DONTUNMAP, NULL);

	if (new_addr != MAP_FAILED)
		munmap(new_addr, TEST_AREA_SIZE);

	return new_addr != MAP_FAILED;
}

struct test {
	const char *name;
	bool (*test)(void);
	 /* How many VMA slots below the limit this test needs to start? */
	int vma_slots_needed;
	bool expect_success;
};

/* --- Test Cases --- */
struct test tests[] = {
	{
		.name = "mmap_at_1_below_vma_count_limit",
		.test = mmap_anon,
		.vma_slots_needed = 1,
		.expect_success = true,
	},
	{
		.name = "mmap_at_vma_count_limit",
		.test = mmap_anon,
		.vma_slots_needed = 0,
		.expect_success = false,
	},
	{
		.name = "mprotect_nosplit_at_1_below_vma_count_limit",
		.test = mprotect_nosplit,
		.vma_slots_needed = 1,
		.expect_success = true,
	},
	{
		.name = "mprotect_nosplit_at_vma_count_limit",
		.test = mprotect_nosplit,
		.vma_slots_needed = 0,
		.expect_success = true,
	},
	{
		.name = "mprotect_2way_split_at_1_below_vma_count_limit",
		.test = mprotect_2way_split,
		.vma_slots_needed = 1,
		.expect_success = true,
	},
	{
		.name = "mprotect_2way_split_at_vma_count_limit",
		.test = mprotect_2way_split,
		.vma_slots_needed = 0,
		.expect_success = false,
	},
	{
		.name = "mprotect_3way_split_at_2_below_vma_count_limit",
		.test = mprotect_3way_split,
		.vma_slots_needed = 2,
		.expect_success = true,
	},
	{
		.name = "mprotect_3way_split_at_1_below_vma_count_limit",
		.test = mprotect_3way_split,
		.vma_slots_needed = 1,
		.expect_success = false,
	},
	{
		.name = "mprotect_3way_split_at_vma_count_limit",
		.test = mprotect_3way_split,
		.vma_slots_needed = 0,
		.expect_success = false,
	},
	{
		.name = "munmap_nosplit_at_1_below_vma_count_limit",
		.test = munmap_nosplit,
		.vma_slots_needed = 1,
		.expect_success = true,
	},
	{
		.name = "munmap_nosplit_at_vma_count_limit",
		.test = munmap_nosplit,
		.vma_slots_needed = 0,
		.expect_success = true,
	},
	{
		.name = "munmap_2way_split_at_1_below_vma_count_limit",
		.test = munmap_2way_split,
		.vma_slots_needed = 1,
		.expect_success = true,
	},
	{
		.name = "munmap_2way_split_at_vma_count_limit",
		.test = munmap_2way_split,
		.vma_slots_needed = 0,
		.expect_success = true,
	},
	{
		.name = "munmap_3way_split_at_2_below_vma_count_limit",
		.test = munmap_3way_split,
		.vma_slots_needed = 2,
		.expect_success = true,
	},
	{
		.name = "munmap_3way_split_at_1_below_vma_count_limit",
		.test = munmap_3way_split,
		.vma_slots_needed = 1,
		.expect_success = true,
	},
	{
		.name = "munmap_3way_split_at_vma_count_limit",
		.test = munmap_3way_split,
		.vma_slots_needed = 0,
		.expect_success = false,
	},
	{
		.name = "mremap_dontunmap_at_required_vma_count_capcity",
		.test = mremap_dontunmap,
		.vma_slots_needed = MREMAP_REQUIRED_VMA_SLOTS,
		.expect_success = true,
	},
	{
		.name = "mremap_dontunmap_at_1_below_required_vma_count_capacity",
		.test = mremap_dontunmap,
		.vma_slots_needed = MREMAP_REQUIRED_VMA_SLOTS - 1,
		.expect_success = false,
	},
};

/* --- Test Runner --- */
int main(int argc, char **argv)
{
	int num_tests = ARRAY_SIZE(tests);
	int failed_tests = 0;

	ksft_set_plan(num_tests);

	if (!test_suite_setup() != 0) {
		if (MAX_VMA_COUNT > DEFAULT_MAX_MAP_COUNT)
			ksft_exit_skip("max_map_count too high and cannot be lowered\n"
					"Please rerun as root.\n");
		else
			ksft_exit_fail_msg("Test suite setup failed. Aborting.\n");

	}

	for (int i = 0; i < num_tests; i++) {
		int maps_to_unmap = tests[i].vma_slots_needed;
		const char *name = tests[i].name;
		bool test_passed;

		errno = 0;

		/* 1. Setup: TEST_AREA mapping */
		if (mmap(TEST_AREA, TEST_AREA_SIZE, TEST_AREA_PROT,
			MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED, -1, 0)
				== MAP_FAILED) {
			ksft_test_result_fail(
				"%s: Test setup failed to map TEST_AREA\n",
				name);
			maps_to_unmap = 0;
			goto fail;
		}

		/* Label TEST_AREA to ease debugging */
		if (prctl(PR_SET_VMA, PR_SET_VMA_ANON_NAME, TEST_AREA,
			TEST_AREA_SIZE, "TEST_AREA")) {
			ksft_print_msg("WARNING: [%s] prctl(PR_SET_VMA) failed\n",
					name);
			ksft_print_msg(
				"Continuing without named TEST_AREA mapping\n");
		}

		/* 2. Setup: Adjust VMA count based on test requirements */
		if (maps_to_unmap > NR_EXTRA_MAPS) {
			ksft_test_result_fail(
				"%s: Test setup failed: Invalid VMA slots required %d\n",
				name, tests[i].vma_slots_needed);
			maps_to_unmap = 0;
			goto fail;
		}

		/* Unmap extra mappings, accounting for the 1-page gap */
		for (int j = 0; j < maps_to_unmap; j++)
			munmap(EXTRA_MAPS + (j * 2 * EXTRA_MAP_SIZE),
				EXTRA_MAP_SIZE);

		/*
		 * 3. Verify the preconditions.
		 *
		 *    Sometimes there isn't an easy way to determine the cause
		 *    of the test failure.
		 *    e.g. an mprotect ENOMEM may be due to trying to protect
		 *         unmapped area or due to hitting MAX_VMA_COUNT limit.
		 *
		 *    We verify the preconditions of the test to ensure any
		 *    expected failures are from the expected cause and not
		 *    coincidental.
		 */
		if (!is_current_vma_count(name,
			MAX_VMA_COUNT - tests[i].vma_slots_needed))
			goto fail;

		if (!is_test_area_mapped(name))
			goto fail;

		/* 4. Run the test */
		test_passed = (tests[i].test() == tests[i].expect_success);
		if (test_passed) {
			ksft_test_result_pass("%s\n", name);
		} else {
fail:
			failed_tests++;
			ksft_test_result_fail(
				"%s: current_vma_count=%d,max_vma_count=%d: errno: %d (%s)\n",
				name, get_current_vma_count(), MAX_VMA_COUNT,
				errno, strerror(errno));
			print_surrounding_maps(name);
		}

		/* 5. Teardown: Unmap TEST_AREA. */
		munmap(TEST_AREA, TEST_AREA_SIZE);

		/* 6. Teardown: Restore extra mappings to test suite baseline */
		for (int j = 0; j < maps_to_unmap; j++) {
			/* Remap extra mappings, accounting for the gap */
			mmap(EXTRA_MAPS + (j * 2 * EXTRA_MAP_SIZE),
				EXTRA_MAP_SIZE, EXTRA_MAP_PROT,
				MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,
				-1, 0);
		}
	}

	test_suite_teardown();

	if (failed_tests > 0)
		ksft_exit_fail();
	else
		ksft_exit_pass();
}

/* --- Utilities --- */

static int get_max_vma_count(void)
{
	int max_count;
	FILE *f;

	f = fopen("/proc/sys/vm/max_map_count", "r");
	if (!f)
		return -1;

	if (fscanf(f, "%d", &max_count) != 1)
		max_count = -1;


	fclose(f);

	return max_count;
}

static bool set_max_vma_count(int val)
{
	FILE *f;
	bool success = false;

	f = fopen("/proc/sys/vm/max_map_count", "w");
	if (!f)
		return false;

	if (fprintf(f, "%d", val) > 0)
		success = true;

	fclose(f);
	return success;
}

static int get_current_vma_count(void)
{
	char line[1024];
	int count = 0;
	FILE *f;

	f = fopen("/proc/self/maps", "r");
	if (!f)
		return -1;

	while (fgets(line, sizeof(line), f)) {
		if (!strstr(line, "[vsyscall]"))
			count++;
	}

	fclose(f);

	return count;
}

static bool is_current_vma_count(const char *msg, int expected)
{
	int current = get_current_vma_count();

	if (current == expected)
		return true;

	ksft_print_msg("%s: vma count is %d, expected %d\n", msg, current,
			expected);
	return false;
}

static bool is_test_area_mapped(const char *msg)
{
	unsigned long search_start = (unsigned long)TEST_AREA;
	unsigned long search_end = search_start + TEST_AREA_SIZE;
	bool found = false;
	char line[1024];
	FILE *f;

	f = fopen("/proc/self/maps", "r");
	if (!f) {
		ksft_print_msg("failed to open /proc/self/maps\n");
		return false;
	}

	while (fgets(line, sizeof(line), f)) {
		unsigned long start, end;

		if (sscanf(line, "%lx-%lx", &start, &end) != 2)
			continue;

		/* Check for an exact match of the range */
		if (start == search_start && end == search_end) {
			found = true;
			break;
		} else if (start > search_end) {
			/*
			 *Since maps are sorted, if we've passed the end, we
			 * can stop searching.
			 */
			break;
		}
	}

	fclose(f);

	if (found)
		return true;

	/* Not found */
	ksft_print_msg(
		"%s: TEST_AREA is not mapped as a single contiguous block.\n",
		msg);
	print_surrounding_maps(msg);

	return false;
}

static void print_surrounding_maps(const char *msg)
{
	unsigned long search_start = (unsigned long)TEST_AREA;
	unsigned long search_end = search_start + TEST_AREA_SIZE;
	unsigned long start;
	unsigned long end;
	char line[1024] = {};
	int line_idx = 0;
	int first_match_idx = -1;
	int last_match_idx = -1;
	FILE *f;

	f = fopen("/proc/self/maps", "r");
	if (!f)
		return;

	if (msg)
		ksft_print_msg("%s\n", msg);

	ksft_print_msg("--- Surrounding VMA entries for TEST_AREA (%p) ---\n",
			TEST_AREA);

	/* First pass: Read all lines and find the range of matching entries */
	fseek(f, 0, SEEK_SET); /* Rewind file */
	while (fgets(line, sizeof(line), f)) {
		if (sscanf(line, "%lx-%lx", &start, &end) != 2) {
			line_idx++;
			continue;
		}

		/* Check for any overlap */
		if (start < search_end && end > search_start) {
			if (first_match_idx == -1)
				first_match_idx = line_idx;
			last_match_idx = line_idx;
		} else if (start > search_end) {
			/*
			 * Since maps are sorted, if we've passed the end, we
			 * can stop searching.
			 */
			break;
		}

		line_idx++;
	}

	if (first_match_idx == -1) {
		ksft_print_msg("TEST_AREA (%p) is not currently mapped.\n",
				TEST_AREA);
	} else {
		/* Second pass: Print the relevant lines */
		fseek(f, 0, SEEK_SET); /* Rewind file */
		line_idx = 0;
		while (fgets(line, sizeof(line), f)) {
			/* Print 2 lines before the first match */
			if (line_idx >= first_match_idx - 2 &&
				line_idx < first_match_idx)
				ksft_print_msg("   %s", line);

			/* Print all matching TEST_AREA entries */
			if (line_idx >= first_match_idx &&
				line_idx <= last_match_idx)
				ksft_print_msg(">> %s", line);

			/* Print 2 lines after the last match */
			if (line_idx > last_match_idx &&
				line_idx <= last_match_idx + 2)
				ksft_print_msg("   %s", line);

			line_idx++;
		}
	}

	ksft_print_msg("--------------------------------------------------\n");

	fclose(f);
}
