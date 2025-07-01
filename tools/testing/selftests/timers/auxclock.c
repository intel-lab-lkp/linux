// SPDX-License-Identifier: GPL-2.0

/* Work around type conflicts between libc and the UAPI headers */
#define _SYS_TIME_H
#define __timeval_defined
#define _GNU_SOURCE

#include <fcntl.h>
#include <linux/types.h>
#include <linux/timex.h>
#include <sched.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "../kselftest_harness.h"

#ifndef CLOCK_AUX
#define	CLOCK_AUX	16
#endif

#ifndef NSEC_PER_SEC
#define NSEC_PER_SEC 1000000000ULL
#endif

#define AUXCLOCK_SELFTEST_TIMENS_OFFSET 10000

static int configure_auxclock(__kernel_clockid_t clockid, bool enable)
{
	char path[100];
	int fd, ret;

	ret = snprintf(path, sizeof(path),
		       "/sys/kernel/time/aux_clocks/%d/aux_clock_enable",
		       (int)clockid - CLOCK_AUX);
	if (ret >= sizeof(path))
		return -ENOSPC;

	fd = open(path, O_WRONLY);
	if (fd == -1)
		return -errno;

	/* Always disable to reset */
	ret = dprintf(fd, "0\n");
	if (enable)
		ret = dprintf(fd, "1\n");
	close(fd);

	if (ret < 0)
		return ret;

	return 0;
}

/* Everything is done in terms of 64bit time values to keep the code readable */

static inline void timespec_to_kernel_timespec(const struct timespec *ts,
					       struct __kernel_timespec *kts)
{
	if (!kts)
		return;

	kts->tv_sec = ts->tv_sec;
	kts->tv_nsec = ts->tv_nsec;
}

static inline void kernel_timespec_to_timespec(const struct __kernel_timespec *kts,
					       struct timespec *ts)
{
	if (!kts)
		return;

	ts->tv_sec = kts->tv_sec;
	ts->tv_nsec = kts->tv_nsec;
}

static int sys_clock_getres_time64(__kernel_clockid_t clockid, struct __kernel_timespec *ts)
{
#if defined(__NR_clock_getres_time64)
	return syscall(__NR_clock_getres_time64, clockid, ts);
#elif defined(__NR_clock_getres)
	struct timespec _ts;
	int ret;

	ret = syscall(__NR_clock_getres, clockid, &_ts);
	if (!ret)
		timespec_to_kernel_timespec(&_ts, ts);
	return ret;
#else
#error "No clock_getres() support"
#endif
}

static int sys_clock_gettime64(__kernel_clockid_t clockid, struct __kernel_timespec *ts)
{
#if defined(__NR_clock_gettime64)
	return syscall(__NR_clock_gettime64, clockid, ts);
#elif defined(__NR_clock_gettime)
	struct timespec _ts;
	int ret;

	ret = syscall(__NR_clock_gettime, clockid, &_ts);
	if (!ret)
		timespec_to_kernel_timespec(&_ts, ts);
	return ret;
#else
#error "No clock_gettime() support"
#endif
}

static int sys_clock_settime64(__kernel_clockid_t clockid, const struct __kernel_timespec *ts)
{
#if defined(__NR_clock_settime64)
	return syscall(__NR_clock_settime64, clockid, ts);
#elif defined(__NR_clock_settime)
	struct timespec _ts;

	kernel_timespec_to_timespec(ts, &_ts);
	return syscall(__NR_clock_settime, clockid, &_ts);
#else
#error "No clock_settime() support"
#endif
}

static int sys_clock_adjtime64(__kernel_clockid_t clockid, struct __kernel_timex *tx)
{
#if defined(__NR_clock_adjtime64)
	return syscall(__NR_clock_adjtime64, clockid, tx);
#elif __LONG_WIDTH__ == 64 && defined(__NR_clock_adjtime)
	return syscall(__NR_clock_adjtime, clockid, tx);
#else
#error "No clock_adjtime() support"
#endif
}

FIXTURE(auxclock) {};

FIXTURE_VARIANT(auxclock) {
	__kernel_clockid_t clock;
	bool clock_enabled;
	bool use_timens;
};

FIXTURE_VARIANT_ADD(auxclock, default) {
	.clock		= CLOCK_AUX,
	.clock_enabled	= true,
	.use_timens	= false,
};

FIXTURE_VARIANT_ADD(auxclock, timens) {
	.clock		= CLOCK_AUX,
	.clock_enabled	= true,
	.use_timens	= true,
};

FIXTURE_VARIANT_ADD(auxclock, disabled) {
	.clock		= CLOCK_AUX,
	.clock_enabled	= false,
	.use_timens	= false,
};

/* No timens_disabled to keep the testmatrix smaller. */

