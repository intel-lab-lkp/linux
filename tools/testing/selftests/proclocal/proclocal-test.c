// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (C) 2024 Amazon.com, Inc. or its affiliates. All rights reserved.
 * Author: Roman Kagan <rkagan@amazon.de>
 *
 * test for proclocal memory allocator using the corresponding test device
 */

#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include "../kselftest_harness.h"

struct proclocal_test_alloc {
	uint64_t size;
	uint64_t ptr;
};

#define PROCLOCAL_TEST_ALLOC _IOWR('A', 0x10, struct proclocal_test_alloc)

const char proclocal_content[] = "this is test";
char buf[256];

FIXTURE(proclocal) {
	int fd;
	void *ptr;
};

FIXTURE_SETUP(proclocal)
{
	struct proclocal_test_alloc pta = {
		.size = sizeof(buf),
	};

	self->fd = open("/dev/proclocal-test", O_RDWR);
	ASSERT_LE(0, self->fd);

	ASSERT_LE(0, ioctl(self->fd, PROCLOCAL_TEST_ALLOC, &pta));

	self->ptr = (void *) pta.ptr;
	TH_LOG("self->ptr = %p\n", self->ptr);
}

FIXTURE_TEARDOWN(proclocal)
{
}

TEST_F(proclocal, kernel_access)
{
	ASSERT_EQ((off_t)self->ptr,
		  lseek(self->fd, (off_t)self->ptr, SEEK_SET));
	EXPECT_EQ(sizeof(proclocal_content),
		  write(self->fd,
			proclocal_content, sizeof(proclocal_content)));
	ASSERT_EQ((off_t)self->ptr,
		  lseek(self->fd, (off_t)self->ptr, SEEK_SET));
	EXPECT_EQ(sizeof(proclocal_content),
		  read(self->fd, buf, sizeof(proclocal_content)));
	EXPECT_STREQ(proclocal_content, buf);
}

sigjmp_buf jmpbuf;
void segv_handler(int signum, siginfo_t *si, void *uc)
{
	if (signum == SIGSEGV)
		siglongjmp(jmpbuf, 1);
}

TEST_F(proclocal, direct_access)
{
	bool access_succeeded;
	struct sigaction sa;

	if (sigsetjmp(jmpbuf, 1) == 0) {
		sa.sa_sigaction = segv_handler;
		sa.sa_flags = SA_SIGINFO | SA_RESETHAND;
		sigemptyset(&sa.sa_mask);

		sigaction(SIGSEGV, &sa, NULL);

		(void)((volatile char *)self->ptr)[0];

		access_succeeded = true;
	} else
		access_succeeded = false;

	EXPECT_FALSE(access_succeeded);
}

#define PAGE_SIZE 0x1000

TEST_F(proclocal, map_over)
{
	void *ptr_page = (void *)((uintptr_t)self->ptr & ~(PAGE_SIZE - 1));
	void *map;
	int errno_save;

	errno = 0;
	map = mmap(ptr_page, PAGE_SIZE, PROT_NONE,
		   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
	errno_save = errno;

	EXPECT_EQ(MAP_FAILED, map);
	TH_LOG("errno = %d", errno_save);

	if (map != MAP_FAILED)
		munmap(map, PAGE_SIZE);
}

TEST_F(proclocal, release)
{
	EXPECT_EQ(0, close(self->fd));
}

TEST_F(proclocal, map_over_closed)
{
	void *ptr_page = (void *)((uintptr_t)self->ptr & ~(PAGE_SIZE - 1));
	void *map;
	int errno_save;

	ASSERT_EQ(0, close(self->fd));

	errno = 0;
	map = mmap(ptr_page, PAGE_SIZE, PROT_NONE,
		   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
	errno_save = errno;

	EXPECT_EQ(ptr_page, map);
	TH_LOG("errno = %d", errno_save);

	if (map != MAP_FAILED)
		munmap(map, PAGE_SIZE);
}

TEST_F(proclocal, kernel_access_closed)
{
	ASSERT_EQ(0, close(self->fd));
	self->fd = open("/dev/proclocal-test", O_RDWR);
	ASSERT_LE(0, self->fd);

	ASSERT_EQ((off_t)self->ptr,
		  lseek(self->fd, (off_t)self->ptr, SEEK_SET));
	EXPECT_EQ(-1, read(self->fd, buf, sizeof(proclocal_content)));
}

TEST_HARNESS_MAIN
