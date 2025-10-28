// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2025 Google LLC
 */
#define _GNU_SOURCE

#include <errno.h>
#include <linux/prctl.h>  /* Definition of PR_* constants */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <unistd.h>

#define TH_LOG_ENABLED			0
#include "../kselftest_harness.h"
#include "vm_util.h"

#define DEFAULT_MAX_VMA_COUNT		65530
#define TEST_AREA_NR_PAGES		3
#define TEST_AREA_PROT			(PROT_NONE)
#define EXTRA_MAP_PROT			(PROT_NONE)

/* mremap accounts for the worst case to fail early */
#define MREMAP_REQUIRED_VMA_SLOTS	6

FIXTURE(max_vma_count) {
	int max_vma_count;
	int original_max_vma_count;
	int test_area_size;
	int nr_extra_maps;
	char *test_area;
	char *extra_maps;
};

/* To keep checkpatch happy */
#define max_vma_count_data_t FIXTURE_DATA(max_vma_count)

static int get_max_vma_count(void);
static bool set_max_vma_count(int val);
static int get_current_vma_count(void);
static bool is_test_area_mapped(char *test_area, int test_area_size);
static bool lower_max_vma_count_if_needed(max_vma_count_data_t *self,
					  struct __test_metadata *_metadata);
static void restore_max_vma_count_if_needed(max_vma_count_data_t *self,
					    struct __test_metadata *_metadata);
static bool free_vma_slots(max_vma_count_data_t *self, int slots_to_free);
static void create_reservation(max_vma_count_data_t *self,
			       struct __test_metadata *_metadata);
static void create_extra_maps(max_vma_count_data_t *self,
			      struct __test_metadata *_metadata);

/**
 * FIXTURE_SETUP - Sets up the VMA layout for max VMA count testing.
 *
 * Sets up a specific VMA layout to test behavior near the max_vma_count limit.
 * A large memory area is reserved and then unmapped to create a contiguous
 * address space. Mappings are then created within this space.
 *
 * The layout is as follows (addresses increase downwards):
 *
 *  base_addr --> +----------------------+
 *                |      Hole (1 page)   |
 *                +----------------------+
 *  TEST_AREA --> |      TEST_AREA       |
 *                | (unmapped, 3 pages)  |
 *                +----------------------+
 *                |      Hole (1 page)   |
 *                +----------------------+
 * EXTRA_MAPS --> | Extra Map 1 (1 page) |
 *                +----------------------+
 *                |      Hole (1 page)   |
 *                +----------------------+
 *                | Extra Map 2 (1 page) |
 *                +----------------------+
 *                |         ...          |
 *                +----------------------+
 *                | Extra Map N (1 page) |
 *                +----------------------+
 *                |      Hole (1 page)   |
 *                +----------------------+
 *
 * "Holes" are unmapped, 1-page gaps used to isolate mappings.
 * The number of "Extra Maps" is calculated to bring the total VMA count
 * to MAX_VMA_COUNT - 1.
 *
 * Populates TEST_AREA and other globals required for the tests.
 *
 * Return: true on success, false on failure.
 */
FIXTURE_SETUP(max_vma_count)
{
	int initial_vma_count;

	TH_LOG("Setting up vma_max_count test ...");

	self->test_area_size = TEST_AREA_NR_PAGES * psize();

	if (!lower_max_vma_count_if_needed(self, _metadata)) {
		SKIP(return,
		     "max_vma_count too high and cannot be lowered. Please rerun as root.");
	}

	initial_vma_count = get_current_vma_count();
	ASSERT_GT(initial_vma_count, 0);

	self->nr_extra_maps = self->max_vma_count - 1 - initial_vma_count;
	if (self->nr_extra_maps < 1) {
		SKIP(return,
		    "Not enough available maps to run test (max: %d, current: %d)",
		     self->max_vma_count, initial_vma_count);
	}

	create_reservation(self, _metadata);
	create_extra_maps(self, _metadata);

	ASSERT_EQ(get_current_vma_count(), self->max_vma_count - 1);
	TH_LOG("vma_max_count test setup done.");
}

