/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _UAPI_LINUX_BINFMTS_H
#define _UAPI_LINUX_BINFMTS_H

#include <linux/capability.h>

struct pt_regs;

/*
 * These are the maximum length and maximum number of strings passed to the
 * execve() system call.  MAX_ARG_STRLEN is as large as Linux allows new
 * stack to grow.  Currently it's `_STK_LIM / 4 * 3 = 6MB` (see fs/exec.c).
 * MAX_ARG_STRINGS is chosen to fit in a signed 32-bit integer.
 */
#define MAX_ARG_STRLEN (6 * 1024 * 1024)
#define MAX_ARG_STRINGS 0x7FFFFFFF

/* sizeof(linux_binprm->buf) */
#define BINPRM_BUF_SIZE 256

/* preserve argv0 for the interpreter  */
#define AT_FLAGS_PRESERVE_ARGV0_BIT 0
#define AT_FLAGS_PRESERVE_ARGV0 (1 << AT_FLAGS_PRESERVE_ARGV0_BIT)

#endif /* _UAPI_LINUX_BINFMTS_H */
