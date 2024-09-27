// SPDX-License-Identifier: GPL-2.0-or-later

#define _GNU_SOURCE
#include "../kselftest_harness.h"
#include <assert.h>
#include <fcntl.h>
#include <setjmp.h>
#include <errno.h>
#include <linux/userfaultfd.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <unistd.h>

/* These may not yet be available in the uAPI so define if not. */

#ifndef MADV_GUARD_POISON
#define MADV_GUARD_POISON	102
#endif

#ifndef MADV_GUARD_UNPOISON
#define MADV_GUARD_UNPOISON	103
#endif

volatile bool signal_jump_set;
sigjmp_buf signal_jmp_buf;

static int userfaultfd(int flags)
{
	return syscall(SYS_userfaultfd, flags);
}

static void handle_fatal(int c)
{
	if (!signal_jump_set)
		return;

	siglongjmp(signal_jmp_buf, c);
}

static int pidfd_open(pid_t pid, unsigned int flags)
{
	return syscall(SYS_pidfd_open, pid, flags);
}

/*
 * Enable our signal catcher and try to read/write the specified buffer. The
 * return value indicates whether the read/write succeeds without a fatal
 * signal.
 */
static bool try_access_buf(char *ptr, bool write)
{
	bool failed;

	/* Tell signal handler to jump back here on fatal signal. */
	signal_jump_set = true;
	/* If a fatal signal arose, we will jump back here and failed is set. */
	failed = sigsetjmp(signal_jmp_buf, 0) != 0;

	if (!failed) {
		if (write) {
			*ptr = 'x';
		} else {
			const volatile char *chr = ptr;

			/* Force read. */
			(void)*chr;
		}
	}

	signal_jump_set = false;
	return !failed;
}

/* Try and read from a buffer, return true if no fatal signal. */
static bool try_read_buf(char *ptr)
{
	return try_access_buf(ptr, false);
}

/* Try and write to a buffer, return true if no fatal signal. */
static bool try_write_buf(char *ptr)
{
	return try_access_buf(ptr, true);
}

/*
 * Try and BOTH read from AND write to a buffer, return true if BOTH operations
 * succeed.
 */
static bool try_read_write_buf(char *ptr)
{
	return try_read_buf(ptr) && try_write_buf(ptr);
}

FIXTURE(guard_pages)
{
	unsigned long page_size;
};

FIXTURE_SETUP(guard_pages)
{
	struct sigaction act = {
		.sa_handler = &handle_fatal,
		.sa_flags = SA_NODEFER,
	};

	sigemptyset(&act.sa_mask);
	if (sigaction(SIGSEGV, &act, NULL)) {
		perror("sigaction");
		ksft_exit_fail();
	}

	self->page_size = (unsigned long)sysconf(_SC_PAGESIZE);
};

FIXTURE_TEARDOWN(guard_pages)
{
	struct sigaction act = {
		.sa_handler = SIG_DFL,
		.sa_flags = SA_NODEFER,
	};

	sigemptyset(&act.sa_mask);
	sigaction(SIGSEGV, &act, NULL);
}

TEST_F(guard_pages, basic)
{
	const unsigned long NUM_PAGES = 10;
	const unsigned long page_size = self->page_size;
	char *ptr;
	int i;

	ptr = mmap(NULL, NUM_PAGES * page_size, PROT_READ | PROT_WRITE,
		   MAP_PRIVATE | MAP_ANON, -1, 0);
	ASSERT_NE(ptr, MAP_FAILED);

	/* Trivially assert we can touch the first page. */
	ASSERT_TRUE(try_read_write_buf(ptr));

	ASSERT_EQ(madvise(ptr, page_size, MADV_GUARD_POISON), 0);

	/* Establish that 1st page SIGSEGV's. */
	ASSERT_FALSE(try_read_write_buf(ptr));

	/* Ensure we can touch everything else.*/
	for (i = 1; i < NUM_PAGES; i++) {
		ASSERT_TRUE(try_read_write_buf(&ptr[i * page_size]));
	}

	/* Establish a guard page at the end of the mapping. */
	ASSERT_EQ(madvise(&ptr[(NUM_PAGES - 1) * page_size], page_size,
			  MADV_GUARD_POISON), 0);

	/* Check that both guard pages result in SIGSEGV. */
	ASSERT_FALSE(try_read_write_buf(ptr));
	ASSERT_FALSE(try_read_write_buf(&ptr[(NUM_PAGES - 1) * page_size]));

	/* Unpoison the first. */
	ASSERT_FALSE(madvise(ptr, page_size, MADV_GUARD_UNPOISON));

	/* Make sure we can touch it. */
	ASSERT_TRUE(try_read_write_buf(ptr));

	/* Unpoison the last. */
	ASSERT_FALSE(madvise(&ptr[(NUM_PAGES - 1) * page_size], page_size,
			     MADV_GUARD_UNPOISON));

	/* Make sure we can touch it. */
	ASSERT_TRUE(try_read_write_buf(&ptr[(NUM_PAGES - 1) * page_size]));

	/*
	 *  Test setting a _range_ of pages, namely the first 3. The first of
	 *  these be faulted in, so this also tests that we can poison backed
	 *  pages.
	 */
	ASSERT_EQ(madvise(ptr, 3 * page_size, MADV_GUARD_POISON), 0);

	/* Make sure they are all poisoned. */
	for (i = 0; i < 3; i++) {
		ASSERT_FALSE(try_read_write_buf(&ptr[i * page_size]));
	}

	/* Make sure the rest are not. */
	for (i = 3; i < NUM_PAGES; i++) {
		ASSERT_TRUE(try_read_write_buf(&ptr[i * page_size]));
	}

	/* Unpoison them. */
	ASSERT_EQ(madvise(ptr, NUM_PAGES * page_size, MADV_GUARD_UNPOISON), 0);

	/* Now make sure we can touch everything. */
	for (i = 0; i < NUM_PAGES; i++) {
		ASSERT_TRUE(try_read_write_buf(&ptr[i * page_size]));
	}

	/* Now unpoison everything, make sure we don't remove existing entries */
	ASSERT_EQ(madvise(ptr, NUM_PAGES * page_size, MADV_GUARD_UNPOISON), 0);

	for (i = 0; i < NUM_PAGES * page_size; i += page_size) {
		ASSERT_EQ(ptr[i], 'x');
	}

	ASSERT_EQ(munmap(ptr, NUM_PAGES * page_size), 0);
}