FIXTURE_TEARDOWN(max_vma_count)
{
	/*
	 * NOTE: Each test is run in a separate process; we leave
	 * mapping cleanup to process teardown for simplicity.
	 */

	restore_max_vma_count_if_needed(self, _metadata);
}

static bool mmap_anon(max_vma_count_data_t *self)
{
	void *addr =  mmap(NULL, psize(), PROT_READ,
			   MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
	return addr != MAP_FAILED;
}

static inline bool __mprotect(char *addr, int size)
{
	int new_prot = ~TEST_AREA_PROT & (PROT_READ | PROT_WRITE | PROT_EXEC);

	return mprotect(addr, size, new_prot) == 0;
}

static bool mprotect_nosplit(max_vma_count_data_t *self)
{
	return __mprotect(self->test_area, self->test_area_size);
}

static bool mprotect_2way_split(max_vma_count_data_t *self)
{
	return __mprotect(self->test_area, self->test_area_size - psize());
}

static bool mprotect_3way_split(max_vma_count_data_t *self)
{
	return __mprotect(self->test_area + psize(), psize());
}

static inline bool __munmap(char *addr, int size)
{
	return munmap(addr, size) == 0;
}

static bool munmap_nosplit(max_vma_count_data_t *self)
{
	return __munmap(self->test_area, self->test_area_size);
}

static bool munmap_2way_split(max_vma_count_data_t *self)
{
	return __munmap(self->test_area, self->test_area_size - psize());
}

static bool munmap_3way_split(max_vma_count_data_t *self)
{
	return __munmap(self->test_area + psize(), psize());
}

static bool mremap_dontunmap(max_vma_count_data_t *self)
{
	/*
	 * Using MREMAP_DONTUNMAP will create a new mapping without
	 * removing the old one, consuming one VMA slot.
	 */
	return mremap(self->test_area, self->test_area_size,
		      self->test_area_size, MREMAP_MAYMOVE | MREMAP_DONTUNMAP,
		      NULL) != MAP_FAILED;
}

TEST_F(max_vma_count, mmap_at_1_below_vma_count_limit)
{
	int vma_slots_needed = 1;

	ASSERT_NE(mmap(self->test_area, self->test_area_size, TEST_AREA_PROT,
		       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0),
		  MAP_FAILED);

	ASSERT_TRUE(free_vma_slots(self, vma_slots_needed));

	ASSERT_EQ(get_current_vma_count(),
		  self->max_vma_count - vma_slots_needed);
	ASSERT_TRUE(is_test_area_mapped(self->test_area, self->test_area_size));

	ASSERT_TRUE(mmap_anon(self));
}

TEST_F(max_vma_count, mmap_at_vma_count_limit)
{
	int vma_slots_needed = 0;

	ASSERT_NE(mmap(self->test_area, self->test_area_size, TEST_AREA_PROT,
		       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0),
		  MAP_FAILED);

	ASSERT_TRUE(free_vma_slots(self, vma_slots_needed));

	ASSERT_EQ(get_current_vma_count(),
		  self->max_vma_count - vma_slots_needed);
	ASSERT_TRUE(is_test_area_mapped(self->test_area, self->test_area_size));

	/*
	 * Validate the historical lenient behavior of mmap() at the VMA limit.
	 *
	 * Unlike stricter syscalls (e.g., mprotect(), mremap()) that fail
	 * preemptively at the limit, mmap() is allowed to proceed. This is
	 * because the new mapping may merge with an adjacent VMA, in which
	 * case a new VMA slot is not consumed.
	 *
	 * This test confirms that an mmap() call at exactly the
	 * sysctl_max_map_count limit succeeds, preserving this behavior.
	 */
	ASSERT_TRUE(mmap_anon(self));
}

TEST_F(max_vma_count, mmap_at_1_above_vma_count_limit)
{
	/*
	 * Verify the upper bound of the lenient mmap() behavior.
	 *
	 * The previous test confirms mmap() can succeed at the VMA limit,
	 * potentially bringing the count to limit + 1. This test ensures
	 * that this behavior does not permit unrestricted growth.
	 *
	 * We first perform one successful mmap() to exceed the limit, then
	 * assert that the subsequent mmap() call fails as expected.
	 */
	int vma_slots_needed = 0;

	ASSERT_NE(mmap(self->test_area, self->test_area_size, TEST_AREA_PROT,
		       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0),
		  MAP_FAILED);

	ASSERT_TRUE(free_vma_slots(self, vma_slots_needed));

	ASSERT_EQ(get_current_vma_count(),
		  self->max_vma_count - vma_slots_needed);
	ASSERT_TRUE(is_test_area_mapped(self->test_area, self->test_area_size));

	ASSERT_TRUE(mmap_anon(self));

	/*
	 * We are now 1 above the vma_count_limit.
	 * Test that unrestricted growth of VMAs is prevented.
	 */
	ASSERT_FALSE(mmap_anon(self));
}

TEST_F(max_vma_count, mprotect_nosplit_at_1_below_vma_count_limit)
{
	int vma_slots_needed = 1;

	ASSERT_NE(mmap(self->test_area, self->test_area_size, TEST_AREA_PROT,
		       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0),
		  MAP_FAILED);

	ASSERT_TRUE(free_vma_slots(self, vma_slots_needed));

	ASSERT_EQ(get_current_vma_count(),
		  self->max_vma_count - vma_slots_needed);
	ASSERT_TRUE(is_test_area_mapped(self->test_area, self->test_area_size));

	ASSERT_TRUE(mprotect_nosplit(self));
}

TEST_F(max_vma_count, mprotect_nosplit_at_vma_count_limit)
{
	int vma_slots_needed = 0;

	ASSERT_NE(mmap(self->test_area, self->test_area_size, TEST_AREA_PROT,
		       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0),
		  MAP_FAILED);

	ASSERT_TRUE(free_vma_slots(self, vma_slots_needed));

	ASSERT_EQ(get_current_vma_count(),
		  self->max_vma_count - vma_slots_needed);
	ASSERT_TRUE(is_test_area_mapped(self->test_area, self->test_area_size));

	ASSERT_TRUE(mprotect_nosplit(self));
}

TEST_F(max_vma_count, mprotect_2way_split_at_1_below_vma_count_limit)
{
	int vma_slots_needed = 1;

	ASSERT_NE(mmap(self->test_area, self->test_area_size, TEST_AREA_PROT,
		       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0),
		  MAP_FAILED);

	ASSERT_TRUE(free_vma_slots(self, vma_slots_needed));

	ASSERT_EQ(get_current_vma_count(),
		  self->max_vma_count - vma_slots_needed);
	ASSERT_TRUE(is_test_area_mapped(self->test_area, self->test_area_size));

	ASSERT_TRUE(mprotect_2way_split(self));
}

TEST_F(max_vma_count, mprotect_2way_split_at_vma_count_limit)
{
	int vma_slots_needed = 0;

	ASSERT_NE(mmap(self->test_area, self->test_area_size, TEST_AREA_PROT,
		       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0),
		  MAP_FAILED);

	ASSERT_TRUE(free_vma_slots(self, vma_slots_needed));

	ASSERT_EQ(get_current_vma_count(),
		  self->max_vma_count - vma_slots_needed);
	ASSERT_TRUE(is_test_area_mapped(self->test_area, self->test_area_size));

	ASSERT_FALSE(mprotect_2way_split(self));
}

TEST_F(max_vma_count, mprotect_3way_split_at_2_below_vma_count_limit)
{
	int vma_slots_needed = 2;

	ASSERT_NE(mmap(self->test_area, self->test_area_size, TEST_AREA_PROT,
		       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0),
		  MAP_FAILED);

	ASSERT_TRUE(free_vma_slots(self, vma_slots_needed));

	ASSERT_EQ(get_current_vma_count(),
		  self->max_vma_count - vma_slots_needed);
	ASSERT_TRUE(is_test_area_mapped(self->test_area, self->test_area_size));

	ASSERT_TRUE(mprotect_3way_split(self));
}

TEST_F(max_vma_count, mprotect_3way_split_at_1_below_vma_count_limit)
{
	int vma_slots_needed = 1;

	ASSERT_NE(mmap(self->test_area, self->test_area_size, TEST_AREA_PROT,
		       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0),
		  MAP_FAILED);

	ASSERT_TRUE(free_vma_slots(self, vma_slots_needed));

	ASSERT_EQ(get_current_vma_count(),
		  self->max_vma_count - vma_slots_needed);
	ASSERT_TRUE(is_test_area_mapped(self->test_area, self->test_area_size));

	ASSERT_FALSE(mprotect_3way_split(self));
}

TEST_F(max_vma_count, mprotect_3way_split_at_vma_count_limit)
{
	int vma_slots_needed = 0;

	ASSERT_NE(mmap(self->test_area, self->test_area_size, TEST_AREA_PROT,
		       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0),
		  MAP_FAILED);

	ASSERT_TRUE(free_vma_slots(self, vma_slots_needed));

	ASSERT_EQ(get_current_vma_count(),
		  self->max_vma_count - vma_slots_needed);
	ASSERT_TRUE(is_test_area_mapped(self->test_area, self->test_area_size));

	ASSERT_FALSE(mprotect_3way_split(self));
}

TEST_F(max_vma_count, munmap_nosplit_at_1_below_vma_count_limit)
{
	int vma_slots_needed = 1;

	ASSERT_NE(mmap(self->test_area, self->test_area_size, TEST_AREA_PROT,
		       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0),
		  MAP_FAILED);

	ASSERT_TRUE(free_vma_slots(self, vma_slots_needed));

	ASSERT_EQ(get_current_vma_count(),
		  self->max_vma_count - vma_slots_needed);
	ASSERT_TRUE(is_test_area_mapped(self->test_area, self->test_area_size));

	ASSERT_TRUE(munmap_nosplit(self));
}

TEST_F(max_vma_count, munmap_nosplit_at_vma_count_limit)
{
	int vma_slots_needed = 0;

	ASSERT_NE(mmap(self->test_area, self->test_area_size, TEST_AREA_PROT,
		       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0),
		  MAP_FAILED);

	ASSERT_TRUE(free_vma_slots(self, vma_slots_needed));

	ASSERT_EQ(get_current_vma_count(),
		  self->max_vma_count - vma_slots_needed);
	ASSERT_TRUE(is_test_area_mapped(self->test_area, self->test_area_size));

	ASSERT_TRUE(munmap_nosplit(self));
}

TEST_F(max_vma_count, munmap_2way_split_at_1_below_vma_count_limit)
{
	int vma_slots_needed = 1;

	ASSERT_NE(mmap(self->test_area, self->test_area_size, TEST_AREA_PROT,
		       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0),
		  MAP_FAILED);

	ASSERT_TRUE(free_vma_slots(self, vma_slots_needed));

	ASSERT_EQ(get_current_vma_count(),
		  self->max_vma_count - vma_slots_needed);
	ASSERT_TRUE(is_test_area_mapped(self->test_area, self->test_area_size));

	ASSERT_TRUE(munmap_2way_split(self));
}

TEST_F(max_vma_count, munmap_2way_split_at_vma_count_limit)
{
	int vma_slots_needed = 0;

	ASSERT_NE(mmap(self->test_area, self->test_area_size, TEST_AREA_PROT,
		       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0),
		  MAP_FAILED);

	ASSERT_TRUE(free_vma_slots(self, vma_slots_needed));

	ASSERT_EQ(get_current_vma_count(),
		  self->max_vma_count - vma_slots_needed);
	ASSERT_TRUE(is_test_area_mapped(self->test_area, self->test_area_size));

	ASSERT_TRUE(munmap_2way_split(self));
}

TEST_F(max_vma_count, munmap_3way_split_at_2_below_vma_count_limit)
{
	int vma_slots_needed = 2;

	ASSERT_NE(mmap(self->test_area, self->test_area_size, TEST_AREA_PROT,
		       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0),
		  MAP_FAILED);

	ASSERT_TRUE(free_vma_slots(self, vma_slots_needed));

	ASSERT_EQ(get_current_vma_count(),
		  self->max_vma_count - vma_slots_needed);
	ASSERT_TRUE(is_test_area_mapped(self->test_area, self->test_area_size));

	ASSERT_TRUE(munmap_3way_split(self));
}

TEST_F(max_vma_count, munmap_3way_split_at_1_below_vma_count_limit)
{
	int vma_slots_needed = 1;

	ASSERT_NE(mmap(self->test_area, self->test_area_size, TEST_AREA_PROT,
		       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0),
		  MAP_FAILED);

	ASSERT_TRUE(free_vma_slots(self, vma_slots_needed));

	ASSERT_EQ(get_current_vma_count(),
		  self->max_vma_count - vma_slots_needed);
	ASSERT_TRUE(is_test_area_mapped(self->test_area, self->test_area_size));

	ASSERT_TRUE(munmap_3way_split(self));
}

TEST_F(max_vma_count, munmap_3way_split_at_vma_count_limit)
{
	int vma_slots_needed = 0;

	ASSERT_NE(mmap(self->test_area, self->test_area_size, TEST_AREA_PROT,
		       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0),
		  MAP_FAILED);

	ASSERT_TRUE(free_vma_slots(self, vma_slots_needed));

	ASSERT_EQ(get_current_vma_count(),
		  self->max_vma_count - vma_slots_needed);
	ASSERT_TRUE(is_test_area_mapped(self->test_area, self->test_area_size));

	ASSERT_FALSE(munmap_3way_split(self));
}

TEST_F(max_vma_count, mremap_dontunmap_at_required_vma_count_capcity)
{
	int vma_slots_needed = MREMAP_REQUIRED_VMA_SLOTS;

	ASSERT_NE(mmap(self->test_area, self->test_area_size, TEST_AREA_PROT,
		       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0),
		  MAP_FAILED);

	ASSERT_TRUE(free_vma_slots(self, vma_slots_needed));

	ASSERT_EQ(get_current_vma_count(),
		  self->max_vma_count - vma_slots_needed);
	ASSERT_TRUE(is_test_area_mapped(self->test_area, self->test_area_size));

	ASSERT_TRUE(mremap_dontunmap(self));
}

TEST_F(max_vma_count, mremap_dontunmap_at_1_below_required_vma_count_capacity)
{
	int vma_slots_needed = MREMAP_REQUIRED_VMA_SLOTS - 1;

	ASSERT_NE(mmap(self->test_area, self->test_area_size, TEST_AREA_PROT,
		       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0),
		  MAP_FAILED);

	ASSERT_TRUE(free_vma_slots(self, vma_slots_needed));

	ASSERT_EQ(get_current_vma_count(),
		  self->max_vma_count - vma_slots_needed);
	ASSERT_TRUE(is_test_area_mapped(self->test_area, self->test_area_size));

	ASSERT_FALSE(mremap_dontunmap(self));
}

TEST_HARNESS_MAIN

/* --- Utilities --- */

static bool lower_max_vma_count_if_needed(max_vma_count_data_t *self,
			      struct __test_metadata *_metadata)
{
	self->max_vma_count = get_max_vma_count();

	ASSERT_GT(self->max_vma_count, 0);

	self->original_max_vma_count = 0;
	if (self->max_vma_count > DEFAULT_MAX_VMA_COUNT) {
		self->original_max_vma_count = self->max_vma_count;
		TH_LOG("Max VMA count: %d; lowering to default %d for test...",
		       self->max_vma_count, DEFAULT_MAX_VMA_COUNT);

		if (!set_max_vma_count(DEFAULT_MAX_VMA_COUNT))
			return false;
		self->max_vma_count = DEFAULT_MAX_VMA_COUNT;
	}
	return true;
}

static void restore_max_vma_count_if_needed(max_vma_count_data_t *self,
					    struct __test_metadata *_metadata)
{
	if (!self->original_max_vma_count)
		return;

	if (self->max_vma_count == self->original_max_vma_count)
		return;

	if (!set_max_vma_count(self->original_max_vma_count))
		TH_LOG("Failed to restore max_vma_count to %d",
			self->original_max_vma_count);
}

static int get_max_vma_count(void)
{
	unsigned long val;
	int ret;

	ret = read_sysfs("/proc/sys/vm/max_map_count", &val);
	if (ret)
		return -1;
	return val;
}

static bool set_max_vma_count(int val)
{
	return write_sysfs("/proc/sys/vm/max_map_count", val) == 0;
}

static int get_current_vma_count(void)
{
	struct procmap_fd pmap;
	int count = 0;
	int ret;
	char vma_name[PATH_MAX];

	ret = open_self_procmap(&pmap);
	if (ret)
		return -1;

	pmap.query.query_addr = 0;
	pmap.query.query_flags = PROCMAP_QUERY_COVERING_OR_NEXT_VMA;

	while (true) {
		pmap.query.vma_name_addr = (uint64_t)(uintptr_t)vma_name;
		pmap.query.vma_name_size = sizeof(vma_name);
		vma_name[0] = '\0';

		ret = query_procmap(&pmap);
		if (ret != 0)
			break;

		/*
		 * The [vsyscall] mapping is a special mapping that
		 * doesn't count against the max_vma_count limit.
		 * Ignore it here to match the kernel's accounting.
		 */
		if (strcmp(vma_name, "[vsyscall]") != 0)
			count++;

		pmap.query.query_addr = pmap.query.vma_end;
	}

	close_procmap(&pmap);
	return count;
}

static void create_reservation(max_vma_count_data_t *self,
			       struct __test_metadata *_metadata)
{
	size_t reservation_size;
	void *base_addr = NULL;

	/*
	 * To break the dependency on knowing the exact number of extra maps
	 * before creating the reservation, we allocate a reservation size
	 * large enough for the maximum possible number of extra maps.
	 * The maximum number of extra maps is bounded by max_vma_count.
	 */
	reservation_size = ((self->max_vma_count * 2) +
				TEST_AREA_NR_PAGES +
				2 /* Holes around TEST_AREA */) * psize();

	base_addr = mmap(NULL, reservation_size, PROT_NONE,
			MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
	ASSERT_NE(base_addr, MAP_FAILED);

	ASSERT_EQ(munmap(base_addr, reservation_size), 0);

	/* The test area is offset by one hole page from the base address. */
	self->test_area = (char *)base_addr + psize();

	/* The extra maps start after the test area and another hole page. */
	self->extra_maps = self->test_area + self->test_area_size + psize();
}

static void create_extra_maps(max_vma_count_data_t *self,
			    struct __test_metadata *_metadata)
{
	char *ptr = self->extra_maps;

	for (int i = 0; i < self->nr_extra_maps; ++i) {
		ASSERT_NE(mmap(ptr, psize(), EXTRA_MAP_PROT,
			MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,
			-1, 0), MAP_FAILED) {
			TH_LOG("Failed on mapping #%d of %d", i + 1,
				self->nr_extra_maps);
		}

		/*
		 * Advance pointer by two pages to leave a 1-page hole,
		 * after each 1-page map.
		 */
		ptr += (2 * psize());
	}
}

static bool free_vma_slots(max_vma_count_data_t *self, int slots_to_free)
{
	for (int i = 0; i < slots_to_free; i++) {
		if (munmap(self->extra_maps + (i * 2 * psize()), psize()) != 0)
			return false;
	}

	return true;
}

static bool is_test_area_mapped(char *test_area, int test_area_size)
{
	struct procmap_fd pmap;
	bool found = false;
	int ret;

	ret = open_self_procmap(&pmap);
	if (ret)
		return false;

	pmap.query.query_addr = (uint64_t)(uintptr_t)test_area;
	pmap.query.query_flags = 0; /* Find VMA covering address */

	if (query_procmap(&pmap) == 0 &&
	    pmap.query.vma_start == (unsigned long)test_area &&
	    pmap.query.vma_end == (unsigned long)test_area + test_area_size)
		found = true;

	close_procmap(&pmap);
	return found;
}

