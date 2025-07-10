/* SPDX-License-Identifier: LGPL-2.1 OR MIT */
/*
 * SPARC (32bit and 64bit) specific definitions for NOLIBC
 * Copyright (C) 2025 Thomas Weißschuh <linux@weissschuh.net>
 */

#ifndef _NOLIBC_ARCH_SPARC_H
#define _NOLIBC_ARCH_SPARC_H

#include <linux/unistd.h>

#include "compiler.h"
#include "crt.h"

/* The includes are sane, if one sets __WANT_POSIX1B_SIGNALS__ */
#define __WANT_POSIX1B_SIGNALS__
#include <linux/signal.h>

/*
 * Syscalls for SPARC:
 *   - registers are native word size
 *   - syscall number is passed in g1
 *   - arguments are in o0-o5
 *   - the system call is performed by calling a trap instruction
 *   - syscall return value is in o0
 *   - syscall error flag is in the carry bit of the processor status register
 */

#ifdef __arch64__

#define _NOLIBC_SYSCALL "t	0x6d\n"                                       \
			"bcs,a	%%xcc, 1f\n"                                  \
			"sub	%%g0, %%o0, %%o0\n"                           \
			"1:\n"

#else

#define _NOLIBC_SYSCALL "t	0x10\n"                                       \
			"bcs,a	1f\n"                                         \
			"sub	%%g0, %%o0, %%o0\n"                           \
			"1:\n"

#endif /* __arch64__ */

#define my_syscall0(num)                                                      \
({                                                                            \
	register long _num  __asm__ ("g1") = (num);                           \
	register long _arg1 __asm__ ("o0");                                   \
									      \
	__asm__ volatile (                                                    \
		_NOLIBC_SYSCALL                                               \
		: "+r"(_arg1)                                                 \
		: "r"(_num)                                                   \
		: "memory", "cc"                                              \
	);                                                                    \
	_arg1;                                                                \
})

#define my_syscall1(num, arg1)                                                \
({                                                                            \
	register long _num  __asm__ ("g1") = (num);                           \
	register long _arg1 __asm__ ("o0") = (long)(arg1);                    \
									      \
	__asm__ volatile (                                                    \
		_NOLIBC_SYSCALL                                               \
		: "+r"(_arg1)                                                 \
		: "r"(_num)                                                   \
		: "memory", "cc"                                              \
	);                                                                    \
	_arg1;                                                                \
})

#define my_syscall2(num, arg1, arg2)                                          \
({                                                                            \
	register long _num  __asm__ ("g1") = (num);                           \
	register long _arg1 __asm__ ("o0") = (long)(arg1);                    \
	register long _arg2 __asm__ ("o1") = (long)(arg2);                    \
									      \
	__asm__ volatile (                                                    \
		_NOLIBC_SYSCALL                                               \
		: "+r"(_arg1)                                                 \
		: "r"(_arg2), "r"(_num)                                       \
		: "memory", "cc"                                              \
	);                                                                    \
	_arg1;                                                                \
})

#define my_syscall3(num, arg1, arg2, arg3)                                    \
({                                                                            \
	register long _num  __asm__ ("g1") = (num);                           \
	register long _arg1 __asm__ ("o0") = (long)(arg1);                    \
	register long _arg2 __asm__ ("o1") = (long)(arg2);                    \
	register long _arg3 __asm__ ("o2") = (long)(arg3);                    \
									      \
	__asm__ volatile (                                                    \
		_NOLIBC_SYSCALL                                               \
		: "+r"(_arg1)                                                 \
		: "r"(_arg2), "r"(_arg3), "r"(_num)                           \
		: "memory", "cc"                                              \
	);                                                                    \
	_arg1;                                                                \
})

#define my_syscall4(num, arg1, arg2, arg3, arg4)                              \
({                                                                            \
	register long _num  __asm__ ("g1") = (num);                           \
	register long _arg1 __asm__ ("o0") = (long)(arg1);                    \
	register long _arg2 __asm__ ("o1") = (long)(arg2);                    \
	register long _arg3 __asm__ ("o2") = (long)(arg3);                    \
	register long _arg4 __asm__ ("o3") = (long)(arg4);                    \
									      \
	__asm__ volatile (                                                    \
		_NOLIBC_SYSCALL                                               \
		: "+r"(_arg1)                                                 \
		: "r"(_arg2), "r"(_arg3), "r"(_arg4), "r"(_num)               \
		: "memory", "cc"                                              \
	);                                                                    \
	_arg1;                                                                \
})