static void enter_timens(struct __test_metadata *_metadata)
{
	int ret, fd;
	char buf[100];

	ret = unshare(CLONE_NEWTIME);
	if (ret != 0 && errno == EPERM)
		SKIP(return, "no permissions for unshare(CLONE_NEWTIME)");
	if (ret != 0 && errno == EINVAL)
		SKIP(return, "time namespaces not available");
	ASSERT_EQ(0, ret) TH_LOG("unshare(CLONE_NEWTIME) failed: %s", strerror(errno));
	fd = open("/proc/self/timens_offsets", O_WRONLY);
	if (fd == -1 && errno == ENOENT)
		SKIP(return, "no support for time namespaces");
	ASSERT_NE(-1, fd);
	/* Fiddle with the namespace to make the tests more meaningful */
	ret = snprintf(buf, sizeof(buf), "monotonic %d 0\nboottime %d 0\n",
		       AUXCLOCK_SELFTEST_TIMENS_OFFSET, AUXCLOCK_SELFTEST_TIMENS_OFFSET);
	ASSERT_TRUE(ret > 0 && ret < sizeof(buf));
	ret = write(fd, buf, ret);
	ASSERT_NE(-1, ret);
	close(fd);
	fd = open("/proc/self/ns/time_for_children", O_RDONLY);
	ASSERT_NE(-1, fd);
	ret = setns(fd, CLONE_NEWTIME);
	close(fd);
	ASSERT_EQ(0, ret);
}

FIXTURE_SETUP(auxclock) {
	int ret;

	ret = configure_auxclock(variant->clock, variant->clock_enabled);
	if (ret == -ENOENT)
		SKIP(return, "auxclocks not enabled");
	ASSERT_EQ(0, ret);

	if (variant->use_timens)
		enter_timens(_metadata);
}

FIXTURE_TEARDOWN(auxclock) {
	int ret;

	ret = configure_auxclock(variant->clock, false);
	ASSERT_EQ(0, ret);
}

TEST_F(auxclock, sys_clock_getres) {
	struct __kernel_timespec ts;
	int ret;

	/* clock_getres() is always expected to work */
	ret = sys_clock_getres_time64(variant->clock, &ts);
	ASSERT_EQ(0, ret);
	ASSERT_EQ(0, ts.tv_sec);
	ASSERT_EQ(1, ts.tv_nsec);
}

TEST_F(auxclock, sys_clock_gettime) {
	struct __kernel_timespec ts;
	int ret;

	ret = sys_clock_gettime64(variant->clock, &ts);
	if (variant->clock_enabled) {
		ASSERT_EQ(0, ret);
	} else {
		ASSERT_EQ(-1, ret);
		ASSERT_EQ(ENODEV, errno);
	}
}

static void auxclock_validate_progression(struct __test_metadata *_metadata,
					  const struct __kernel_timespec *a,
					  const struct __kernel_timespec *b)
{
	int64_t diff;

	diff = (b->tv_sec - a->tv_sec) * NSEC_PER_SEC;
	diff += b->tv_nsec - a->tv_nsec;

	/* Arbitrary values */
	ASSERT_LT(1, diff);
	ASSERT_GT(1 * NSEC_PER_SEC, diff);
}

TEST_F(auxclock, sys_clock_settime) {
	struct __kernel_timespec a, b = {};
	int ret;

	a.tv_sec = 1234;
	a.tv_nsec = 5678;

	ret = sys_clock_settime64(variant->clock, &a);
	if (!variant->clock_enabled) {
		ASSERT_EQ(-1, ret);
		ASSERT_EQ(ENODEV, errno);
		return;
	}

	ASSERT_EQ(0, ret);

	ret = sys_clock_gettime64(variant->clock, &b);
	ASSERT_EQ(0, ret);

	auxclock_validate_progression(_metadata, &a, &b);
}

TEST_F(auxclock, sys_clock_adjtime) {
	struct __kernel_timex tx;
	int ret, realtime_freq;

	memset(&tx, 0, sizeof(tx));
	tx.modes = ADJ_FREQUENCY;
	ret = sys_clock_adjtime64(CLOCK_REALTIME, &tx);
	ASSERT_NE(-1, ret);
	ASSERT_TRUE(tx.modes & ADJ_FREQUENCY);
	realtime_freq = tx.freq;

	ret = sys_clock_adjtime64(variant->clock, &tx);
	if (variant->clock_enabled) {
		ASSERT_NE(-1, ret);
		ASSERT_EQ(realtime_freq, tx.freq);
	} else {
		ASSERT_EQ(-1, ret);
		ASSERT_EQ(ENODEV, errno);
	}
}

TEST_F(auxclock, progression) {
	struct __kernel_timespec a, b;
	int ret;

	if (!variant->clock_enabled) {
		TH_LOG("no progression on disabled clocks");
		return;
	}

	/* set up reference */
	ret = sys_clock_gettime64(variant->clock, &a);
	ASSERT_EQ(0, ret);

	for (int i = 0; i < 100; i++) {
		memset(&b, 0, sizeof(b));
		ret = sys_clock_gettime64(variant->clock, &b);
		ASSERT_EQ(0, ret);
		auxclock_validate_progression(_metadata, &a, &b);

		memset(&a, 0, sizeof(a));
		ret = sys_clock_gettime64(variant->clock, &a);
		ASSERT_EQ(0, ret);
		auxclock_validate_progression(_metadata, &b, &a);
	}
}

TEST_HARNESS_MAIN
