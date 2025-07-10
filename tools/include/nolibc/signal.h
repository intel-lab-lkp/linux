/* SPDX-License-Identifier: LGPL-2.1 OR MIT */
/*
 * signal function definitions for NOLIBC
 * Copyright (C) 2017-2022 Willy Tarreau <w@1wt.eu>
 */

/* make sure to include all global symbols */
#include "nolibc.h"

#ifndef _NOLIBC_SIGNAL_H
#define _NOLIBC_SIGNAL_H

#include "std.h"
#include "arch.h"
#include "types.h"
#include "sys.h"
#include "string.h"
/* other signal definitions are included by arch.h */

/* The kernel headers do not provide a sig_atomic_t definition */
#ifndef __sig_atomic_t_defined
#define __sig_atomic_t_defined 1
typedef int sig_atomic_t;
#endif

/* This one is not marked static as it's needed by libgcc for divide by zero */
int raise(int signal);
__attribute__((weak,unused,section(".text.nolibc_raise")))
int raise(int signal)
{
	return sys_kill(sys_getpid(), signal);
}

/*
 * sigaction(int signum, const struct sigaction *act, struct sigaction *oldact)
 */
#if defined(_NOLIBC_ARCH_NEEDS_SA_RESTORER) && !defined(__nolibc_sa_restorer)
static __no_stack_protector
void __nolibc_sa_restorer(void)
{
	my_syscall0(__NR_rt_sigreturn);
}
#endif

#ifndef sys_rt_sigaction
static __attribute__((unused))
int sys_rt_sigaction(int signum, const struct sigaction *act, struct sigaction *oldact)
{
	struct sigaction real_act = *act;
#if defined(_NOLIBC_ARCH_NEEDS_SA_RESTORER)
	if (!(real_act.sa_flags & SA_RESTORER)) {
		real_act.sa_flags |= SA_RESTORER;
		real_act.sa_restorer = __nolibc_sa_restorer;
	}
#endif
#ifdef _NOLIBC_ARCH_FORCE_SIG_FLAGS
	real_act.sa_flags |= _NOLIBC_ARCH_FORCE_SIG_FLAGS;
#endif

	return my_syscall4(__NR_rt_sigaction, signum, &real_act, oldact,
			   sizeof(act->sa_mask));
}
#endif

static __attribute__((unused))
int sigaction(int signum, const struct sigaction *act, struct sigaction *oldact)
{
	return __sysret(sys_rt_sigaction(signum, act, oldact));
}

/*
 * int sigemptyset(sigset_t *set)
 */
static __attribute__((unused))
int sigemptyset(sigset_t *set)
{
	__NOLIBC_BITMASK_ZERO(set->sig);
	return 0;
}

/*
 * int sigfillset(sigset_t *set)
 */
static __attribute__((unused))
int sigfillset(sigset_t *set)
{
	__NOLIBC_BITMASK_FILL(set->sig);
	return 0;
}

/*
 * int sigaddset(sigset_t *set, int signum)
 */
static __attribute__((unused))
int sigaddset(sigset_t *set, int signum)
{
	if (signum < 1 || signum > _NSIG)
		return __sysret(-EINVAL);

	__NOLIBC_BITMASK_SET(signum - 1, set->sig);
	return 0;
}

/*
 * int sigdelset(sigset_t *set, int signum)
 */
static __attribute__((unused))
int sigdelset(sigset_t *set, int signum)
{
	if (signum < 1 || signum > _NSIG)
		return __sysret(-EINVAL);

	__NOLIBC_BITMASK_CLEAR(signum - 1, set->sig);
	return 0;
}

/*
 * int sigismember(sigset_t *set, int signum)
 */
static __attribute__((unused))
int sigismember(sigset_t *set, int signum)
{
	if (signum < 1 || signum > _NSIG)
		return __sysret(-EINVAL);

	return __NOLIBC_BITMASK_TEST(signum - 1, set->sig);
}

#endif /* _NOLIBC_SIGNAL_H */