/* Assert that operations applied across multiple VMAs work as expected. */
TEST_F(guard_pages, multi_vma)
{
	const unsigned long page_size = self->page_size;
	char *ptr_region, *ptr, *ptr1, *ptr2, *ptr3;
	int i;

	/* Reserve a 100 page region over which we can install VMAs. */
	ptr_region = mmap(NULL, 100 * page_size, PROT_NONE,
			  MAP_ANON | MAP_PRIVATE, -1, 0);
	ASSERT_NE(ptr_region, MAP_FAILED);

	/* Place a VMA of 10 pages size at the start of the region. */
	ptr1 = mmap(ptr_region, 10 * page_size, PROT_READ | PROT_WRITE,
		    MAP_FIXED | MAP_ANON | MAP_PRIVATE, -1, 0);
	ASSERT_NE(ptr1, MAP_FAILED);

	/* Place a VMA of 5 pages size 50 pages into the region. */
	ptr2 = mmap(&ptr_region[50 * page_size], 5 * page_size,
		    PROT_READ | PROT_WRITE,
		    MAP_FIXED | MAP_ANON | MAP_PRIVATE, -1, 0);
	ASSERT_NE(ptr2, MAP_FAILED);

	/* Place a VMA of 20 pages size at the end of the region. */
	ptr3 = mmap(&ptr_region[80 * page_size], 20 * page_size,
		    PROT_READ | PROT_WRITE,
		    MAP_FIXED | MAP_ANON | MAP_PRIVATE, -1, 0);
	ASSERT_NE(ptr3, MAP_FAILED);

	/* Unmap gaps. */
	ASSERT_EQ(munmap(&ptr_region[10 * page_size], 40 * page_size), 0);
	ASSERT_EQ(munmap(&ptr_region[55 * page_size], 25 * page_size), 0);

	/*
	 * We end up with VMAs like this:
	 *
	 * 0    10 .. 50   55 .. 80   100
	 * [---]      [---]      [---]
	 */

	/* Now poison the whole range and make sure all VMAs are poisoned. */

	/*
	 * madvise() is certifiable and lets you perform operations over gaps,
	 * everything works, but it indicates an error and errno is set to
	 * -ENOMEM. Also if anything runs out of memory it is set to
	 * -ENOMEM. You are meant to guess which is which.
	 */
	ASSERT_EQ(madvise(ptr_region, 100 * page_size, MADV_GUARD_POISON), -1);
	ASSERT_EQ(errno, ENOMEM);

	for (i = 0; i < 10; i++) {
		ASSERT_FALSE(try_read_write_buf(&ptr1[i * page_size]));
	}

	for (i = 0; i < 5; i++) {
		ASSERT_FALSE(try_read_write_buf(&ptr2[i * page_size]));
	}

	for (i = 0; i < 20; i++) {
		ASSERT_FALSE(try_read_write_buf(&ptr3[i * page_size]));
	}

	/* Now unpoison the range and assert the opposite. */

	ASSERT_EQ(madvise(ptr_region, 100 * page_size, MADV_GUARD_UNPOISON), -1);
	ASSERT_EQ(errno, ENOMEM);

	for (i = 0; i < 10; i++) {
		ASSERT_TRUE(try_read_write_buf(&ptr1[i * page_size]));
	}

	for (i = 0; i < 5; i++) {
		ASSERT_TRUE(try_read_write_buf(&ptr2[i * page_size]));
	}

	for (i = 0; i < 20; i++) {
		ASSERT_TRUE(try_read_write_buf(&ptr3[i * page_size]));
	}

	/* Now map incompatible VMAs in the gaps. */
	ptr = mmap(&ptr_region[10 * page_size], 40 * page_size,
		   PROT_READ | PROT_WRITE | PROT_EXEC,
		   MAP_FIXED | MAP_ANON | MAP_PRIVATE, -1, 0);
	ASSERT_NE(ptr, MAP_FAILED);
	ptr = mmap(&ptr_region[55 * page_size], 25 * page_size,
		   PROT_READ | PROT_WRITE | PROT_EXEC,
		   MAP_FIXED | MAP_ANON | MAP_PRIVATE, -1, 0);
	ASSERT_NE(ptr, MAP_FAILED);

	/*
	 * We end up with VMAs like this:
	 *
	 * 0    10 .. 50   55 .. 80   100
	 * [---][xxxx][---][xxxx][---]
	 *
	 * Where 'x' signifies VMAs that cannot be merged with those adjacent to
	 * them.
	 */

	/* Multiple VMAs adjacent to one another should result in no error. */
	ASSERT_EQ(madvise(ptr_region, 100 * page_size, MADV_GUARD_POISON), 0);
	for (i = 0; i < 100; i++) {
		ASSERT_FALSE(try_read_write_buf(&ptr_region[i * page_size]));
	}
	ASSERT_EQ(madvise(ptr_region, 100 * page_size, MADV_GUARD_UNPOISON), 0);
	for (i = 0; i < 100; i++) {
		ASSERT_TRUE(try_read_write_buf(&ptr_region[i * page_size]));
	}

	/* Cleanup. */
	ASSERT_EQ(munmap(ptr_region, 100 * page_size), 0);
}

