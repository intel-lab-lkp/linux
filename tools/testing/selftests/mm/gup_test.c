#define __SANE_USERSPACE_TYPES__ // Use ll64
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <mm/gup_test.h>
#include <mm/hugepage_settings.h>

#include "kselftest.h"
#include "vm_util.h"
#include "kselftest_harness.h"

#define MB (1UL << 20)

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))
#endif

/* Just the flags we need, copied from the kernel internals. */
#define FOLL_WRITE	0x01	/* check pte is writable */

/* Page counts exercising single, THP-batch, partial, and full-mapping GUP. */
static const int nr_pages_list[] = { 1, 512, 123, -1 };

#define GUP_TEST_FILE "/sys/kernel/debug/gup_test"

FIXTURE(gup_test)
{
	int gup_fd;
	char *addr;
	unsigned long size;
};

FIXTURE_VARIANT(gup_test)
{
	bool thp;
	bool hugetlb;
	bool write;
	bool shared;
};

FIXTURE_VARIANT_ADD(gup_test, private_write)
{
	.thp = false,
	.hugetlb = false,
	.write = true,
	.shared = false,
};

FIXTURE_VARIANT_ADD(gup_test, private_readonly)
{
	.thp = false,
	.hugetlb = false,
	.write = false,
	.shared = false,
};

FIXTURE_VARIANT_ADD(gup_test, private_write_thp)
{
	.thp = true,
	.hugetlb = false,
	.write = true,
	.shared = false,
};

FIXTURE_VARIANT_ADD(gup_test, private_readonly_thp)
{
	.thp = true,
	.hugetlb = false,
	.write = false,
	.shared = false,
};

FIXTURE_VARIANT_ADD(gup_test, private_write_hugetlb)
{
	.thp = false,
	.hugetlb = true,
	.write = true,
	.shared = false,
};

FIXTURE_VARIANT_ADD(gup_test, private_readonly_hugetlb)
{
	.thp = false,
	.hugetlb = true,
	.write = false,
	.shared = false,
};

FIXTURE_VARIANT_ADD(gup_test, shared_write)
{
	.thp = false,
	.hugetlb = false,
	.write = true,
	.shared = true,
};

FIXTURE_VARIANT_ADD(gup_test, shared_readonly)
{
	.thp = false,
	.hugetlb = false,
	.write = false,
	.shared = true,
};

FIXTURE_VARIANT_ADD(gup_test, shared_write_thp)
{
	.thp = true,
	.hugetlb = false,
	.write = true,
	.shared = true,
};

FIXTURE_VARIANT_ADD(gup_test, shared_readonly_thp)
{
	.thp = true,
	.hugetlb = false,
	.write = false,
	.shared = true,
};

FIXTURE_VARIANT_ADD(gup_test, shared_write_hugetlb)
{
	.thp = false,
	.hugetlb = true,
	.write = true,
	.shared = true,
};

FIXTURE_VARIANT_ADD(gup_test, shared_readonly_hugetlb)
{
	.thp = false,
	.hugetlb = true,
	.write = false,
	.shared = true,
};

FIXTURE_SETUP(gup_test)
{
	int mmap_flags = MAP_PRIVATE;
	int zero_fd;
	char *p;

	self->size = variant->hugetlb ? 256 * MB : 128 * MB;

	/* Check for hugetlb */
	if (variant->hugetlb) {
		unsigned long hp_size = default_huge_page_size();

		if (!hp_size)
			SKIP(return, "HugeTLB not available\n");

		self->size = (self->size + hp_size - 1) & ~(hp_size - 1);
		if (!hugetlb_setup_default(self->size / hp_size)) {
			hugetlb_restore_settings();
			SKIP(return, "Not enough huge pages\n");
		}

		mmap_flags |= (MAP_HUGETLB | MAP_ANONYMOUS);
	}

	/* zero_fd has to be >= 0. Already checked in main() */
	zero_fd = open("/dev/zero", O_RDWR);
	ASSERT_GE(zero_fd, 0);

	/* gup_fd has to be >= 0. Already checked in main() */
	self->gup_fd = open(GUP_TEST_FILE, O_RDWR);
	ASSERT_GE(self->gup_fd, 0);

	if (variant->shared)
		mmap_flags = (mmap_flags & ~MAP_PRIVATE) | MAP_SHARED;

	self->addr = mmap(NULL, self->size, PROT_READ | PROT_WRITE,
					mmap_flags, zero_fd, 0);
	close(zero_fd);
	ASSERT_NE(self->addr, MAP_FAILED);

	if (variant->thp)
		madvise(self->addr, self->size, MADV_HUGEPAGE);
	else
		madvise(self->addr, self->size, MADV_NOHUGEPAGE);

	for (p = self->addr; (unsigned long)p < (unsigned long)self->addr
			+ self->size; p += psize())
		p[0] = 0;
}

