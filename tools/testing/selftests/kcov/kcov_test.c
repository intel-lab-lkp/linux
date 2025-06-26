// SPDX-License-Identifier: GPL-2.0
/*
 * Test the kernel coverage (/sys/kernel/debug/kcov).
 *
 * Copyright 2025 Google LLC.
 */
#include <fcntl.h>
#include <linux/kcov.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>

#include "../kselftest_harness.h"

/* Normally these defines should be provided by linux/kcov.h, but they aren't there yet. */
#define KCOV_UNIQUE_ENABLE _IOW('c', 103, unsigned long)
#define KCOV_RESET_TRACE _IO('c', 104)

#define COVER_SIZE (64 << 10)
#define BITMAP_SIZE (4 << 10)

#define DEBUG_COVER_PCS 0

FIXTURE(kcov)
{
	int fd;
	unsigned long *mapping;
	size_t mapping_size;
};

FIXTURE_VARIANT(kcov)
{
	int mode;
	bool fast_reset;
	bool map_readonly;
};

/* clang-format off */
FIXTURE_VARIANT_ADD(kcov, mode_trace_pc)
{
	/* clang-format on */
	.mode = KCOV_TRACE_PC,
	.fast_reset = true,
	.map_readonly = false,
};

/* clang-format off */
FIXTURE_VARIANT_ADD(kcov, mode_trace_cmp)
{
	/* clang-format on */
	.mode = KCOV_TRACE_CMP,
	.fast_reset = true,
	.map_readonly = false,
};

/* clang-format off */
FIXTURE_VARIANT_ADD(kcov, reset_ioctl_rw)
{
	/* clang-format on */
	.mode = KCOV_TRACE_PC,
	.fast_reset = false,
	.map_readonly = false,
};

FIXTURE_VARIANT_ADD(kcov, reset_ioctl_ro)
/* clang-format off */
{
	/* clang-format on */
	.mode = KCOV_TRACE_PC,
	.fast_reset = false,
	.map_readonly = true,
};

int kcov_open_init(struct __test_metadata *_metadata, unsigned long size,
		   int prot, unsigned long **out_mapping)
{
	unsigned long *mapping;

	/* A single fd descriptor allows coverage collection on a single thread. */
	int fd = open("/sys/kernel/debug/kcov", O_RDWR);

	ASSERT_NE(fd, -1)
	{
		perror("open");
	}

	EXPECT_EQ(ioctl(fd, KCOV_INIT_TRACE, COVER_SIZE), 0)
	{
		perror("ioctl KCOV_INIT_TRACE");
		close(fd);
	}

	/* Mmap buffer shared between kernel- and user-space. */
	mapping = (unsigned long *)mmap(NULL, size * sizeof(unsigned long),
					prot, MAP_SHARED, fd, 0);
	ASSERT_NE((void *)mapping, MAP_FAILED)
	{
		perror("mmap");
		close(fd);
	}
	*out_mapping = mapping;

	return fd;
}

FIXTURE_SETUP(kcov)
{
	int prot = variant->map_readonly ? PROT_READ : (PROT_READ | PROT_WRITE);

	/* Read-only mapping is incompatible with fast reset. */
	ASSERT_FALSE(variant->map_readonly && variant->fast_reset);

	self->mapping_size = COVER_SIZE;
	self->fd = kcov_open_init(_metadata, self->mapping_size, prot,
				  &(self->mapping));

	/* Enable coverage collection on the current thread. */
	EXPECT_EQ(ioctl(self->fd, KCOV_ENABLE, variant->mode), 0)
	{
		perror("ioctl KCOV_ENABLE");
		munmap(self->mapping, COVER_SIZE * sizeof(unsigned long));
		close(self->fd);
	}
}

