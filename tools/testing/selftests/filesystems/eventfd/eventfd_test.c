// SPDX-License-Identifier: GPL-2.0

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <asm/unistd.h>
#include <linux/time_types.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>
#include <signal.h>
#include <pthread.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
/*
 * Prevent <asm-generic/fcntl.h> (pulled in via <linux/eventfd.h> ->
 * <linux/fcntl.h> -> <asm/fcntl.h>) from redefining struct flock and
 * friends that are already provided by the system <fcntl.h> above.
 */
#define _ASM_GENERIC_FCNTL_H
#include <linux/eventfd.h>
#include "kselftest_harness.h"

#define EVENTFD_TEST_ITERATIONS 100000UL

struct error {
	int  code;
	char msg[512];
};

static int error_set(struct error *err, int code, const char *fmt, ...)
{
	va_list args;
	int r;

	if (code == 0 || !err || err->code != 0)
		return code;

	err->code = code;
	va_start(args, fmt);
	r = vsnprintf(err->msg, sizeof(err->msg), fmt, args);
	assert((size_t)r < sizeof(err->msg));
	va_end(args);

	return code;
}

static inline int sys_eventfd2(unsigned int count, int flags)
{
	return syscall(__NR_eventfd2, count, flags);
}

TEST(eventfd_check_flag_rdwr)
{
	int fd, flags;

	fd = sys_eventfd2(0, 0);
	ASSERT_GE(fd, 0);

	flags = fcntl(fd, F_GETFL);
	// The kernel automatically adds the O_RDWR flag.
	EXPECT_EQ(flags, O_RDWR);

	close(fd);
}

TEST(eventfd_check_flag_cloexec)
{
	int fd, flags;

	fd = sys_eventfd2(0, EFD_CLOEXEC);
	ASSERT_GE(fd, 0);

	flags = fcntl(fd, F_GETFD);
	ASSERT_GT(flags, -1);
	EXPECT_EQ(flags, FD_CLOEXEC);

	close(fd);
}

TEST(eventfd_check_flag_nonblock)
{
	int fd, flags;

	fd = sys_eventfd2(0, EFD_NONBLOCK);
	ASSERT_GE(fd, 0);

	flags = fcntl(fd, F_GETFL);
	ASSERT_GT(flags, -1);
	EXPECT_EQ(flags & EFD_NONBLOCK, EFD_NONBLOCK);
	EXPECT_EQ(flags & O_RDWR, O_RDWR);

	close(fd);
}

TEST(eventfd_check_flag_cloexec_and_nonblock)
{
	int fd, flags;

	fd = sys_eventfd2(0, EFD_CLOEXEC|EFD_NONBLOCK);
	ASSERT_GE(fd, 0);

	flags = fcntl(fd, F_GETFL);
	ASSERT_GT(flags, -1);
	EXPECT_EQ(flags & EFD_NONBLOCK, EFD_NONBLOCK);
	EXPECT_EQ(flags & O_RDWR, O_RDWR);

	flags = fcntl(fd, F_GETFD);
	ASSERT_GT(flags, -1);
	EXPECT_EQ(flags, FD_CLOEXEC);

	close(fd);
}

static inline void trim_newline(char *str)
{
	char *pos = strrchr(str, '\n');

	if (pos)
		*pos = '\0';
}

static int verify_fdinfo(int fd, struct error *err, const char *prefix,
		size_t prefix_len, const char *expect, ...)
{
	char buffer[512] = {0, };
	char path[512] = {0, };
	va_list args;
	FILE *f;
	char *line = NULL;
	size_t n = 0;
	int found = 0;
	int r;

	va_start(args, expect);
	r = vsnprintf(buffer, sizeof(buffer), expect, args);
	assert((size_t)r < sizeof(buffer));
	va_end(args);

	snprintf(path, sizeof(path), "/proc/self/fdinfo/%d", fd);
	f = fopen(path, "re");
	if (!f)
		return error_set(err, -1, "fdinfo open failed for %d", fd);

	while (getline(&line, &n, f) != -1) {
		char *val;

		if (strncmp(line, prefix, prefix_len))
			continue;

		found = 1;

		val = line + prefix_len;
		r = strcmp(val, buffer);
		if (r != 0) {
			trim_newline(line);
			trim_newline(buffer);
			error_set(err, -1, "%s '%s' != '%s'",
				  prefix, val, buffer);
		}
		break;
	}

	free(line);
	fclose(f);

	if (found == 0)
		return error_set(err, -1, "%s not found for fd %d",
				 prefix, fd);

	return 0;
}

