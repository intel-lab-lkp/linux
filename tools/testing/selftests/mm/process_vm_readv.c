// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>
#include <linux/userfaultfd.h>

#include "kselftest_harness.h"

#ifndef PROCESS_VM_PIDFD
#define PROCESS_VM_PIDFD	(1UL << 0)
#endif

#ifndef PROCESS_VM_NOWAIT
#define PROCESS_VM_NOWAIT	(1UL << 1)
#endif

static int sys_pidfd_open(pid_t pid, unsigned int flags)
{
	return syscall(__NR_pidfd_open, pid, flags);
}

static const uint8_t test_data[] = { 0x01, 0x02, 0x03, 0x04,
				     0x05, 0x06, 0x07, 0x08 };
#define POISON_BYTE 0xCC

/*
 * Test: basic process_vm_readv with no flags
 */
TEST(read_basic)
{
	uint8_t buf[sizeof(test_data)];
	struct iovec local_iov = { .iov_base = buf, .iov_len = sizeof(buf) };
	struct iovec remote_iov = {
		.iov_base = (void *)test_data,
		.iov_len = sizeof(test_data)
	};
	ssize_t n;

	memset(buf, POISON_BYTE, sizeof(buf));
	n = process_vm_readv(getpid(), &local_iov, 1, &remote_iov, 1, 0);
	ASSERT_EQ(sizeof(test_data), n);
	ASSERT_EQ(0, memcmp(buf, test_data, sizeof(test_data)));
}

/*
 * Test: invalid flags should return EINVAL
 */
TEST(read_invalid_flags)
{
	uint8_t buf[8] = { 0 };
	struct iovec local_iov = { .iov_base = buf, .iov_len = sizeof(buf) };
	struct iovec remote_iov = {
		.iov_base = (void *)test_data,
		.iov_len = sizeof(test_data)
	};
	ssize_t n;

	n = process_vm_readv(getpid(), &local_iov, 1, &remote_iov, 1, 255);
	ASSERT_EQ(-1, n);
	ASSERT_EQ(EINVAL, errno);
}

/*
 * Test: invalid address should return EFAULT
 */
TEST(read_invalid_address)
{
	uint8_t buf[8] = { 0 };
	struct iovec local_iov = { .iov_base = buf, .iov_len = sizeof(buf) };
	struct iovec remote_iov = { .iov_base = NULL, .iov_len = 8 };
	ssize_t n;

	n = process_vm_readv(getpid(), &local_iov, 1, &remote_iov, 1, 0);
	ASSERT_EQ(-1, n);
	ASSERT_EQ(EFAULT, errno);
}

/*
 * Test: invalid address with invalid flags should return EINVAL
 * (flag check happens before address validation)
 */
TEST(read_invalid_address_invalid_flags)
{
	uint8_t buf[8] = { 0 };
	struct iovec local_iov = { .iov_base = buf, .iov_len = sizeof(buf) };
	struct iovec remote_iov = { .iov_base = NULL, .iov_len = 8 };
	ssize_t n;

	n = process_vm_readv(getpid(), &local_iov, 1, &remote_iov, 1, 255);
	ASSERT_EQ(-1, n);
	ASSERT_EQ(EINVAL, errno);
}

/*
 * Test: invalid address with all valid flags should return EFAULT
 * (flags are valid so we get past the flag check to the address check)
 */
TEST(read_invalid_address_all_valid_flags)
{
	int pidfd;
	struct iovec local_iov = { .iov_base = NULL, .iov_len = 8 };
	struct iovec remote_iov = { .iov_base = NULL, .iov_len = 8 };
	ssize_t n;

	pidfd = sys_pidfd_open(getpid(), 0);
	if (pidfd < 0 && errno == ENOSYS)
		SKIP(return, "pidfd_open not supported");
	ASSERT_GE(pidfd, 0);

	n = process_vm_readv(pidfd, &local_iov, 1, &remote_iov, 1,
			     PROCESS_VM_PIDFD | PROCESS_VM_NOWAIT);
	ASSERT_EQ(-1, n);
	ASSERT_EQ(EFAULT, errno);

	close(pidfd);
}

/*
 * Test: read with an invalid pidfd should return an error, not crash
 */
TEST(read_invalid_pidfd)
{
	uint8_t buf[sizeof(test_data)] = { 0 };
	struct iovec local_iov = { .iov_base = buf, .iov_len = sizeof(buf) };
	struct iovec remote_iov = {
		.iov_base = (void *)test_data,
		.iov_len = sizeof(test_data)
	};
	ssize_t n;

	/* fd 9999 is almost certainly not a valid pidfd */
	n = process_vm_readv(9999, &local_iov, 1, &remote_iov, 1,
			     PROCESS_VM_PIDFD);
	ASSERT_EQ(-1, n);
	if (errno == EINVAL)
		SKIP(return, "PROCESS_VM_PIDFD not supported");
	ASSERT_EQ(EBADF, errno);
}