void kcov_uninit_close(struct __test_metadata *_metadata, int fd,
		       unsigned long *mapping, size_t size)
{
	/* Disable coverage collection for the current thread. */
	EXPECT_EQ(ioctl(fd, KCOV_DISABLE, 0), 0)
	{
		perror("ioctl KCOV_DISABLE");
	}

	/* Free resources. */
	EXPECT_EQ(munmap(mapping, size * sizeof(unsigned long)), 0)
	{
		perror("munmap");
	}

	EXPECT_EQ(close(fd), 0)
	{
		perror("close");
	}
}

FIXTURE_TEARDOWN(kcov)
{
	kcov_uninit_close(_metadata, self->fd, self->mapping,
			  self->mapping_size);
}

void dump_collected_pcs(struct __test_metadata *_metadata, unsigned long *cover,
			size_t start, size_t end)
{
	int i = 0;

	TH_LOG("Collected %lu PCs", end - start);
#if DEBUG_COVER_PCS
	for (i = start; i < end; i++)
		TH_LOG("0x%lx", cover[i + 1]);
#endif
}

/* Coverage collection helper without assertions. */
unsigned long collect_coverage_unchecked(struct __test_metadata *_metadata,
					 unsigned long *cover, bool dump)
{
	unsigned long before, after;

	before = __atomic_load_n(&cover[0], __ATOMIC_RELAXED);
	/*
	 * Call the target syscall call. Here we use read(-1, NULL, 0) as an example.
	 * This will likely return an error (-EFAULT or -EBADF), but the goal is to
	 * collect coverage for the syscall's entry/exit paths.
	 */
	read(-1, NULL, 0);

	after = __atomic_load_n(&cover[0], __ATOMIC_RELAXED);

	if (dump)
		dump_collected_pcs(_metadata, cover, before, after);
	return after - before;
}

unsigned long collect_coverage_once(struct __test_metadata *_metadata,
				    unsigned long *cover)
{
	unsigned long collected =
		collect_coverage_unchecked(_metadata, cover, /*dump*/ true);

	/* Coverage must be non-zero. */
	EXPECT_GT(collected, 0);
	return collected;
}

void reset_coverage(struct __test_metadata *_metadata, bool fast, int fd,
		    unsigned long *mapping)
{
	unsigned long count;

	if (fast) {
		__atomic_store_n(&mapping[0], 0, __ATOMIC_RELAXED);
	} else {
		EXPECT_EQ(ioctl(fd, KCOV_RESET_TRACE, 0), 0)
		{
			perror("ioctl KCOV_RESET_TRACE");
		}
		count = __atomic_load_n(&mapping[0], __ATOMIC_RELAXED);
		EXPECT_NE(count, 0);
	}
}

TEST_F(kcov, kcov_basic_syscall_coverage)
{
	unsigned long first, second, before, after, i;

	/* Reset coverage that may be left over from the fixture setup. */
	reset_coverage(_metadata, variant->fast_reset, self->fd, self->mapping);

	/* Collect the coverage for a single syscall two times in a row. */
	first = collect_coverage_once(_metadata, self->mapping);
	second = collect_coverage_once(_metadata, self->mapping);
	/* Collected coverage should not differ too much. */
	EXPECT_GT(first * 10, second);
	EXPECT_GT(second * 10, first);

	/* Now reset the buffer and collect the coverage again. */
	reset_coverage(_metadata, variant->fast_reset, self->fd, self->mapping);
	collect_coverage_once(_metadata, self->mapping);

	/* Now try many times to fill up the buffer. */
	reset_coverage(_metadata, variant->fast_reset, self->fd, self->mapping);
	while (collect_coverage_unchecked(_metadata, self->mapping,
					  /*dump*/ false)) {
		/* Do nothing. */
	}
	before = __atomic_load_n(&(self->mapping[0]), __ATOMIC_RELAXED);
	/*
	 * Resetting with ioctl may still generate some coverage, but much less
	 * than there was before.
	 */
	reset_coverage(_metadata, variant->fast_reset, self->fd, self->mapping);
	after = __atomic_load_n(&(self->mapping[0]), __ATOMIC_RELAXED);
	EXPECT_GT(before, after);
	/* Collecting coverage after reset will now succeed. */
	collect_coverage_once(_metadata, self->mapping);
}