/*
 * Assert that batched operations performed using process_madvise() work as
 * expected.
 */
TEST_F(guard_pages, process_madvise)
{
	const unsigned long page_size = self->page_size;
	pid_t pid = getpid();
	int pidfd = pidfd_open(pid, 0);
	char *ptr_region, *ptr1, *ptr2, *ptr3;
	ssize_t count;
	struct iovec vec[6];

	ASSERT_NE(pidfd, -1);

	/* Reserve region to map over. */
	ptr_region = mmap(NULL, 100 * page_size, PROT_NONE,
			  MAP_ANON | MAP_PRIVATE, -1, 0);
	ASSERT_NE(ptr_region, MAP_FAILED);

	/* 10 pages offset 1 page into reserve region. */
	ptr1 = mmap(&ptr_region[page_size], 10 * page_size,
		    PROT_READ | PROT_WRITE,
		    MAP_FIXED | MAP_ANON | MAP_PRIVATE, -1, 0);
	ASSERT_NE(ptr1, MAP_FAILED);
	/* We want poison markers at start/end of each VMA. */
	vec[0].iov_base = ptr1;
	vec[0].iov_len = page_size;
	vec[1].iov_base = &ptr1[9 * page_size];
	vec[1].iov_len = page_size;

	/* 5 pages offset 50 pages into reserve region. */
	ptr2 = mmap(&ptr_region[50 * page_size], 5 * page_size,
		    PROT_READ | PROT_WRITE,
		    MAP_FIXED | MAP_ANON | MAP_PRIVATE, -1, 0);
	ASSERT_NE(ptr2, MAP_FAILED);
	vec[2].iov_base = ptr2;
	vec[2].iov_len = page_size;
	vec[3].iov_base = &ptr2[4 * page_size];
	vec[3].iov_len = page_size;

	/* 20 pages offset 79 pages into reserve region. */
	ptr3 = mmap(&ptr_region[79 * page_size], 20 * page_size,
		    PROT_READ | PROT_WRITE,
		    MAP_FIXED | MAP_ANON | MAP_PRIVATE, -1, 0);
	ASSERT_NE(ptr3, MAP_FAILED);
	vec[4].iov_base = ptr3;
	vec[4].iov_len = page_size;
	vec[5].iov_base = &ptr3[19 * page_size];
	vec[5].iov_len = page_size;

	/* Free surrounding VMAs. */
	ASSERT_EQ(munmap(ptr_region, page_size), 0);
	ASSERT_EQ(munmap(&ptr_region[11 * page_size], 39 * page_size), 0);
	ASSERT_EQ(munmap(&ptr_region[55 * page_size], 24 * page_size), 0);
	ASSERT_EQ(munmap(&ptr_region[99 * page_size], page_size), 0);

	/* Now poison in one step. */
	count = process_madvise(pidfd, vec, 6, MADV_GUARD_POISON, 0);

	/* OK we don't have permission to do this, skip. */
	if (count == -1 && errno == EPERM)
		ksft_exit_skip("No process_madvise() permissions\n");

	/* Returns the number of bytes advised. */
	ASSERT_EQ(count, 6 * page_size);

	/* Now make sure the poisoning was applied. */

	ASSERT_FALSE(try_read_write_buf(ptr1));
	ASSERT_FALSE(try_read_write_buf(&ptr1[9 * page_size]));

	ASSERT_FALSE(try_read_write_buf(ptr2));
	ASSERT_FALSE(try_read_write_buf(&ptr2[4 * page_size]));

	ASSERT_FALSE(try_read_write_buf(ptr3));
	ASSERT_FALSE(try_read_write_buf(&ptr3[19 * page_size]));

	/* Now do the same with unpoison... */
	count = process_madvise(pidfd, vec, 6, MADV_GUARD_UNPOISON, 0);

	/* ...and everything should now succeed. */

	ASSERT_TRUE(try_read_write_buf(ptr1));
	ASSERT_TRUE(try_read_write_buf(&ptr1[9 * page_size]));

	ASSERT_TRUE(try_read_write_buf(ptr2));
	ASSERT_TRUE(try_read_write_buf(&ptr2[4 * page_size]));

	ASSERT_TRUE(try_read_write_buf(ptr3));
	ASSERT_TRUE(try_read_write_buf(&ptr3[19 * page_size]));

	/* Cleanup. */
	ASSERT_EQ(munmap(ptr1, 10 * page_size), 0);
	ASSERT_EQ(munmap(ptr2, 5 * page_size), 0);
	ASSERT_EQ(munmap(ptr3, 20 * page_size), 0);
	close(pidfd);
}

