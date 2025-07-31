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
#include <asm/siginfo.h>
#include "asm-signal.h"

typedef void __signalfn_t(int);
typedef __signalfn_t *__sighandler_t;

typedef void __restorefn_t(void);
typedef __restorefn_t *__sigrestore_t;

#define SIG_DFL ((__sighandler_t)0)     /* default signal handling */
#define SIG_IGN ((__sighandler_t)1)     /* ignore signal */
#define SIG_ERR ((__sighandler_t)-1)    /* error return from signal */

#if defined(__mips__)
struct sigaction {
        unsigned int    sa_flags;
        __sighandler_t  sa_handler;
        sigset_t        sa_mask;
};
#else
struct sigaction {
	__sighandler_t sa_handler;
	unsigned long sa_flags;
#if defined(SA_RESTORER) || defined(__sparc__)
	__sigrestore_t sa_restorer;
#endif
	sigset_t sa_mask;		/* mask last for extensibility */
};
#endif

typedef struct sigaltstack {
	void *ss_sp;
	int ss_flags;
	__kernel_size_t ss_size;
} stack_t;

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
#if defined(__sparc__)
/*
 * Sparc needs a restorer, which needs to be implemented in assembler and
 * passed as a separate argument.
 */

void __nolibc_sa_restorer(void);
void __nolibc_sa_restorer_wrapper(void);
void __attribute__((weak,noreturn)) __nolibc_entrypoint __no_stack_protector
__nolibc_sa_restorer_wrapper(void)
{
	/* The C function will have a prologue corrupting "sp" */
	__asm__  volatile (
		".section .text\n"
		".align 4\n"
		".type __nolibc_sa_restorer, @function\n"
		"__nolibc_sa_restorer:\n"
		"nop\n"
		"nop\n"
		"mov %0, %%g1 \n"
#ifdef __arch64__
		"t 0x6d\n"
#else
		"t 0x10\n"
#endif
		".size __nolibc_sa_restorer, .-__nolibc_sa_restorer\n"
		:: "n"(__NR_rt_sigreturn)
	);
	__nolibc_entrypoint_epilogue();
}

static __attribute__((unused))
int sys_rt_sigaction(int signum, const struct sigaction *act, struct sigaction *oldact)
{
	struct sigaction real_act = *act;

	/* Otherwise we would need to use sigreturn instead of rt_sigreturn */
	real_act.sa_flags |= SA_SIGINFO;

	return my_syscall5(__NR_rt_sigaction, signum, &real_act, oldact,
			   __nolibc_sa_restorer, sizeof(act->sa_mask));
}

#else
#if defined(__x86_64__) || defined(__i386_) || defined(__powerpc__)
static __no_stack_protector
void __nolibc_sa_restorer(void)
{
	my_syscall0(__NR_rt_sigreturn);
}
#endif

static __attribute__((unused))
int sys_rt_sigaction(int signum, const struct sigaction *act, struct sigaction *oldact)
{
	struct sigaction real_act = *act;
#if defined(__x86_64__) || defined(__i386_) || defined(__powerpc__)
	if (!(real_act.sa_flags & SA_RESTORER)) {
		real_act.sa_flags |= SA_RESTORER;
		real_act.sa_restorer = __nolibc_sa_restorer;
	}
#endif
#if defined(__i386__)
	/* Otherwise we would need to use sigreturn instead of rt_sigreturn */
	real_act.sa_flags |= SA_SIGINFO;
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