#define my_syscall5(num, arg1, arg2, arg3, arg4, arg5)                        \
({                                                                            \
	register long _num  __asm__ ("g1") = (num);                           \
	register long _arg1 __asm__ ("o0") = (long)(arg1);                    \
	register long _arg2 __asm__ ("o1") = (long)(arg2);                    \
	register long _arg3 __asm__ ("o2") = (long)(arg3);                    \
	register long _arg4 __asm__ ("o3") = (long)(arg4);                    \
	register long _arg5 __asm__ ("o4") = (long)(arg5);                    \
									      \
	__asm__ volatile (                                                    \
		_NOLIBC_SYSCALL                                               \
		: "+r"(_arg1)                                                 \
		: "r"(_arg2), "r"(_arg3), "r"(_arg4), "r"(_arg5), "r"(_num)   \
		: "memory", "cc"                                              \
	);                                                                    \
	_arg1;                                                                \
})

#define my_syscall6(num, arg1, arg2, arg3, arg4, arg5, arg6)                  \
({                                                                            \
	register long _num  __asm__ ("g1") = (num);                           \
	register long _arg1 __asm__ ("o0") = (long)(arg1);                    \
	register long _arg2 __asm__ ("o1") = (long)(arg2);                    \
	register long _arg3 __asm__ ("o2") = (long)(arg3);                    \
	register long _arg4 __asm__ ("o3") = (long)(arg4);                    \
	register long _arg5 __asm__ ("o4") = (long)(arg5);                    \
	register long _arg6 __asm__ ("o5") = (long)(arg6);                    \
									      \
	__asm__ volatile (                                                    \
		_NOLIBC_SYSCALL                                               \
		: "+r"(_arg1)                                                 \
		: "r"(_arg2), "r"(_arg3), "r"(_arg4), "r"(_arg5), "r"(_arg6), \
		  "r"(_num)                                                   \
		: "memory", "cc"                                              \
	);                                                                    \
	_arg1;                                                                \
})

/* startup code */
void __attribute__((weak, noreturn)) __nolibc_entrypoint __no_stack_protector _start(void)
{
	__asm__ volatile (
		/*
		 * Save argc pointer to o0, as arg1 of _start_c.
		 * Account for the window save area, which is 16 registers wide.
		 */
#ifdef __arch64__
		"add %sp, 128 + 2047, %o0\n" /* on sparc64 / v9 the stack is offset by 2047 */
#else
		"add %sp, 64, %o0\n"
#endif
		"b,a _start_c\n"     /* transfer to c runtime */
	);
	__nolibc_entrypoint_epilogue();
}

static pid_t getpid(void);

static __attribute__((unused))
pid_t sys_fork(void)
{
	pid_t parent, ret;

	parent = getpid();
	ret = my_syscall0(__NR_fork);

	/* The syscall returns the parent pid in the child instead of 0 */
	if (ret == parent)
		return 0;
	else
		return ret;
}
#define sys_fork sys_fork

static __attribute__((unused))
pid_t sys_vfork(void)
{
	pid_t parent, ret;

	parent = getpid();
	ret = my_syscall0(__NR_vfork);

	/* The syscall returns the parent pid in the child instead of 0 */
	if (ret == parent)
		return 0;
	else
		return ret;
}
#define sys_vfork sys_vfork

#define __nolibc_sa_restorer __nolibc_sa_restorer
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

/*
 * sparc has ODD_RT_SIGACTION, we need to pass the restorer as an argument
 * to rt_sigaction.
 */
#define sys_rt_sigaction sys_rt_sigaction
static __attribute__((unused))
int sys_rt_sigaction(int signum, const struct sigaction *act, struct sigaction *oldact)
{
	struct sigaction real_act = *act;

	/* Otherwise we would need to use sigreturn instead of rt_sigreturn */
	real_act.sa_flags |= SA_SIGINFO;

	return my_syscall5(__NR_rt_sigaction, signum, &real_act, oldact,
			   __nolibc_sa_restorer, sizeof(act->sa_mask));
}

#endif /* _NOLIBC_ARCH_SPARC_H */