/*
 * Test: read with an invalid pid should return ESRCH
 */
TEST(read_invalid_pid)
{
	uint8_t buf[sizeof(test_data)] = { 0 };
	struct iovec local_iov = { .iov_base = buf, .iov_len = sizeof(buf) };
	struct iovec remote_iov = {
		.iov_base = (void *)test_data,
		.iov_len = sizeof(test_data)
	};
	ssize_t n;

	/* pid 999999 is almost certainly not a valid process */
	n = process_vm_readv(999999, &local_iov, 1, &remote_iov, 1, 0);
	ASSERT_EQ(-1, n);
	ASSERT_EQ(ESRCH, errno);
}

/*
 * Test: read with PIDFD flag
 */
TEST(read_pidfd)
{
	uint8_t buf[sizeof(test_data)];
	struct iovec local_iov = { .iov_base = buf, .iov_len = sizeof(buf) };
	struct iovec remote_iov = {
		.iov_base = (void *)test_data,
		.iov_len = sizeof(test_data)
	};
	ssize_t n;
	int pidfd;

	memset(buf, POISON_BYTE, sizeof(buf));
	pidfd = sys_pidfd_open(getpid(), 0);
	if (pidfd < 0 && errno == ENOSYS)
		SKIP(return, "pidfd_open not supported");
	ASSERT_GE(pidfd, 0);

	n = process_vm_readv(pidfd, &local_iov, 1, &remote_iov, 1,
			     PROCESS_VM_PIDFD);
	ASSERT_EQ(sizeof(test_data), n);
	ASSERT_EQ(0, memcmp(buf, test_data, sizeof(test_data)));

	close(pidfd);
}

/*
 * Test: read with NOWAIT from resident memory (should succeed)
 */
TEST(read_nowait_resident)
{
	uint8_t buf[sizeof(test_data)];
	struct iovec local_iov = { .iov_base = buf, .iov_len = sizeof(buf) };
	struct iovec remote_iov = {
		.iov_base = (void *)test_data,
		.iov_len = sizeof(test_data)
	};
	ssize_t n;

	*(volatile uint64_t *)test_data; /* fault in page for NOWAIT */
	memset(buf, POISON_BYTE, sizeof(buf));
	n = process_vm_readv(getpid(), &local_iov, 1, &remote_iov, 1,
			     PROCESS_VM_NOWAIT);
	ASSERT_EQ(sizeof(test_data), n);
	ASSERT_EQ(0, memcmp(buf, test_data, sizeof(test_data)));
}

/*
 * Test: read with PIDFD + NOWAIT from resident memory
 */
TEST(read_pidfd_nowait_resident)
{
	uint8_t buf[sizeof(test_data)];
	struct iovec local_iov = { .iov_base = buf, .iov_len = sizeof(buf) };
	struct iovec remote_iov = {
		.iov_base = (void *)test_data,
		.iov_len = sizeof(test_data)
	};
	ssize_t n;
	int pidfd;

	*(volatile uint64_t *)test_data; /* fault in page for NOWAIT */
	memset(buf, POISON_BYTE, sizeof(buf));
	pidfd = sys_pidfd_open(getpid(), 0);
	if (pidfd < 0 && errno == ENOSYS)
		SKIP(return, "pidfd_open not supported");
	ASSERT_GE(pidfd, 0);

	n = process_vm_readv(pidfd, &local_iov, 1, &remote_iov, 1,
			     PROCESS_VM_PIDFD | PROCESS_VM_NOWAIT);
	ASSERT_EQ(sizeof(test_data), n);
	ASSERT_EQ(0, memcmp(buf, test_data, sizeof(test_data)));

	close(pidfd);
}

/*
 * Userfaultfd helpers for NOWAIT tests
 */
static int setup_userfaultfd(void)
{
	struct uffdio_api api = { .api = UFFD_API };
	int uffd;

	uffd = syscall(__NR_userfaultfd, O_CLOEXEC | O_NONBLOCK);
	if (uffd < 0)
		return -errno;

	if (ioctl(uffd, UFFDIO_API, &api)) {
		close(uffd);
		return -errno;
	}

	return uffd;
}