TEST(eventfd_check_flag_semaphore)
{
	struct error err = {0};
	int fd, ret;

	fd = sys_eventfd2(0, EFD_SEMAPHORE);
	ASSERT_GE(fd, 0);

	ret = fcntl(fd, F_GETFL);
	ASSERT_GT(ret, -1);
	EXPECT_EQ(ret & O_RDWR, O_RDWR);

	// The semaphore could only be obtained from fdinfo.
	ret = verify_fdinfo(fd, &err, "eventfd-semaphore: ", 19, "1\n");
	if (ret != 0)
		ksft_print_msg("eventfd semaphore flag check failed: %s\n", err.msg);
	EXPECT_EQ(ret, 0);

	close(fd);
}

/*
 * A write(2) fails with the error EINVAL if the size of the supplied buffer
 * is less than 8 bytes, or if an attempt is made to write the value
 * 0xffffffffffffffff.
 */
TEST(eventfd_check_write)
{
	uint64_t value = 1;
	ssize_t size;
	int fd;

	fd = sys_eventfd2(0, 0);
	ASSERT_GE(fd, 0);

	size = write(fd, &value, sizeof(int));
	EXPECT_EQ(size, -1);
	EXPECT_EQ(errno, EINVAL);

	size = write(fd, &value, sizeof(value));
	EXPECT_EQ(size, sizeof(value));

	value = (uint64_t)-1;
	size = write(fd, &value, sizeof(value));
	EXPECT_EQ(size, -1);
	EXPECT_EQ(errno, EINVAL);

	close(fd);
}

/*
 * A read(2) fails with the error EINVAL if the size of the supplied buffer is
 * less than 8 bytes.
 */
TEST(eventfd_check_read)
{
	uint64_t value;
	ssize_t size;
	int fd;

	fd = sys_eventfd2(1, 0);
	ASSERT_GE(fd, 0);

	size = read(fd, &value, sizeof(int));
	EXPECT_EQ(size, -1);
	EXPECT_EQ(errno, EINVAL);

	size = read(fd, &value, sizeof(value));
	EXPECT_EQ(size, sizeof(value));
	EXPECT_EQ(value, 1);

	close(fd);
}


/*
 * If EFD_SEMAPHORE was not specified and the eventfd counter has a nonzero
 * value, then a read(2) returns 8 bytes containing that value, and the
 * counter's value is reset to zero.
 * If the eventfd counter is zero at the time of the call to read(2), then the
 * call fails with the error EAGAIN if the file descriptor has been made nonblocking.
 */
TEST(eventfd_check_read_with_nonsemaphore)
{
	uint64_t value;
	ssize_t size;
	int fd;
	int i;

	fd = sys_eventfd2(0, EFD_NONBLOCK);
	ASSERT_GE(fd, 0);

	value = 1;
	for (i = 0; i < EVENTFD_TEST_ITERATIONS; i++) {
		size = write(fd, &value, sizeof(value));
		EXPECT_EQ(size, sizeof(value));
	}

	size = read(fd, &value, sizeof(value));
	EXPECT_EQ(size, sizeof(uint64_t));
	EXPECT_EQ(value, EVENTFD_TEST_ITERATIONS);

	size = read(fd, &value, sizeof(value));
	EXPECT_EQ(size, -1);
	EXPECT_EQ(errno, EAGAIN);

	close(fd);
}

/*
 * If EFD_SEMAPHORE was specified and the eventfd counter has a nonzero value,
 * then a read(2) returns 8 bytes containing the value 1, and the counter's
 * value is decremented by 1.
 * If the eventfd counter is zero at the time of the call to read(2), then the
 * call fails with the error EAGAIN if the file descriptor has been made nonblocking.
 */
