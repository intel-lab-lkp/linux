// SPDX-License-Identifier: GPL-2.0
/*
 * Tests for the sample memory provider (/dev/memory_provider).
 */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "test_util.h"
#include "../../../../samples/kvm/memory_provider.h"

#define DEV_PATH "/dev/memory_provider"

static int open_provider_dev(void)
{
	int fd = open(DEV_PATH, O_RDWR);

	if (fd < 0) {
		TEST_REQUIRE(errno != ENOENT && errno != ENODEV);
		TEST_FAIL("Failed to open %s: %d (%s)", DEV_PATH, errno,
			  strerror(errno));
	}
	return fd;
}

static int create_provider_file(int dev_fd, uint64_t size)
{
	struct mp_new_file nf = {
		.size = size,
	};
	int fd;

	fd = ioctl(dev_fd, MP_NEW_FILE, &nf);
	TEST_ASSERT(fd >= 0, "MP_NEW_FILE failed for size 0x%lx: %d (%s)",
		    size, errno, strerror(errno));
	return fd;
}

static void test_lifecycle_and_creation(int dev_fd)
{
	size_t page_size = getpagesize();
	uint64_t sizes[] = {
		page_size,
		2 * page_size,
		2 * 1024 * 1024,
	};
	struct mp_stats st;
	int i, fd;

	for (i = 0; i < ARRAY_SIZE(sizes); i++) {
		fd = create_provider_file(dev_fd, sizes[i]);

		memset(&st, 0, sizeof(st));
		TEST_ASSERT(!ioctl(fd, MP_GET_STATS, &st),
			    "MP_GET_STATS failed: %d (%s)",
			    errno, strerror(errno));
		TEST_ASSERT(st.size == sizes[i],
			    "size mismatch: expected 0x%lx, got 0x%llx",
			    sizes[i], st.size);
		TEST_ASSERT(st.absent_pages == 0,
			    "absent_pages mismatch: expected 0, got %llu",
			    st.absent_pages);
		TEST_ASSERT(st.readonly_pages == 0,
			    "readonly_pages mismatch: expected 0, got %llu",
			    st.readonly_pages);

		close(fd);
	}
}

static void test_set_present(int dev_fd)
{
	size_t page_size = getpagesize();
	uint64_t nr_pages = 4;
	uint64_t file_size = nr_pages * page_size;
	struct mp_set_present sp;
	struct mp_stats st;
	int ret, fd;

	fd = create_provider_file(dev_fd, file_size);

	/* Initially all pages are present */
	TEST_ASSERT(!ioctl(fd, MP_GET_STATS, &st),
		    "MP_GET_STATS failed: %d (%s)", errno, strerror(errno));
	TEST_ASSERT(st.absent_pages == 0,
		    "Initial absent_pages mismatch: expected 0, got %llu",
		    st.absent_pages);

	/* Mark page 1 absent */
	memset(&sp, 0, sizeof(sp));
	sp.value = 0;
	sp.offset = page_size;
	sp.len = page_size;
	ret = ioctl(fd, MP_SET_PRESENT, &sp);
	TEST_ASSERT(!ret, "MP_SET_PRESENT (absent) failed: %d (%s)",
		    errno, strerror(errno));

	TEST_ASSERT(!ioctl(fd, MP_GET_STATS, &st),
		    "MP_GET_STATS failed: %d (%s)", errno, strerror(errno));
	TEST_ASSERT(st.size == file_size,
		    "size changed unexpectedly: 0x%llx", st.size);
	TEST_ASSERT(st.absent_pages == 1,
		    "absent_pages mismatch after absent: expected 1, got %llu",
		    st.absent_pages);

	/* Mark page 1 back to present */
	sp.value = 1;
	ret = ioctl(fd, MP_SET_PRESENT, &sp);
	TEST_ASSERT(!ret, "MP_SET_PRESENT (present) failed: %d (%s)",
		    errno, strerror(errno));

	TEST_ASSERT(!ioctl(fd, MP_GET_STATS, &st),
		    "MP_GET_STATS failed: %d (%s)", errno, strerror(errno));
	TEST_ASSERT(st.absent_pages == 0,
		    "absent_pages mismatch after restore: expected 0, got %llu",
		    st.absent_pages);

	/* Mark all pages absent */
	sp.value = 0;
	sp.offset = 0;
	sp.len = file_size;
	ret = ioctl(fd, MP_SET_PRESENT, &sp);
	TEST_ASSERT(!ret, "MP_SET_PRESENT (all absent) failed: %d (%s)",
		    errno, strerror(errno));

	TEST_ASSERT(!ioctl(fd, MP_GET_STATS, &st),
		    "MP_GET_STATS failed: %d (%s)", errno, strerror(errno));
	TEST_ASSERT(st.absent_pages == nr_pages,
		    "absent_pages mismatch after all absent: expected %lu, got %llu",
		    nr_pages, st.absent_pages);

	close(fd);
}