/* Assert that unmapping ranges does not leave poison behind. */
TEST_F(guard_pages, munmap)
{
	const unsigned long page_size = self->page_size;
	char *ptr, *ptr_new1, *ptr_new2;

	ptr = mmap(NULL, 10 * page_size, PROT_READ | PROT_WRITE,
		   MAP_ANON | MAP_PRIVATE, -1, 0);
	ASSERT_NE(ptr, MAP_FAILED);

	/* Poison first and last pages. */
	ASSERT_EQ(madvise(ptr, page_size, MADV_GUARD_POISON), 0);
	ASSERT_EQ(madvise(&ptr[9 * page_size], page_size, MADV_GUARD_POISON), 0);

	/* Assert that they are poisoned. */
	ASSERT_FALSE(try_read_write_buf(ptr));
	ASSERT_FALSE(try_read_write_buf(&ptr[9 * page_size]));

	/* Unmap them. */
	ASSERT_EQ(munmap(ptr, page_size), 0);
	ASSERT_EQ(munmap(&ptr[9 * page_size], page_size), 0);

	/* Map over them.*/
	ptr_new1 = mmap(ptr, page_size, PROT_READ | PROT_WRITE,
			MAP_FIXED | MAP_ANON | MAP_PRIVATE, -1, 0);
	ASSERT_NE(ptr_new1, MAP_FAILED);
	ptr_new2 = mmap(&ptr[9 * page_size], page_size, PROT_READ | PROT_WRITE,
			MAP_FIXED | MAP_ANON | MAP_PRIVATE, -1, 0);
	ASSERT_NE(ptr_new2, MAP_FAILED);

	/* Assert that they are now not poisoned. */
	ASSERT_TRUE(try_read_write_buf(ptr_new1));
	ASSERT_TRUE(try_read_write_buf(ptr_new2));

	/* Cleanup. */
	ASSERT_EQ(munmap(ptr, 10 * page_size), 0);
}

/* Assert that mprotect() operations have no bearing on guard poison markers. */
TEST_F(guard_pages, mprotect)
{
	const unsigned long page_size = self->page_size;
	char *ptr;
	int i;

	ptr = mmap(NULL, 10 * page_size, PROT_READ | PROT_WRITE,
		   MAP_ANON | MAP_PRIVATE, -1, 0);
	ASSERT_NE(ptr, MAP_FAILED);

	/* Poison the middle of the range. */
	ASSERT_EQ(madvise(&ptr[5 * page_size], 2 * page_size,
			  MADV_GUARD_POISON), 0);

	/* Assert that it is indeed poisoned. */
	ASSERT_FALSE(try_read_write_buf(&ptr[5 * page_size]));
	ASSERT_FALSE(try_read_write_buf(&ptr[6 * page_size]));

	/* Now make these pages read-only. */
	ASSERT_EQ(mprotect(&ptr[5 * page_size], 2 * page_size, PROT_READ), 0);

	/* Make sure the range is still poisoned. */
	ASSERT_FALSE(try_read_buf(&ptr[5 * page_size]));
	ASSERT_FALSE(try_read_buf(&ptr[6 * page_size]));

	/* Make sure we can poison again without issue.*/
	ASSERT_EQ(madvise(&ptr[5 * page_size], 2 * page_size,
			  MADV_GUARD_POISON), 0);

	/* Make sure the range is, yet again, still poisoned. */
	ASSERT_FALSE(try_read_buf(&ptr[5 * page_size]));
	ASSERT_FALSE(try_read_buf(&ptr[6 * page_size]));

	/* Now unpoison the whole range. */
	ASSERT_EQ(madvise(ptr, 10 * page_size, MADV_GUARD_UNPOISON), 0);

	/* Make sure the whole range is readable. */
	for (i = 0; i < 10; i++) {
		ASSERT_TRUE(try_read_buf(&ptr[i * page_size]));
	}

	/* Cleanup. */
	ASSERT_EQ(munmap(ptr, 10 * page_size), 0);
}