TEST(eventfd_check_read_with_semaphore)
{
	uint64_t value;
	ssize_t size;
	int fd;
	int i;

	fd = sys_eventfd2(0, EFD_SEMAPHORE|EFD_NONBLOCK);
	ASSERT_GE(fd, 0);

	value = 1;
	for (i = 0; i < EVENTFD_TEST_ITERATIONS; i++) {
		size = write(fd, &value, sizeof(value));
		EXPECT_EQ(size, sizeof(value));
	}

	for (i = 0; i < EVENTFD_TEST_ITERATIONS; i++) {
		size = read(fd, &value, sizeof(value));
		EXPECT_EQ(size, sizeof(value));
		EXPECT_EQ(value, 1);
	}

	size = read(fd, &value, sizeof(value));
	EXPECT_EQ(size, -1);
	EXPECT_EQ(errno, EAGAIN);

	close(fd);
}

/*
 * The default maximum is ULLONG_MAX, matching the original behaviour.
 */
TEST(eventfd_check_ioctl_get_maximum_default)
{
	uint64_t max;
	int fd, ret;

	fd = sys_eventfd2(0, EFD_NONBLOCK);
	ASSERT_GE(fd, 0);

	ret = ioctl(fd, EFD_IOC_GET_MAXIMUM, &max);
	EXPECT_EQ(ret, 0);
	EXPECT_EQ(max, UINT64_MAX);

	close(fd);
}

/*
 * EFD_IOC_SET_MAXIMUM and EFD_IOC_GET_MAXIMUM round-trip.
 */
TEST(eventfd_check_ioctl_set_get_maximum)
{
	uint64_t max;
	int fd, ret;

	fd = sys_eventfd2(0, EFD_NONBLOCK);
	ASSERT_GE(fd, 0);

	max = 1000;
	ret = ioctl(fd, EFD_IOC_SET_MAXIMUM, &max);
	EXPECT_EQ(ret, 0);

	max = 0;
	ret = ioctl(fd, EFD_IOC_GET_MAXIMUM, &max);
	EXPECT_EQ(ret, 0);
	EXPECT_EQ(max, 1000);

	close(fd);
}

/*
 * Setting a maximum that is less than or equal to the current counter
 * must fail with EINVAL.
 */
TEST(eventfd_check_ioctl_set_maximum_invalid)
{
	uint64_t value = 5, max;
	ssize_t size;
	int fd, ret;

	fd = sys_eventfd2(0, EFD_NONBLOCK);
	ASSERT_GE(fd, 0);

	/* write 5 into the counter */
	size = write(fd, &value, sizeof(value));
	EXPECT_EQ(size, (ssize_t)sizeof(value));

	/* setting maximum == count (5) must fail */
	max = 5;
	ret = ioctl(fd, EFD_IOC_SET_MAXIMUM, &max);
	EXPECT_EQ(ret, -1);
	EXPECT_EQ(errno, EINVAL);

	/* setting maximum < count (3 < 5) must also fail */
	max = 3;
	ret = ioctl(fd, EFD_IOC_SET_MAXIMUM, &max);
	EXPECT_EQ(ret, -1);
	EXPECT_EQ(errno, EINVAL);

	/* setting maximum > count (10 > 5) must succeed */
	max = 10;
	ret = ioctl(fd, EFD_IOC_SET_MAXIMUM, &max);
	EXPECT_EQ(ret, 0);

	close(fd);
}

/*
 * Writes that would push the counter to or beyond maximum must return
 * EAGAIN on a non-blocking fd.  After a read drains the counter the
 * write should succeed again.
 */
TEST(eventfd_check_ioctl_write_blocked_at_maximum)
{
	uint64_t value, max_val = 5;
	ssize_t size;
	int fd, ret;

	fd = sys_eventfd2(0, EFD_NONBLOCK);
	ASSERT_GE(fd, 0);

	ret = ioctl(fd, EFD_IOC_SET_MAXIMUM, &max_val);
	ASSERT_EQ(ret, 0);

	/* write 4 — counter becomes 4, one slot before maximum */
	value = 4;
	size = write(fd, &value, sizeof(value));
	EXPECT_EQ(size, (ssize_t)sizeof(value));

	/*
	 * Writing 1 more would reach maximum (4+1 == 5 == maximum), which
	 * is the overflow level.  The write must block, i.e. return EAGAIN
	 * in non-blocking mode.
	 */
	value = 1;
	size = write(fd, &value, sizeof(value));
	EXPECT_EQ(size, -1);
	EXPECT_EQ(errno, EAGAIN);

	/* drain the counter */
	size = read(fd, &value, sizeof(value));
	EXPECT_EQ(size, (ssize_t)sizeof(value));
	EXPECT_EQ(value, 4);

	/* now the write must succeed (counter was reset to 0) */
	value = 1;
	size = write(fd, &value, sizeof(value));
	EXPECT_EQ(size, (ssize_t)sizeof(value));

	close(fd);
}

