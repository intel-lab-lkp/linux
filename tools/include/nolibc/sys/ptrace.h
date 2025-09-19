/* SPDX-License-Identifier: LGPL-2.1 OR MIT */
/*
 * ptrace for NOLIBC
 * Copyright (C) 2017-2021 Willy Tarreau <w@1wt.eu>
 * Copyright (C) 2025 Intel Corporation
 */

/* make sure to include all global symbols */
#include "../nolibc.h"

#ifndef _NOLIBC_SYS_PTRACE_H
#define _NOLIBC_SYS_PTRACE_H

#include "../sys.h"
#include "uio.h"


#include <linux/ptrace.h>

/*
 * long ptrace(int op, pid_t pid, void *addr, void *data);
 *
 * However, addr may also be an integer in some cases.
 */
static __attribute__((unused))
long sys_vptrace(int op, pid_t pid, va_list args)
{
	return my_syscall4(__NR_ptrace, op, pid,
			   va_arg(args, void *), va_arg(args, void *));
}

static __attribute__((unused))
ssize_t sys_ptrace(int op, pid_t pid, ...)
{
	va_list args;

	va_start(args, pid);
	return sys_vptrace(op, pid, args);
	va_end(args);
}

static __attribute__((unused))
ssize_t ptrace(int op, pid_t pid, ...)
{
	va_list args;

	va_start(args, pid);
	return __sysret(sys_vptrace(op, pid, args));
	va_end(args);
}

#endif /* _NOLIBC_SYS_PTRACE_H */
