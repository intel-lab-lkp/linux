// SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note
/*
 * vdso_clock_getres.c: Sample code to test clock_getres.
 * Copyright (c) 2019 Arm Ltd.
 *
 * Compile with:
 * gcc -std=gnu99 vdso_clock_getres.c
 *
 * Tested on ARM, ARM64, MIPS32, x86 (32-bit and 64-bit),
 * Power (32-bit and 64-bit), S390x (32-bit and 64-bit).
 * Might work on other architectures.
 */

#define _GNU_SOURCE
#include <elf.h>
#include <err.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <unistd.h>
#include <sys/syscall.h>

#include "../kselftest_harness.h"

static long syscall_clock_getres(clockid_t _clkid, struct timespec *_ts)
{
	long ret;

	ret = syscall(SYS_clock_getres, _clkid, _ts);

	return ret;
}

const char *vdso_clock_name[12] = {
	"CLOCK_REALTIME",
	"CLOCK_MONOTONIC",
	"CLOCK_PROCESS_CPUTIME_ID",
	"CLOCK_THREAD_CPUTIME_ID",
	"CLOCK_MONOTONIC_RAW",
	"CLOCK_REALTIME_COARSE",
	"CLOCK_MONOTONIC_COARSE",
	"CLOCK_BOOTTIME",
	"CLOCK_REALTIME_ALARM",
	"CLOCK_BOOTTIME_ALARM",
	"CLOCK_SGI_CYCLE",
	"CLOCK_TAI",
};

/*
 * This function calls clock_getres in vdso and by system call
 * with different values for clock_id.
 */
static inline void vdso_test_clock(struct __test_metadata *_metadata, unsigned int clock_id)
{
	struct timespec x, y;

	printf("clock_id: %s", vdso_clock_name[clock_id]);
	clock_getres(clock_id, &x);
	syscall_clock_getres(clock_id, &y);

	ASSERT_EQ(0, ((x.tv_sec != y.tv_sec) || (x.tv_nsec != y.tv_nsec)));
}

#if _POSIX_TIMERS > 0

#ifdef CLOCK_REALTIME
TEST(clock_realtime)
{
	vdso_test_clock(_metadata, CLOCK_REALTIME);
}
#endif

#ifdef CLOCK_BOOTTIME
TEST(clock_boottime)
{
	vdso_test_clock(_metadata, CLOCK_BOOTTIME);
}
#endif

#ifdef CLOCK_TAI
TEST(clock_tai)
{
	vdso_test_clock(_metadata, CLOCK_TAI);
}
#endif

#ifdef CLOCK_REALTIME_COARSE
TEST(clock_realtime_coarse)
{
	vdso_test_clock(_metadata, CLOCK_REALTIME_COARSE);
}
#endif

#ifdef CLOCK_MONOTONIC
TEST(clock_monotonic)
{
	vdso_test_clock(_metadata, CLOCK_MONOTONIC);
}
#endif

#ifdef CLOCK_MONOTONIC_RAW
TEST(clock_monotonic_raw)
{
	vdso_test_clock(_metadata, CLOCK_MONOTONIC_RAW);
}
#endif

#ifdef CLOCK_MONOTONIC_COARSE
TEST(clock_monotonic_coarse)
{
	vdso_test_clock(_metadata, CLOCK_MONOTONIC_COARSE);
}
#endif

#endif /* _POSIX_TIMERS > 0 */

TEST_HARNESS_MAIN