FIXTURE_TEARDOWN(gup_test)
{
	munmap(self->addr, self->size);
	close(self->gup_fd);

	if (variant->hugetlb)
		hugetlb_restore_settings();
}

TEST_F(gup_test, get_user_pages)
{
	/* Tests the get_user_pages path */
	int i;

	for (i = 0; i < (int)ARRAY_SIZE(nr_pages_list); i++) {
		struct gup_test gup = { 0 };

		gup.addr = (unsigned long)self->addr;
		gup.size = self->size;
		gup.nr_pages_per_call = nr_pages_list[i] < 0 ?
			self->size / psize() : nr_pages_list[i];

		if (variant->write)
			gup.gup_flags |= FOLL_WRITE;

		TH_LOG("nr_pages_per_call=%u", gup.nr_pages_per_call);
		ASSERT_EQ(ioctl(self->gup_fd, GUP_BASIC_TEST, &gup), 0);
	}
}

TEST_F(gup_test, pin_user_pages)
{
	/* Tests the pin_user_pages path */
	int i;

	for (i = 0; i < (int)ARRAY_SIZE(nr_pages_list); i++) {
		struct gup_test gup = { 0 };

		gup.addr = (unsigned long)self->addr;
		gup.size = self->size;
		gup.nr_pages_per_call = nr_pages_list[i] < 0 ?
			self->size / psize() : nr_pages_list[i];

		if (variant->write)
			gup.gup_flags |= FOLL_WRITE;

		TH_LOG("nr_pages_per_call=%u", gup.nr_pages_per_call);
		ASSERT_EQ(ioctl(self->gup_fd, PIN_BASIC_TEST, &gup), 0);
	}
}

TEST_F(gup_test, dump_user_pages_with_get)
{
	/* Tests DUMP_USER_PAGES_TEST using get_user_pages */
	int i;

	for (i = 0; i < (int)ARRAY_SIZE(nr_pages_list); i++) {
		struct gup_test gup = { 0 };

		gup.addr = (unsigned long)self->addr;
		gup.size = self->size;
		gup.nr_pages_per_call = nr_pages_list[i] < 0 ?
			self->size / psize() : nr_pages_list[i];

		if (variant->write)
			gup.gup_flags |= FOLL_WRITE;

		gup.which_pages[0] = 1;

		TH_LOG("nr_pages_per_call=%u", gup.nr_pages_per_call);
		ASSERT_EQ(ioctl(self->gup_fd, DUMP_USER_PAGES_TEST, &gup), 0);
	}
}

TEST_F(gup_test, dump_user_pages_with_pin)
{
	/* Tests DUMP_USER_PAGES_TEST using pin_user_pages */
	int i;

	for (i = 0; i < (int)ARRAY_SIZE(nr_pages_list); i++) {
		struct gup_test gup = { 0 };

		gup.addr = (unsigned long)self->addr;
		gup.size = self->size;
		gup.nr_pages_per_call = nr_pages_list[i] < 0 ?
			self->size / psize() : nr_pages_list[i];

		if (variant->write)
			gup.gup_flags |= FOLL_WRITE;

		gup.which_pages[0] = 1;
		gup.test_flags |= GUP_TEST_FLAG_DUMP_PAGES_USE_PIN;

		TH_LOG("nr_pages_per_call=%u", gup.nr_pages_per_call);
		ASSERT_EQ(ioctl(self->gup_fd, DUMP_USER_PAGES_TEST, &gup), 0);
	}
}

TEST_F(gup_test, get_user_pages_fast)
{
	/* Tests the lockless get_user_pages_fast() path */
	int i;

	for (i = 0; i < (int)ARRAY_SIZE(nr_pages_list); i++) {
		struct gup_test gup = { 0 };

		gup.addr = (unsigned long)self->addr;
		gup.size = self->size;
		gup.nr_pages_per_call = nr_pages_list[i] < 0 ?
			self->size / psize() : nr_pages_list[i];

		if (variant->write)
			gup.gup_flags |= FOLL_WRITE;

		TH_LOG("nr_pages_per_call=%u", gup.nr_pages_per_call);
		ASSERT_EQ(ioctl(self->gup_fd, GUP_FAST_BENCHMARK, &gup), 0);
	}
}