static void *register_uffd_region(int uffd, size_t size)
{
	struct uffdio_register reg;
	void *mem;

	mem = mmap(NULL, size, PROT_READ | PROT_WRITE,
		   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (mem == MAP_FAILED)
		return NULL;

	reg.range.start = (unsigned long)mem;
	reg.range.len = size;
	reg.mode = UFFDIO_REGISTER_MODE_MISSING;
	if (ioctl(uffd, UFFDIO_REGISTER, &reg)) {
		munmap(mem, size);
		return NULL;
	}

	return mem;
}

struct uffd_handler_args {
	int uffd;
	const void *content;
	size_t content_len;
};

static void *uffd_handler_thread(void *arg)
{
	struct uffd_handler_args *ha = arg;
	struct uffd_msg msg;
	struct uffdio_copy uffd_copy;
	struct pollfd pfd = {
		.fd = ha->uffd,
		.events = POLLIN
	};
	void *page;
	long page_size = sysconf(_SC_PAGESIZE);
	int ret;

	page = mmap(NULL, page_size, PROT_READ | PROT_WRITE,
		    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (page == MAP_FAILED)
		return (void *)(long)-ENOMEM;

	memcpy(page, ha->content, ha->content_len);

	ret = poll(&pfd, 1, 5000);
	if (ret <= 0)
		goto out;

	if (read(ha->uffd, &msg, sizeof(msg)) != sizeof(msg))
		goto out;

	if (msg.event != UFFD_EVENT_PAGEFAULT)
		goto out;

	uffd_copy.dst = msg.arg.pagefault.address & ~(page_size - 1);
	uffd_copy.src = (unsigned long)page;
	uffd_copy.len = page_size;
	uffd_copy.mode = 0;
	ioctl(ha->uffd, UFFDIO_COPY, &uffd_copy);

out:
	munmap(page, page_size);
	return NULL;
}

/*
 * Test: read from userfaultfd-registered memory (no flags, should block
 * until page fault is resolved by handler thread)
 */
TEST(read_userfaultfd_blocking)
{
	int uffd;
	void *mem;
	long page_size = sysconf(_SC_PAGESIZE);
	uint8_t buf[sizeof(test_data)];
	struct iovec local_iov = { .iov_base = buf, .iov_len = sizeof(buf) };
	struct iovec remote_iov;
	struct uffd_handler_args ha;
	pthread_t handler;
	ssize_t n;

	memset(buf, POISON_BYTE, sizeof(buf));

	uffd = setup_userfaultfd();
	if (uffd == -EPERM)
		SKIP(return, "userfaultfd requires privileges (vm.unprivileged_userfaultfd=0)");
	if (uffd == -ENOSYS)
		SKIP(return, "userfaultfd not supported");
	ASSERT_GE(uffd, 0);

	mem = register_uffd_region(uffd, page_size);
	ASSERT_NE(NULL, mem);

	ha.uffd = uffd;
	ha.content = test_data;
	ha.content_len = sizeof(test_data);
	ASSERT_EQ(0, pthread_create(&handler, NULL, uffd_handler_thread, &ha));

	remote_iov.iov_base = mem;
	remote_iov.iov_len = sizeof(test_data);
	n = process_vm_readv(getpid(), &local_iov, 1, &remote_iov, 1, 0);
	ASSERT_EQ(sizeof(test_data), n);
	ASSERT_EQ(0, memcmp(buf, test_data, sizeof(test_data)));

	pthread_join(handler, NULL);
	munmap(mem, page_size);
	close(uffd);
}

/*
 * Test: read with NOWAIT from userfaultfd-registered memory that has
 * not been faulted in yet. Should return EFAULT (not block).
 */
TEST(read_nowait_userfaultfd)
{
	int uffd;
	void *mem;
	long page_size = sysconf(_SC_PAGESIZE);
	uint8_t buf[sizeof(test_data)] = { 0 };
	struct iovec local_iov = { .iov_base = buf, .iov_len = sizeof(buf) };
	struct iovec remote_iov;
	ssize_t n;

	uffd = setup_userfaultfd();
	if (uffd == -EPERM)
		SKIP(return, "userfaultfd requires privileges (vm.unprivileged_userfaultfd=0)");
	if (uffd == -ENOSYS)
		SKIP(return, "userfaultfd not supported");
	ASSERT_GE(uffd, 0);

	mem = register_uffd_region(uffd, page_size);
	ASSERT_NE(NULL, mem);

	/* Ensure the page is not present */
	madvise(mem, page_size, MADV_DONTNEED);

	remote_iov.iov_base = mem;
	remote_iov.iov_len = sizeof(test_data);
	n = process_vm_readv(getpid(), &local_iov, 1, &remote_iov, 1,
			     PROCESS_VM_NOWAIT);
	ASSERT_EQ(-1, n);
	ASSERT_EQ(EFAULT, errno);

	munmap(mem, page_size);
	close(uffd);
}

TEST_HARNESS_MAIN