/* Split and merge VMAs and make sure guard pages still behave. */
TEST_F(guard_pages, split_merge)
{
	const unsigned long page_size = self->page_size;
	char *ptr, *ptr_new;
	int i;

	ptr = mmap(NULL, 10 * page_size, PROT_READ | PROT_WRITE,
		   MAP_ANON | MAP_PRIVATE, -1, 0);
	ASSERT_NE(ptr, MAP_FAILED);

	/* Poison the whole range. */
	ASSERT_EQ(madvise(ptr, 10 * page_size, MADV_GUARD_POISON), 0);

	/* Make sure the whole range is poisoned. */
	for (i = 0; i < 10; i++) {
		ASSERT_FALSE(try_read_write_buf(&ptr[i * page_size]));
	}

	/* Now unmap some pages in the range so we split. */
	ASSERT_EQ(munmap(&ptr[2 * page_size], page_size), 0);
	ASSERT_EQ(munmap(&ptr[5 * page_size], page_size), 0);
	ASSERT_EQ(munmap(&ptr[8 * page_size], page_size), 0);

	/* Make sure the remaining ranges are poisoned post-split. */
	for (i = 0; i < 2; i++) {
		ASSERT_FALSE(try_read_write_buf(&ptr[i * page_size]));
	}
	for (i = 2; i < 5; i++) {
		ASSERT_FALSE(try_read_write_buf(&ptr[i * page_size]));
	}
	for (i = 6; i < 8; i++) {
		ASSERT_FALSE(try_read_write_buf(&ptr[i * page_size]));
	}
	for (i = 9; i < 10; i++) {
		ASSERT_FALSE(try_read_write_buf(&ptr[i * page_size]));
	}

	/* Now map them again - the unmap will have cleared the poison. */
	ptr_new = mmap(&ptr[2 * page_size], page_size, PROT_READ | PROT_WRITE,
		       MAP_FIXED | MAP_ANON | MAP_PRIVATE, -1, 0);
	ASSERT_NE(ptr_new, MAP_FAILED);
	ptr_new = mmap(&ptr[5 * page_size], page_size, PROT_READ | PROT_WRITE,
		       MAP_FIXED | MAP_ANON | MAP_PRIVATE, -1, 0);
	ASSERT_NE(ptr_new, MAP_FAILED);
	ptr_new = mmap(&ptr[8 * page_size], page_size, PROT_READ | PROT_WRITE,
		       MAP_FIXED | MAP_ANON | MAP_PRIVATE, -1, 0);
	ASSERT_NE(ptr_new, MAP_FAILED);

	/* Now make sure poisoning is as expected. */
	for (i = 0; i < 10; i++) {
		bool result = try_read_write_buf(&ptr[i * page_size]);

		if (i == 2 || i == 5 || i == 8) {
			ASSERT_TRUE(result);
		} else {
			ASSERT_FALSE(result);
		}
	}

	/* Now poison everything again. */
	ASSERT_EQ(madvise(ptr, 10 * page_size, MADV_GUARD_POISON), 0);

	/* Make sure the whole range is poisoned. */
	for (i = 0; i < 10; i++) {
		ASSERT_FALSE(try_read_write_buf(&ptr[i * page_size]));
	}

	/* Now split the range into three. */
	ASSERT_EQ(mprotect(ptr, 3 * page_size, PROT_READ), 0);
	ASSERT_EQ(mprotect(&ptr[7 * page_size], 3 * page_size, PROT_READ), 0);

	/* Make sure the whole range is poisoned for read. */
	for (i = 0; i < 10; i++) {
		ASSERT_FALSE(try_read_buf(&ptr[i * page_size]));
	}

	/* Now reset protection bits so we merge the whole thing. */
	ASSERT_EQ(mprotect(ptr, 3 * page_size, PROT_READ | PROT_WRITE), 0);
	ASSERT_EQ(mprotect(&ptr[7 * page_size], 3 * page_size,
			   PROT_READ | PROT_WRITE), 0);

	/* Make sure the whole range is still poisoned. */
	for (i = 0; i < 10; i++) {
		ASSERT_FALSE(try_read_write_buf(&ptr[i * page_size]));
	}

	/* Split range into 3 again... */
	ASSERT_EQ(mprotect(ptr, 3 * page_size, PROT_READ), 0);
	ASSERT_EQ(mprotect(&ptr[7 * page_size], 3 * page_size, PROT_READ), 0);

	/* ...and unpoison the whole range. */
	ASSERT_EQ(madvise(ptr, 10 * page_size, MADV_GUARD_UNPOISON), 0);

	/* Make sure the whole range is remedied for read. */
	for (i = 0; i < 10; i++) {
		ASSERT_TRUE(try_read_buf(&ptr[i * page_size]));
	}

	/* Merge them again. */
	ASSERT_EQ(mprotect(ptr, 3 * page_size, PROT_READ | PROT_WRITE), 0);
	ASSERT_EQ(mprotect(&ptr[7 * page_size], 3 * page_size,
			   PROT_READ | PROT_WRITE), 0);

	/* Now ensure the merged range is remedied for read/write. */
	for (i = 0; i < 10; i++) {
		ASSERT_TRUE(try_read_write_buf(&ptr[i * page_size]));
	}

	/* Cleanup. */
	ASSERT_EQ(munmap(ptr, 10 * page_size), 0);
}

/* Assert that MADV_DONTNEED does not remove guard poison markers. */
TEST_F(guard_pages, dontneed)
{
	const unsigned long page_size = self->page_size;
	char *ptr;
	int i;

	ptr = mmap(NULL, 10 * page_size, PROT_READ | PROT_WRITE,
		   MAP_ANON | MAP_PRIVATE, -1, 0);
	ASSERT_NE(ptr, MAP_FAILED);

	/* Back the whole range. */
	for (i = 0; i < 10; i++) {
		ptr[i * page_size] = 'y';
	}

	/* Poison every other page. */
	for (i = 0; i < 10; i += 2) {
		ASSERT_EQ(madvise(&ptr[i * page_size],
				  page_size, MADV_GUARD_POISON), 0);
	}

	/* Indicate that we don't need any of the range. */
	ASSERT_EQ(madvise(ptr, 10 * page_size, MADV_DONTNEED), 0);

	/* Check to ensure poison markers are still in place. */
	for (i = 0; i < 10; i++) {
		bool result = try_read_buf(&ptr[i * page_size]);

		if (i % 2 == 0) {
			ASSERT_FALSE(result);
		} else {
			ASSERT_TRUE(result);
			/* Make sure we really did get reset to zero page. */
			ASSERT_EQ(ptr[i * page_size], '\0');
		}

		/* Now write... */
		result = try_write_buf(&ptr[i * page_size]);

		/* ...and make sure same result. */
		if (i % 2 == 0) {
			ASSERT_FALSE(result);
		} else {
			ASSERT_TRUE(result);
		}
	}

	/* Cleanup. */
	ASSERT_EQ(munmap(ptr, 10 * page_size), 0);
}

/* Assert that mlock()'ed pages work correctly with poison markers. */
TEST_F(guard_pages, mlock)
{
	const unsigned long page_size = self->page_size;
	char *ptr;
	int i;

	ptr = mmap(NULL, 10 * page_size, PROT_READ | PROT_WRITE,
		   MAP_ANON | MAP_PRIVATE, -1, 0);
	ASSERT_NE(ptr, MAP_FAILED);

	/* Populate. */
	for (i = 0; i < 10; i++) {
		ptr[i * page_size] = 'y';
	}

	/* Lock. */
	ASSERT_EQ(mlock(ptr, 10 * page_size), 0);

	/* Now try to poison, should fail with EINVAL. */
	ASSERT_EQ(madvise(ptr, 10 * page_size, MADV_GUARD_POISON), -1);
	ASSERT_EQ(errno, EINVAL);

	/* OK unlock. */
	ASSERT_EQ(munlock(ptr, 10 * page_size), 0);

	/* Poison first half of range, should now succeed. */
	ASSERT_EQ(madvise(ptr, 5 * page_size, MADV_GUARD_POISON), 0);

	/* Make sure poison works. */
	for (i = 0; i < 10; i++) {
		bool result = try_read_write_buf(&ptr[i * page_size]);

		if (i < 5) {
			ASSERT_FALSE(result);
		} else {
			ASSERT_TRUE(result);
			ASSERT_EQ(ptr[i * page_size], 'x');
		}
	}

	/*
	 * Now lock the latter part of the range. We can't lock the poisoned
	 * pages, as this would result in the pages being populated and the
	 * poisoning would cause this to error out.
	 */
	ASSERT_EQ(mlock(&ptr[5 * page_size], 5 * page_size), 0);

	/*
	 * Now unpoison, we do not permit mlock()'d ranges to be remedied as it is
	 * a non-destructive operation.
	 */
	ASSERT_EQ(madvise(ptr, 10 * page_size, MADV_GUARD_UNPOISON), 0);

	/* Now check that everything is remedied. */
	for (i = 0; i < 10; i++) {
		ASSERT_TRUE(try_read_write_buf(&ptr[i * page_size]));
	}

	/* Cleanup. */
	ASSERT_EQ(munmap(ptr, 10 * page_size), 0);
}

