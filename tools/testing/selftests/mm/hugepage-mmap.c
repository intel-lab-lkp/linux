// SPDX-License-Identifier: GPL-2.0
/*
 * hugepage-mmap:
 *
 * Example of using huge page memory in a user application using the mmap
 * system call.  Before running this application, make sure that the
 * administrator has mounted the hugetlbfs filesystem (on some directory
 * like /mnt) using the command mount -t hugetlbfs nodev /mnt. In this
 * example, the app is requesting memory of size 256MB that is backed by
 * huge pages.
 */
#define _GNU_SOURCE
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/mman.h>
#include <fcntl.h>
#include "kselftest_harness.h"

#define LENGTH (256UL*1024*1024)
#define PROTECTION (PROT_READ | PROT_WRITE)

FIXTURE(hugepage_mmap)
{
	int fd;
	char *addr;
};

FIXTURE_SETUP(hugepage_mmap)
{
	self->fd = memfd_create("hugepage-mmap", MFD_HUGETLB);
	ASSERT_GE(self->fd, 0) {
		TH_LOG("memfd_create() failed: %s", strerror(errno));
	}

	self->addr = mmap(NULL, LENGTH, PROTECTION, MAP_SHARED, self->fd, 0);
	ASSERT_NE(MAP_FAILED, self->addr) {
		TH_LOG("mmap(): %s", strerror(errno));
		close(self->fd);
	}
	TH_LOG("Returned address is %p", self->addr);
}

FIXTURE_TEARDOWN(hugepage_mmap)
{
	munmap(self->addr, LENGTH);
	close(self->fd);
}

TEST_F(hugepage_mmap, read_write)
{
	unsigned long i;

	TH_LOG("First hex is %x", *((unsigned int *)self->addr));

	for (i = 0; i < LENGTH; i++)
		self->addr[i] = (char)i;

	TH_LOG("First hex is %x", *((unsigned int *)self->addr));

	for (i = 0; i < LENGTH; i++) {
		ASSERT_EQ(self->addr[i], (char)i) {
			TH_LOG("Error: Mismatch at %lu\n", i);
		};
	}
}

TEST_HARNESS_MAIN
