/* SPDX-License-Identifier: LGPL-2.1 OR MIT */
/*
 * Alpha specific definitions for NOLIBC
 * Copyright (C) 2025 Thomas Weißschuh <linux@weissschuh.net>
 */

#ifndef _NOLIBC_ARCH_ALPHA_H
#define _NOLIBC_ARCH_ALPHA_H

#include "compiler.h"
#include "crt.h"

/*
 * Syscalls for Alpha:
 *   - registers are 64-bit
 *   - syscall number is passed in $0/v0
 *   - the system call is performed by calling callsys
 *   - syscall return comes in $0/v0, error flag in $19/a4
 *   - arguments are passed in $16/a0 to $21/a5
 *   - GCC does not support symbol register names
 */

#define my_syscall0(num)                                                      \
({                                                                            \
	register long _num __asm__ ("$0") = (num);                            \
	register long _ret __asm__ ("$0");                                    \
	register long _err __asm__ ("$19");                                   \
									      \
	__asm__ volatile (                                                    \
		"callsys"                                                     \
		: "=r"(_ret), "=r"(_err)                                      \
		: "r"(_num)                                                   \
		: "memory", "cc"                                              \
	);                                                                    \
	_err ? -_ret : _ret;                                                  \
})

#define my_syscall1(num, arg1)                                                \
({                                                                            \
	register long _num __asm__ ("$0") = (num);                            \
	register long _ret __asm__ ("$0");                                    \
	register long _err __asm__ ("$19");                                   \
	register long _arg1 __asm__ ("$16") = (long)(arg1);                   \
									      \
	__asm__ volatile (                                                    \
		"callsys"                                                     \
		: "=r"(_ret), "=r"(_err)                                      \
		: "r"(_num), "r"(_arg1)                                       \
		: "memory", "cc"                                              \
	);                                                                    \
	_err ? -_ret : _ret;                                                  \
})

#define my_syscall2(num, arg1, arg2)                                          \
({                                                                            \
	register long _num __asm__ ("$0") = (num);                            \
	register long _ret __asm__ ("$0");                                    \
	register long _err __asm__ ("$19");                                   \
	register long _arg1 __asm__ ("$16") = (long)(arg1);                   \
	register long _arg2 __asm__ ("$17") = (long)(arg2);                   \
									      \
	__asm__ volatile (                                                    \
		"callsys"                                                     \
		: "=r"(_ret), "=r"(_err)                                      \
		: "r"(_num), "r"(_arg1), "r"(_arg2)                           \
		: "memory", "cc"                                              \
	);                                                                    \
	_err ? -_ret : _ret;                                                  \
})

#define my_syscall3(num, arg1, arg2, arg3)                                    \
({                                                                            \
	register long _num __asm__ ("$0") = (num);                            \
	register long _ret __asm__ ("$0");                                    \
	register long _err __asm__ ("$19");                                   \
	register long _arg1 __asm__ ("$16") = (long)(arg1);                   \
	register long _arg2 __asm__ ("$17") = (long)(arg2);                   \
	register long _arg3 __asm__ ("$18") = (long)(arg3);                   \
									      \
	__asm__ volatile (                                                    \
		"callsys"                                                     \
		: "=r"(_ret), "=r"(_err)                                      \
		: "r"(_num), "r"(_arg1), "r"(_arg2), "r"(_arg3)               \
		: "memory", "cc"                                              \
	);                                                                    \
	_err ? -_ret : _ret;                                                  \
})

#define my_syscall4(num, arg1, arg2, arg3, arg4)                              \
({                                                                            \
	register long _num __asm__ ("$0") = (num);                            \
	register long _ret __asm__ ("$0");                                    \
	register long _err __asm__ ("$19");                                   \
	register long _arg1 __asm__ ("$16") = (long)(arg1);                   \
	register long _arg2 __asm__ ("$17") = (long)(arg2);                   \
	register long _arg3 __asm__ ("$18") = (long)(arg3);                   \
	register long _arg4 __asm__ ("$19") = (long)(arg4);                   \
									      \
	__asm__ volatile (                                                    \
		"callsys"                                                     \
		: "=r"(_ret), "=r"(_err)                                      \
		: "r"(_num), "r"(_arg1), "r"(_arg2), "r"(_arg3), "r"(_arg4)   \
		: "memory", "cc"                                              \
	);                                                                    \
	_err ? -_ret : _ret;                                                  \
})

#define my_syscall5(num, arg1, arg2, arg3, arg4, arg5)                        \
({                                                                            \
	register long _num __asm__ ("$0") = (num);                            \
	register long _ret __asm__ ("$0");                                    \
	register long _err __asm__ ("$19");                                   \
	register long _arg1 __asm__ ("$16") = (long)(arg1);                   \
	register long _arg2 __asm__ ("$17") = (long)(arg2);                   \
	register long _arg3 __asm__ ("$18") = (long)(arg3);                   \
	register long _arg4 __asm__ ("$19") = (long)(arg4);                   \
	register long _arg5 __asm__ ("$20") = (long)(arg5);                   \
									      \
	__asm__ volatile (                                                    \
		"callsys"                                                     \
		: "=r"(_ret), "=r"(_err)                                      \
		: "r"(_num), "r"(_arg1), "r"(_arg2), "r"(_arg3), "r"(_arg4),  \
		  "r"(_arg5)                                                  \
		: "memory", "cc"                                              \
	);                                                                    \
	_err ? -_ret : _ret;                                                  \
})

#define my_syscall6(num, arg1, arg2, arg3, arg4, arg5, arg6)                  \
({                                                                            \
	register long _num __asm__ ("$0") = (num);                            \
	register long _ret __asm__ ("$0");                                    \
	register long _err __asm__ ("$19");                                   \
	register long _arg1 __asm__ ("$16") = (long)(arg1);                   \
	register long _arg2 __asm__ ("$17") = (long)(arg2);                   \
	register long _arg3 __asm__ ("$18") = (long)(arg3);                   \
	register long _arg4 __asm__ ("$19") = (long)(arg4);                   \
	register long _arg5 __asm__ ("$20") = (long)(arg5);                   \
	register long _arg6 __asm__ ("$21") = (long)(arg6);                   \
									      \
	__asm__ volatile (                                                    \
		"callsys"                                                     \
		: "=r"(_ret), "=r"(_err)                                      \
		: "r"(_num), "r"(_arg1), "r"(_arg2), "r"(_arg3), "r"(_arg4),  \
		  "r"(_arg5), "r"(_arg6)                                      \
		: "memory", "cc"                                              \
	);                                                                    \
	_err ? -_ret : _ret;                                                  \
})

/* startup code */
void __attribute__((weak, noreturn)) __nolibc_entrypoint __no_stack_protector _start(void)
{
	__asm__ volatile (
		"br $gp, 0f\n"               /* setup $gp, so that 'lda' works                */
		"0: ldgp $gp, 0($gp)\n"
		"lda $27, _start_c\n"        /* setup current function address for _start_c   */
		"mov $sp, $16\n"             /* save argc pointer to $16, as arg1 of _start_c */
		"br  _start_c\n"             /* transfer to c runtime                         */
	);
	__nolibc_entrypoint_epilogue();
}

#endif /* _NOLIBC_ARCH_ALPHA_H */