/*
 * Assert that moving, extending and shrinking memory via mremap() retains
 * poison markers where possible.
 *
 * - Moving a mapping alone should retain markers as they are.
 */
TEST_F(guard_pages, mremap_move)
{
	const unsigned long page_size = self->page_size;
	char *ptr, *ptr_new;

	/* Map 5 pages. */
	ptr = mmap(NULL, 5 * page_size, PROT_READ | PROT_WRITE,
		   MAP_ANON | MAP_PRIVATE, -1, 0);
	ASSERT_NE(ptr, MAP_FAILED);

	/* Place poison markers at both ends of the 5 page span. */
	ASSERT_EQ(madvise(ptr, page_size, MADV_GUARD_POISON), 0);
	ASSERT_EQ(madvise(&ptr[4 * page_size], page_size, MADV_GUARD_POISON), 0);

	/* Make sure the poison is in effect. */
	ASSERT_FALSE(try_read_write_buf(ptr));
	ASSERT_FALSE(try_read_write_buf(&ptr[4 * page_size]));

	/* Map a new region we will move this range into. Doing this ensures
	 * that we have reserved a range to map into.
	 */
	ptr_new = mmap(NULL, 5 * page_size, PROT_NONE, MAP_ANON | MAP_PRIVATE,
		       -1, 0);
	ASSERT_NE(ptr_new, MAP_FAILED);

	ASSERT_EQ(mremap(ptr, 5 * page_size, 5 * page_size,
			 MREMAP_MAYMOVE | MREMAP_FIXED, ptr_new), ptr_new);

	/* Make sure the poison is retained. */
	ASSERT_FALSE(try_read_write_buf(ptr_new));
	ASSERT_FALSE(try_read_write_buf(&ptr_new[4 * page_size]));

	/*
	 * Clean up - we only need reference the new pointer as we overwrote the
	 * PROT_NONE range and moved the existing one.
	 */
	munmap(ptr_new, 5 * page_size);
}

/*
 * Assert that moving, extending and shrinking memory via mremap() retains
 * poison markers where possible.
 *
 * - Expanding should retain, only now in different position. The user will have
 *   to unpoison manually to fix up (they'd have to do the same if it were a
 *   PROT_NONE mapping)
 */
TEST_F(guard_pages, mremap_expand)
{
	const unsigned long page_size = self->page_size;
	char *ptr, *ptr_new;

	/* Map 10 pages... */
	ptr = mmap(NULL, 10 * page_size, PROT_READ | PROT_WRITE,
		   MAP_ANON | MAP_PRIVATE, -1, 0);
	ASSERT_NE(ptr, MAP_FAILED);
	/* ...But unmap the last 5 so we can ensure we can expand into them. */
	ASSERT_EQ(munmap(&ptr[5 * page_size], 5 * page_size), 0);

	/* Place poison markers at both ends of the 5 page span. */
	ASSERT_EQ(madvise(ptr, page_size, MADV_GUARD_POISON), 0);
	ASSERT_EQ(madvise(&ptr[4 * page_size], page_size, MADV_GUARD_POISON), 0);

	/* Make sure the poison is in effect. */
	ASSERT_FALSE(try_read_write_buf(ptr));
	ASSERT_FALSE(try_read_write_buf(&ptr[4 * page_size]));

	/* Now expand to 10 pages. */
	ptr = mremap(ptr, 5 * page_size, 10 * page_size, 0);
	ASSERT_NE(ptr, MAP_FAILED);

	/* Make sure the poison is retained in its original positions. */
	ASSERT_FALSE(try_read_write_buf(ptr));
	ASSERT_FALSE(try_read_write_buf(&ptr[4 * page_size]));

	/* Reserve a region which we can move to and expand into. */
	ptr_new = mmap(NULL, 20 * page_size, PROT_NONE,
		       MAP_ANON | MAP_PRIVATE, -1, 0);
	ASSERT_NE(ptr_new, MAP_FAILED);

	/* Now move and expand into it. */
	ptr = mremap(ptr, 10 * page_size, 20 * page_size,
		     MREMAP_MAYMOVE | MREMAP_FIXED, ptr_new);
	ASSERT_EQ(ptr, ptr_new);

	/* Again, make sure the poison is retained in its original
	 * positions. */
	ASSERT_FALSE(try_read_write_buf(ptr));
	ASSERT_FALSE(try_read_write_buf(&ptr[4 * page_size]));

	/*
	 * A real user would have to unpoison, but would reasonably expect all
	 * characteristics of the mapping to be retained, including poison
	 * markers.
	 */

	/* Cleanup. */
	munmap(ptr, 20 * page_size);
}
/*
 * Assert that moving, extending and shrinking memory via mremap() retains
 * poison markers where possible.
 *
 * - Shrinking will result in markers that are shrunk over being removed. Again,
 *   if the user were using a PROT_NONE mapping they'd have to manually fix this
 *   up also so this is OK.
 */