static void test_set_readonly(int dev_fd)
{
	size_t page_size = getpagesize();
	uint64_t nr_pages = 4;
	uint64_t file_size = nr_pages * page_size;
	struct mp_set_readonly sr;
	struct mp_stats st;
	int ret, fd;

	fd = create_provider_file(dev_fd, file_size);

	/* Initially all pages are writable */
	TEST_ASSERT(!ioctl(fd, MP_GET_STATS, &st),
		    "MP_GET_STATS failed: %d (%s)", errno, strerror(errno));
	TEST_ASSERT(st.readonly_pages == 0,
		    "Initial readonly_pages mismatch: expected 0, got %llu",
		    st.readonly_pages);

	/* Mark page 1 read-only */
	memset(&sr, 0, sizeof(sr));
	sr.value = 1;
	sr.offset = page_size;
	sr.len = page_size;
	ret = ioctl(fd, MP_SET_READONLY, &sr);
	TEST_ASSERT(!ret, "MP_SET_READONLY (read-only) failed: %d (%s)",
		    errno, strerror(errno));

	TEST_ASSERT(!ioctl(fd, MP_GET_STATS, &st),
		    "MP_GET_STATS failed: %d (%s)", errno, strerror(errno));
	TEST_ASSERT(st.size == file_size,
		    "size changed unexpectedly: 0x%llx", st.size);
	TEST_ASSERT(st.readonly_pages == 1,
		    "readonly_pages mismatch after read-only: expected 1, got %llu",
		    st.readonly_pages);

	/* Mark page 1 back to writable */
	sr.value = 0;
	ret = ioctl(fd, MP_SET_READONLY, &sr);
	TEST_ASSERT(!ret, "MP_SET_READONLY (writable) failed: %d (%s)",
		    errno, strerror(errno));

	TEST_ASSERT(!ioctl(fd, MP_GET_STATS, &st),
		    "MP_GET_STATS failed: %d (%s)", errno, strerror(errno));
	TEST_ASSERT(st.readonly_pages == 0,
		    "readonly_pages mismatch after writable: expected 0, got %llu",
		    st.readonly_pages);

	/* Mark all pages read-only */
	sr.value = 1;
	sr.offset = 0;
	sr.len = file_size;
	ret = ioctl(fd, MP_SET_READONLY, &sr);
	TEST_ASSERT(!ret, "MP_SET_READONLY (all read-only) failed: %d (%s)",
		    errno, strerror(errno));

	TEST_ASSERT(!ioctl(fd, MP_GET_STATS, &st),
		    "MP_GET_STATS failed: %d (%s)", errno, strerror(errno));
	TEST_ASSERT(st.readonly_pages == nr_pages,
		    "readonly_pages mismatch after all read-only: expected %lu, got %llu",
		    nr_pages, st.readonly_pages);

	close(fd);
}

int main(int argc, char *argv[])
{
	int dev_fd;

	dev_fd = open_provider_dev();

	test_lifecycle_and_creation(dev_fd);
	test_set_present(dev_fd);
	test_set_readonly(dev_fd);

	close(dev_fd);
	return 0;
}
