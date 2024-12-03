// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2011 Richard Weinberger <richrd@nod.at>
 *
 * This vDSO turns all calls into a syscall so that UML can trap them.
 */


/* Disable profiling for userspace code */
#define DISABLE_BRANCH_PROFILING

#include <linux/time.h>
#include <linux/getcpu.h>
#include <asm/unistd.h>

/* workaround for -Wmissing-prototypes warnings */
int __vdso_clock_gettime(clockid_t clock, struct __kernel_old_timespec *ts);
int __vdso_gettimeofday(struct __kernel_old_timeval *tv, struct timezone *tz);
__kernel_old_time_t __vdso_time(__kernel_old_time_t *t);
long __vdso_getcpu(unsigned int *cpu, unsigned int *node, struct getcpu_cache *unused);

#ifdef CONFIG_MMU
#define __VDSO_SYSCALL1(sysnr, ret, a0)		\
	asm("syscall"				\
	    : "=a" (ret)			\
	    : "0" (sysnr), "D" (a0)		\
	    : "rcx", "r11", "memory")
#define __VDSO_SYSCALL2(sysnr, ret, a0, a1)		\
	asm("syscall"					\
	    : "=a" (ret)				\
	    : "0" (sysnr), "D" (a0), "S" (a1)		\
	    : "rcx", "r11", "memory")
#else
#define __VDSO_SYSCALL1(sysnr, ret, a0)		\
	asm("call *%%rax"				\
	    : "=a" (ret)			\
	    : "a" (sysnr), "D" (a0)	\
	    : "rcx", "r11", "memory")
#define __VDSO_SYSCALL2(sysnr, ret, a0, a1)		\
	asm("call *%%rax"					\
	    : "=a" (ret)				\
	    : "a" (sysnr), "D" (a0), "S" (a1)	\
	    : "rcx", "r11", "memory")
#endif

int __vdso_clock_gettime(clockid_t clock, struct __kernel_old_timespec *ts)
{
	long ret;

	__VDSO_SYSCALL2(__NR_clock_gettime, ret, clock, ts);
	return ret;
}
int clock_gettime(clockid_t, struct __kernel_old_timespec *)
	__attribute__((weak, alias("__vdso_clock_gettime")));

int __vdso_gettimeofday(struct __kernel_old_timeval *tv, struct timezone *tz)
{
	long ret;

	__VDSO_SYSCALL2(__NR_gettimeofday, ret, tv, tz);
	return ret;
}
int gettimeofday(struct __kernel_old_timeval *, struct timezone *)
	__attribute__((weak, alias("__vdso_gettimeofday")));

__kernel_old_time_t __vdso_time(__kernel_old_time_t *t)
{
	long secs;

	__VDSO_SYSCALL1(__NR_time, secs, t);
	return secs;
}
__kernel_old_time_t time(__kernel_old_time_t *t) __attribute__((weak, alias("__vdso_time")));

long
__vdso_getcpu(unsigned int *cpu, unsigned int *node, struct getcpu_cache *unused)
{
	/*
	 * UML does not support SMP, we can cheat here. :)
	 */

	if (cpu)
		*cpu = 0;
	if (node)
		*node = 0;

	return 0;
}

long getcpu(unsigned int *cpu, unsigned int *node, struct getcpu_cache *tcache)
	__attribute__((weak, alias("__vdso_getcpu")));