TEST_F(guard_pages, mremap_shrink)
{
	const unsigned long page_size = self->page_size;
	char *ptr;
	int i;

	/* Map 5 pages. */
	ptr = mmap(NULL, 5 * page_size, PROT_READ | PROT_WRITE,
		   MAP_ANON | MAP_PRIVATE, -1, 0);
	ASSERT_NE(ptr, MAP_FAILED);

	/* Place poison markers at both ends of the 5 page span. */
	ASSERT_EQ(madvise(ptr, page_size, MADV_GUARD_POISON), 0);
	ASSERT_EQ(madvise(&ptr[4 * page_size], page_size, MADV_GUARD_POISON), 0);

	/* Make sure the poison is in effect. */
	ASSERT_FALSE(try_read_write_buf(ptr));
	ASSERT_FALSE(try_read_write_buf(&ptr[4 * page_size]));

	/* Now shrink to 3 pages. */
	ptr = mremap(ptr, 5 * page_size, 3 * page_size, MREMAP_MAYMOVE);
	ASSERT_NE(ptr, MAP_FAILED);

	/* We expect the poison marker at the start to be retained... */
	ASSERT_FALSE(try_read_write_buf(ptr));

	/* ...But remaining pages will not have poison markers. */
	for (i = 1; i < 3; i++) {
		ASSERT_TRUE(try_read_write_buf(&ptr[i + page_size]));
	}

	/*
	 * As with expansion, a real user would have to unpoison and fixup. But
	 * you'd have to do similar manual things with PROT_NONE mappings too.
	 */

	/*
	 * If we expand back to the original size, the end marker will, of
	 * course, no longer be present.
	 */
	ptr = mremap(ptr, 3 * page_size, 5 * page_size, 0);
	ASSERT_NE(ptr, MAP_FAILED);

	/* Again, we expect the poison marker at the start to be retained... */
	ASSERT_FALSE(try_read_write_buf(ptr));

	/* ...But remaining pages will not have poison markers. */
	for (i = 1; i < 5; i++) {
		ASSERT_TRUE(try_read_write_buf(&ptr[i + page_size]));
	}

	/* Cleanup. */
	munmap(ptr, 5 * page_size);
}

/*
 * Assert that forking a process with VMAs that do not have VM_WIPEONFORK set
 * retain guard pages.
 */
TEST_F(guard_pages, fork)
{
	const unsigned long page_size = self->page_size;
	char *ptr;
	pid_t pid;
	int i;

	/* Map 10 pages. */
	ptr = mmap(NULL, 10 * page_size, PROT_READ | PROT_WRITE,
		   MAP_ANON | MAP_PRIVATE, -1, 0);
	ASSERT_NE(ptr, MAP_FAILED);

	/* Poison the first 5 pages. */
	ASSERT_EQ(madvise(ptr, 5 * page_size, MADV_GUARD_POISON), 0);

	pid = fork();
	ASSERT_NE(pid, -1);
	if (!pid) {
		/* This is the child process now. */

		/* Assert that the poisoning is in effect. */
		for (i = 0; i < 10; i++) {
			bool result = try_read_write_buf(&ptr[i * page_size]);

			if (i < 5) {
				ASSERT_FALSE(result);
			} else {
				ASSERT_TRUE(result);
			}
		}

		/* Now unpoison the range.*/
		ASSERT_EQ(madvise(ptr, 10 * page_size, MADV_GUARD_UNPOISON), 0);

		exit(0);
	}

	/* Parent process. */

	/* Parent simply waits on child. */
	waitpid(pid, NULL, 0);

	/* Child unpoison does not impact parent page table state. */
	for (i = 0; i < 10; i++) {
		bool result = try_read_write_buf(&ptr[i * page_size]);

		if (i < 5) {
			ASSERT_FALSE(result);
		} else {
			ASSERT_TRUE(result);
		}
	}

	/* Cleanup. */
	ASSERT_EQ(munmap(ptr, 10 * page_size), 0);
}

/*
 * Assert that forking a process with VMAs that do have VM_WIPEONFORK set
 * behave as expected.
 */
TEST_F(guard_pages, fork_wipeonfork)
{
	const unsigned long page_size = self->page_size;
	char *ptr;
	pid_t pid;
	int i;

	/* Map 10 pages. */
	ptr = mmap(NULL, 10 * page_size, PROT_READ | PROT_WRITE,
		   MAP_ANON | MAP_PRIVATE, -1, 0);
	ASSERT_NE(ptr, MAP_FAILED);

	/* Mark wipe on fork. */
	ASSERT_EQ(madvise(ptr, 10 * page_size, MADV_WIPEONFORK), 0);

	/* Poison the first 5 pages. */
	ASSERT_EQ(madvise(ptr, 5 * page_size, MADV_GUARD_POISON), 0);

	pid = fork();
	ASSERT_NE(pid, -1);
	if (!pid) {
		/* This is the child process now. */

		/* Poison will have been wiped. */
		for (i = 0; i < 10; i++) {
			ASSERT_TRUE(try_read_write_buf(&ptr[i * page_size]));
		}

		exit(0);
	}

	/* Parent process. */

	waitpid(pid, NULL, 0);

	/* Poison should be in effect.*/
	for (i = 0; i < 10; i++) {
		bool result = try_read_write_buf(&ptr[i * page_size]);

		if (i < 5) {
			ASSERT_FALSE(result);
		} else {
			ASSERT_TRUE(result);
		}
	}

	/* Cleanup. */
	ASSERT_EQ(munmap(ptr, 10 * page_size), 0);
}

