/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * include/asm-generic/seccomp.h
 *
 * Copyright (C) 2014 Linaro Limited
 * Author: AKASHI Takahiro <takahiro.akashi@linaro.org>
 */
#ifndef _ASM_GENERIC_SECCOMP_H
#define _ASM_GENERIC_SECCOMP_H

#include <linux/unistd.h>

#if defined(CONFIG_COMPAT) && !defined(__NR_seccomp_read_32)
#define __NR_seccomp_read_32		__NR_read
#define __NR_seccomp_write_32		__NR_write
#define __NR_seccomp_exit_32		__NR_exit
#ifndef __NR_seccomp_sigreturn_32
#define __NR_seccomp_sigreturn_32	__NR_rt_sigreturn
#endif
#endif /* CONFIG_COMPAT && ! already defined */

#define __NR_seccomp_read		__NR_read
#define __NR_seccomp_write		__NR_write
#define __NR_seccomp_exit		__NR_exit
#ifndef __NR_seccomp_sigreturn
#define __NR_seccomp_sigreturn		__NR_rt_sigreturn
#endif
#if defined(CONFIG_COMPAT) && !defined(__NR_seccomp_rt_sigreturn_32)
#define __NR_seccomp_rt_sigreturn_32	__NR_seccomp_sigreturn_32
#endif
#ifndef __NR_seccomp_rt_sigreturn
#define __NR_seccomp_rt_sigreturn	__NR_seccomp_sigreturn
#endif

#ifndef __NR_seccomp_clone
#define __NR_seccomp_clone		__NR_clone
#endif
#ifndef __NR_seccomp_clone3
#ifdef __NR_clone3
#define __NR_seccomp_clone3		__NR_clone3
#else
#define __NR_seccomp_clone3		(-1)
#endif
#endif
#ifndef __NR_seccomp_fork
#ifdef __NR_fork
#define __NR_seccomp_fork		__NR_fork
#else
#define __NR_seccomp_fork		(-1)
#endif
#endif
#ifndef __NR_seccomp_vfork
#ifdef __NR_vfork
#define __NR_seccomp_vfork		__NR_vfork
#else
#define __NR_seccomp_vfork		(-1)
#endif
#endif
#ifdef CONFIG_COMPAT
#ifndef __NR_seccomp_clone_32
#define __NR_seccomp_clone_32		(-1)
#endif
#ifndef __NR_seccomp_clone3_32
#define __NR_seccomp_clone3_32		(-1)
#endif
#ifndef __NR_seccomp_fork_32
#define __NR_seccomp_fork_32		(-1)
#endif
#ifndef __NR_seccomp_vfork_32
#define __NR_seccomp_vfork_32		(-1)
#endif
#endif /* CONFIG_COMPAT */

#ifdef CONFIG_COMPAT
#ifndef get_compat_mode1_syscalls
static inline const int *get_compat_mode1_syscalls(void)
{
	static const int mode1_syscalls_32[] = {
		__NR_seccomp_read_32, __NR_seccomp_write_32,
		__NR_seccomp_exit_32, __NR_seccomp_sigreturn_32,
		-1, /* negative terminated */
	};
	return mode1_syscalls_32;
}
#endif
#endif /* CONFIG_COMPAT */

#endif /* _ASM_GENERIC_SECCOMP_H */