FIXTURE(kcov_uniq)
{
	int fd;
	unsigned long *mapping;
	size_t mapping_size;
	unsigned long *bitmap;
	size_t bitmap_size;
	unsigned long *cover;
	size_t cover_size;
};

FIXTURE_VARIANT(kcov_uniq)
{
	bool fast_reset;
	bool map_readonly;
};

/* clang-format off */
FIXTURE_VARIANT_ADD(kcov_uniq, fast_rw)
{
	/* clang-format on */
	.fast_reset = true,
	.map_readonly = false,
};

/* clang-format off */
FIXTURE_VARIANT_ADD(kcov_uniq, slow_rw)
{
	/* clang-format on */
	.fast_reset = false,
	.map_readonly = false,
};

/* clang-format off */
FIXTURE_VARIANT_ADD(kcov_uniq, slow_ro)
{
	/* clang-format on */
	.fast_reset = false,
	.map_readonly = true,
};

FIXTURE_SETUP(kcov_uniq)
{
	int prot = variant->map_readonly ? PROT_READ : (PROT_READ | PROT_WRITE);

	/* Read-only mapping is incompatible with fast reset. */
	ASSERT_FALSE(variant->map_readonly && variant->fast_reset);

	self->mapping_size = COVER_SIZE;
	self->fd = kcov_open_init(_metadata, self->mapping_size, prot,
				  &(self->mapping));

	/* Enable coverage collection on the current thread. */
	EXPECT_EQ(ioctl(self->fd, KCOV_UNIQUE_ENABLE, BITMAP_SIZE), 0)
	{
		perror("ioctl KCOV_ENABLE");
		munmap(self->mapping, COVER_SIZE * sizeof(unsigned long));
		close(self->fd);
	}
}

FIXTURE_TEARDOWN(kcov_uniq)
{
	kcov_uninit_close(_metadata, self->fd, self->mapping,
			  self->mapping_size);
}

TEST_F(kcov_uniq, kcov_uniq_coverage)
{
	unsigned long first, second, before, after, i;

	/* Reset coverage that may be left over from the fixture setup. */
	reset_coverage(_metadata, variant->fast_reset, self->fd, self->mapping);

	/*
	 * Collect the coverage for a single syscall two times in a row.
	 * Use collect_coverage_unchecked(), because it may return zero coverage.
	 */
	first = collect_coverage_unchecked(_metadata, self->mapping,
					   /*dump*/ true);
	second = collect_coverage_unchecked(_metadata, self->mapping,
					    /*dump*/ true);

	/* Now reset the buffer and collect the coverage again. */
	reset_coverage(_metadata, variant->fast_reset, self->fd, self->mapping);
	collect_coverage_once(_metadata, self->mapping);

	/* Now try many times to saturate the unique coverage bitmap. */
	reset_coverage(_metadata, variant->fast_reset, self->fd, self->mapping);
	for (i = 0; i < 1000; i++)
		collect_coverage_unchecked(_metadata, self->mapping,
					   /*dump*/ false);
	/* Another invocation of collect_coverage_unchecked() should not produce new coverage. */
	EXPECT_EQ(collect_coverage_unchecked(_metadata, self->mapping,
					     /*dump*/ false),
		  0);

	before = __atomic_load_n(&(self->mapping[0]), __ATOMIC_RELAXED);
	/*
	 * Resetting with ioctl may still generate some coverage, but much less
	 * than there was before.
	 */
	reset_coverage(_metadata, variant->fast_reset, self->fd, self->mapping);
	after = __atomic_load_n(&(self->mapping[0]), __ATOMIC_RELAXED);
	EXPECT_GT(before, after);
	/* Collecting coverage after reset will now succeed. */
	collect_coverage_once(_metadata, self->mapping);
}

TEST_HARNESS_MAIN