/* Ensure that MADV_FREE frees poison entries as expected. */
TEST_F(guard_pages, lazyfree)
{
	const unsigned long page_size = self->page_size;
	char *ptr;
	int i;

	/* Map 10 pages. */
	ptr = mmap(NULL, 10 * page_size, PROT_READ | PROT_WRITE,
		   MAP_ANON | MAP_PRIVATE, -1, 0);
	ASSERT_NE(ptr, MAP_FAILED);

	/* Poison range. */
	ASSERT_EQ(madvise(ptr, 10 * page_size, MADV_GUARD_POISON), 0);

	/* Ensure poisoned. */
	for (i = 0; i < 10; i++) {
		ASSERT_FALSE(try_read_write_buf(&ptr[i * page_size]));
	}

	/* Lazyfree range. */
	ASSERT_EQ(madvise(ptr, 10 * page_size, MADV_FREE), 0);

	/* This should simply clear the poison markers. */
	for (i = 0; i < 10; i++) {
		ASSERT_TRUE(try_read_write_buf(&ptr[i * page_size]));
	}

	/* Cleanup. */
	ASSERT_EQ(munmap(ptr, 10 * page_size), 0);
}

/* Ensure that MADV_POPULATE_READ, MADV_POPULATE_WRITE behave as expected. */
TEST_F(guard_pages, populate)
{
	const unsigned long page_size = self->page_size;
	char *ptr;

	/* Map 10 pages. */
	ptr = mmap(NULL, 10 * page_size, PROT_READ | PROT_WRITE,
		   MAP_ANON | MAP_PRIVATE, -1, 0);
	ASSERT_NE(ptr, MAP_FAILED);

	/* Poison range. */
	ASSERT_EQ(madvise(ptr, 10 * page_size, MADV_GUARD_POISON), 0);

	/* Populate read should error out... */
	ASSERT_EQ(madvise(ptr, 10 * page_size, MADV_POPULATE_READ), -1);
	ASSERT_EQ(errno, EFAULT);

	/* ...as should populate write. */
	ASSERT_EQ(madvise(ptr, 10 * page_size, MADV_POPULATE_WRITE), -1);
	ASSERT_EQ(errno, EFAULT);

	/* Cleanup. */
	ASSERT_EQ(munmap(ptr, 10 * page_size), 0);
}

/* Ensure that MADV_COLD, MADV_PAGEOUT do not remove poison markers. */
TEST_F(guard_pages, cold_pageout)
{
	const unsigned long page_size = self->page_size;
	char *ptr;
	int i;

	/* Map 10 pages. */
	ptr = mmap(NULL, 10 * page_size, PROT_READ | PROT_WRITE,
		   MAP_ANON | MAP_PRIVATE, -1, 0);
	ASSERT_NE(ptr, MAP_FAILED);

	/* Poison range. */
	ASSERT_EQ(madvise(ptr, 10 * page_size, MADV_GUARD_POISON), 0);

	/* Ensured poisoned. */
	for (i = 0; i < 10; i++) {
		ASSERT_FALSE(try_read_write_buf(&ptr[i * page_size]));
	}

	/* Now mark cold. This should have no impact on poison markers. */
	ASSERT_EQ(madvise(ptr, 10 * page_size, MADV_COLD), 0);

	/* Should remain poisoned. */
	for (i = 0; i < 10; i++) {
		ASSERT_FALSE(try_read_write_buf(&ptr[i * page_size]));
	}

	/* OK, now page out. This should equally, have no effect on markers. */
	ASSERT_EQ(madvise(ptr, 10 * page_size, MADV_PAGEOUT), 0);

	/* Should remain poisoned. */
	for (i = 0; i < 10; i++) {
		ASSERT_FALSE(try_read_write_buf(&ptr[i * page_size]));
	}

	/* Cleanup. */
	ASSERT_EQ(munmap(ptr, 10 * page_size), 0);
}

/* Ensure that guard pages do not break userfaultd. */
TEST_F(guard_pages, uffd)
{
	const unsigned long page_size = self->page_size;
	int uffd;
	char *ptr;
	int i;
	struct uffdio_api api = {
		.api = UFFD_API,
		.features = 0,
	};
	struct uffdio_register reg;
	struct uffdio_range range;

	/* Set up uffd. */
	uffd = userfaultfd(0);
	if (uffd == -1 && errno == EPERM)
		ksft_exit_skip("No uffd permissions\n");
	ASSERT_NE(uffd, -1);

	ASSERT_EQ(ioctl(uffd, UFFDIO_API, &api), 0);

	/* Map 10 pages. */
	ptr = mmap(NULL, 10 * page_size, PROT_READ | PROT_WRITE,
		   MAP_ANON | MAP_PRIVATE, -1, 0);
	ASSERT_NE(ptr, MAP_FAILED);

	/* Register the range with uffd. */
	range.start = (unsigned long)ptr;
	range.len = 10 * page_size;
	reg.range = range;
	reg.mode = UFFDIO_REGISTER_MODE_MISSING;
	ASSERT_EQ(ioctl(uffd, UFFDIO_REGISTER, &reg), 0);

	/* Poison the range. This should not trigger the uffd. */
	ASSERT_EQ(madvise(ptr, 10 * page_size, MADV_GUARD_POISON), 0);

	/* The poisoning should behave as usual with no uffd intervention. */
	for (i = 0; i < 10; i++) {
		ASSERT_FALSE(try_read_write_buf(&ptr[i * page_size]));
	}

	/* Cleanup. */
	ASSERT_EQ(ioctl(uffd, UFFDIO_UNREGISTER, &range), 0);
	close(uffd);
	ASSERT_EQ(munmap(ptr, 10 * page_size), 0);
}

TEST_HARNESS_MAIN