/*
 * Verify that EPOLLOUT is correctly gated by the configured maximum:
 * it should be clear when count >= maximum - 1, and set again after a read.
 */
TEST(eventfd_check_ioctl_poll_epollout)
{
	struct epoll_event ev, events[2];
	uint64_t value, max_val = 5;
	ssize_t sz;
	int fd, epfd, nfds, ret;

	fd = sys_eventfd2(0, EFD_NONBLOCK);
	ASSERT_GE(fd, 0);

	epfd = epoll_create1(0);
	ASSERT_GE(epfd, 0);

	ev.events = EPOLLIN | EPOLLOUT | EPOLLERR;
	ev.data.fd = fd;
	ret = epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev);
	ASSERT_EQ(ret, 0);

	ret = ioctl(fd, EFD_IOC_SET_MAXIMUM, &max_val);
	ASSERT_EQ(ret, 0);

	/* fresh fd: EPOLLOUT must be set (count=0 < maximum-1=4) */
	nfds = epoll_wait(epfd, events, 2, 0);
	EXPECT_EQ(nfds, 1);
	EXPECT_TRUE(!!(events[0].events & EPOLLOUT));

	/* write 4 — count reaches maximum-1=4, EPOLLOUT must clear */
	value = 4;
	sz = write(fd, &value, sizeof(value));
	EXPECT_EQ(sz, (ssize_t)sizeof(value));

	nfds = epoll_wait(epfd, events, 2, 0);
	EXPECT_EQ(nfds, 1);
	EXPECT_FALSE(!!(events[0].events & EPOLLOUT));
	EXPECT_TRUE(!!(events[0].events & EPOLLIN));

	/* drain counter — EPOLLOUT must reappear */
	sz = read(fd, &value, sizeof(value));
	EXPECT_EQ(sz, (ssize_t)sizeof(value));

	nfds = epoll_wait(epfd, events, 2, 0);
	EXPECT_EQ(nfds, 1);
	EXPECT_TRUE(!!(events[0].events & EPOLLOUT));

	close(epfd);
	close(fd);
}

/*
 * /proc/self/fdinfo must expose the configured maximum.
 */
TEST(eventfd_check_fdinfo_maximum)
{
	struct error err = {0};
	uint64_t max_val = 12345;
	int fd, ret;

	fd = sys_eventfd2(0, 0);
	ASSERT_GE(fd, 0);

	/* before setting: default should be ULLONG_MAX */
	ret = verify_fdinfo(fd, &err, "eventfd-maximum: ", 17,
			    "%16llx\n", (unsigned long long)UINT64_MAX);
	if (ret != 0)
		ksft_print_msg("eventfd-maximum default check failed: %s\n",
			       err.msg);
	EXPECT_EQ(ret, 0);

	ret = ioctl(fd, EFD_IOC_SET_MAXIMUM, &max_val);
	ASSERT_EQ(ret, 0);

	memset(&err, 0, sizeof(err));
	ret = verify_fdinfo(fd, &err, "eventfd-maximum: ", 17,
			    "%16llx\n", (unsigned long long)max_val);
	if (ret != 0)
		ksft_print_msg("eventfd-maximum after set check failed: %s\n",
			       err.msg);
	EXPECT_EQ(ret, 0);

	close(fd);
}

/*
 * An unrecognised ioctl must return ENOTTY (not EINVAL or ENOENT).
 */
TEST(eventfd_check_ioctl_unknown)
{
	int fd, ret;

	fd = sys_eventfd2(0, 0);
	ASSERT_GE(fd, 0);

	ret = ioctl(fd, _IO('J', 0xff));
	EXPECT_EQ(ret, -1);
	EXPECT_EQ(errno, ENOTTY);

	close(fd);
}

TEST_HARNESS_MAIN