TEST_F(gup_test, pin_user_pages_fast)
{
	/* Tests the lockless pin_user_pages_fast() path */
	int i;

	for (i = 0; i < (int)ARRAY_SIZE(nr_pages_list); i++) {
		struct gup_test gup = { 0 };

		gup.addr = (unsigned long)self->addr;
		gup.size = self->size;
		gup.nr_pages_per_call = nr_pages_list[i] < 0 ?
			self->size / psize() : nr_pages_list[i];

		if (variant->write)
			gup.gup_flags |= FOLL_WRITE;

		TH_LOG("nr_pages_per_call=%u", gup.nr_pages_per_call);
		ASSERT_EQ(ioctl(self->gup_fd, PIN_FAST_BENCHMARK, &gup), 0);
	}
}

TEST_F(gup_test, pin_user_pages_longterm)
{
	/* Tests pin_user_pages() with FOLL_LONGTERM */
	int i;

	for (i = 0; i < (int)ARRAY_SIZE(nr_pages_list); i++) {
		struct gup_test gup = { 0 };

		gup.addr = (unsigned long)self->addr;
		gup.size = self->size;
		gup.nr_pages_per_call = nr_pages_list[i] < 0 ?
			self->size / psize() : nr_pages_list[i];

		if (variant->write)
			gup.gup_flags |= FOLL_WRITE;

		TH_LOG("nr_pages_per_call=%u", gup.nr_pages_per_call);
		ASSERT_EQ(ioctl(self->gup_fd, PIN_LONGTERM_BENCHMARK, &gup), 0);
	}
}

TEST(dump_user_pages_sparse_indices)
{
	/* Tests sparse multi-index which_pages[] inputs. */
	struct gup_test gup = { 0 };
	unsigned long size = 128 * MB;
	int zero_fd, gup_fd;
	char *addr, *p;

	zero_fd = open("/dev/zero", O_RDWR);
	ASSERT_GE(zero_fd, 0);

	gup_fd = open(GUP_TEST_FILE, O_RDWR);
	ASSERT_GE(gup_fd, 0);

	addr = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE, zero_fd, 0);
	close(zero_fd);
	ASSERT_NE(addr, MAP_FAILED);

	ASSERT_EQ(madvise(addr, size, MADV_HUGEPAGE), 0);

	for (p = addr; (unsigned long)p < (unsigned long)addr + size;
	     p += psize())
		p[0] = 0;

	gup.addr = (unsigned long)addr;
	gup.size = size;
	gup.gup_flags = FOLL_WRITE;
	gup.which_pages[0] = 1;
	gup.which_pages[1] = 20;
	gup.which_pages[2] = 0x1001;

	/*
	 * Preserve the old "./gup_test -ct -F 0x1 0 19 0x1000" sparse dump
	 * coverage after removing command-line parsing from this binary.
	 */
	ASSERT_EQ(ioctl(gup_fd, DUMP_USER_PAGES_TEST, &gup), 0);

	munmap(addr, size);
	close(gup_fd);
}

int main(int argc, char **argv)
{
	int fd;
	char *file = "/dev/zero";

	fd = open(file, O_RDWR);
	if (fd < 0) {
		ksft_print_header();
		ksft_exit_fail_msg("Unable to open %s: %s\n", file, strerror(errno));
	}
	close(fd);

	fd = open(GUP_TEST_FILE, O_RDWR);
	if (fd == -1) {
		ksft_print_header();
		if (errno == EACCES)
			ksft_exit_skip("Please run this test as root\n");
		if (errno == ENOENT) {
			if (opendir("/sys/kernel/debug") == NULL)
				ksft_exit_skip("Mount debugfs at /sys/kernel/debug\n");
			else
				ksft_exit_skip("Check CONFIG_GUP_TEST in kernel config\n");
		}
		ksft_exit_skip("failed to open %s: %s\n", GUP_TEST_FILE, strerror(errno));
	}
	close(fd);

	return test_harness_run(argc, argv);
}
