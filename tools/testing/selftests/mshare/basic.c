// SPDX-License-Identifier: GPL-2.0

#include "../kselftest_harness.h"
#include "util.c"

#define STRING "I am Msharefs"

FIXTURE(basic)
{
	char filename[128];
	size_t align_size;
	size_t allocate_size;
};

FIXTURE_VARIANT(basic) {
	/* decide the time of real mapping size besed on align_size */
	size_t map_size_time;
	/* flags for ioctl */
	int map_flags;
};

FIXTURE_VARIANT_ADD(basic, ANON_512G) {
	.map_size_time = 1,
	.map_flags = MAP_ANONYMOUS | MAP_SHARED | MAP_FIXED,
};

FIXTURE_VARIANT_ADD(basic, HUGETLB_512G) {
	.map_size_time = 1,
	.map_flags = MAP_ANONYMOUS | MAP_HUGETLB | MAP_SHARED | MAP_FIXED,
};

FIXTURE_VARIANT_ADD(basic, ANON_1T) {
	.map_size_time = 2,
	.map_flags = MAP_ANONYMOUS | MAP_SHARED | MAP_FIXED,
};

FIXTURE_VARIANT_ADD(basic, HUGETLB_1T) {
	.map_size_time = 2,
	.map_flags = MAP_ANONYMOUS | MAP_HUGETLB | MAP_SHARED | MAP_FIXED,
};

FIXTURE_SETUP(basic)
{
	int fd;

	self->align_size = mshare_get_info();
	self->allocate_size = self->align_size * variant->map_size_time;

	fd = create_mshare_file(self->filename, sizeof(self->filename));
	ftruncate(fd, self->allocate_size);

	ASSERT_EQ(mshare_ioctl_mapping(fd, self->allocate_size, variant->map_flags), 0);
	close(fd);
}

FIXTURE_TEARDOWN(basic)
{
	ASSERT_EQ(unlink(self->filename), 0);
}

TEST_F(basic, shared_mem)
{
	int fd;
	void *addr;
	pid_t pid = fork();

	ASSERT_NE(pid, -1);

	fd = open(self->filename, O_RDWR, 0600);
	ASSERT_NE(fd, -1);

	addr = mmap(NULL, self->allocate_size, PROT_READ | PROT_WRITE,
		       MAP_SHARED, fd, 0);
	ASSERT_NE(addr, MAP_FAILED);

	if (pid == 0) {
		/* Child process write date the shared memory */
		memcpy(addr, STRING, sizeof(STRING));
		exit(0);
	}

	ASSERT_NE(waitpid(pid, NULL, 0), -1);

	/* Parent process should retrieve the data from the shared memory */
	ASSERT_EQ(memcmp(addr, STRING, sizeof(STRING)), 0);
}

TEST_HARNESS_MAIN
