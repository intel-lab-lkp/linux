/* SPDX-License-Identifier: GPL-2.0 */

/*
 * Copyright (C) 2023 Google LLC.
 */

#ifndef __LINUX_LSM_COUNT_H
#define __LINUX_LSM_COUNT_H

#include <linux/kconfig.h>

#ifdef CONFIG_SECURITY

/*
 * Macros to count the number of LSMs enabled in the kernel at compile time.
 */

/*
 * Capabilities is enabled when CONFIG_SECURITY is enabled.
 */
#if IS_ENABLED(CONFIG_SECURITY)
#define CAPABILITIES_ENABLED 1,
#else
#define CAPABILITIES_ENABLED
#endif

#if IS_ENABLED(CONFIG_SECURITY_SELINUX)
#define SELINUX_ENABLED 1,
#else
#define SELINUX_ENABLED
#endif

#if IS_ENABLED(CONFIG_SECURITY_SMACK)
#define SMACK_ENABLED 1,
#else
#define SMACK_ENABLED
#endif

#if IS_ENABLED(CONFIG_SECURITY_APPARMOR)
#define APPARMOR_ENABLED 1,
#else
#define APPARMOR_ENABLED
#endif

#if IS_ENABLED(CONFIG_SECURITY_TOMOYO)
#define TOMOYO_ENABLED 1,
#else
#define TOMOYO_ENABLED
#endif

#if IS_ENABLED(CONFIG_SECURITY_YAMA)
#define YAMA_ENABLED 1,
#else
#define YAMA_ENABLED
#endif

#if IS_ENABLED(CONFIG_SECURITY_LOADPIN)
#define LOADPIN_ENABLED 1,
#else
#define LOADPIN_ENABLED
#endif

#if IS_ENABLED(CONFIG_SECURITY_LOCKDOWN_LSM)
#define LOCKDOWN_ENABLED 1,
#else
#define LOCKDOWN_ENABLED
#endif

#if IS_ENABLED(CONFIG_BPF_LSM)
#define BPF_LSM_ENABLED 1,
#else
#define BPF_LSM_ENABLED
#endif

#if IS_ENABLED(CONFIG_SECURITY_LANDLOCK)
#define LANDLOCK_ENABLED 1,
#else
#define LANDLOCK_ENABLED
#endif


#define __COUNT_COMMAS(_0, _1, _2, _3, _4, _5, _6, _7, _8, _9, _10, _11, _12, _n, X...) _n
#define COUNT_COMMAS(a, X...) __COUNT_COMMAS(, ##X, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0)
#define ___COUNT_COMMAS(args...) COUNT_COMMAS(args)


#define MAX_LSM_COUNT			\
	___COUNT_COMMAS(		\
		CAPABILITIES_ENABLED	\
		SELINUX_ENABLED		\
		SMACK_ENABLED		\
		APPARMOR_ENABLED	\
		TOMOYO_ENABLED		\
		YAMA_ENABLED		\
		LOADPIN_ENABLED		\
		LOCKDOWN_ENABLED	\
		BPF_LSM_ENABLED		\
		LANDLOCK_ENABLED)

#else

#define MAX_LSM_COUNT 0

#endif /* CONFIG_SECURITY */

#endif  /* __LINUX_LSM_COUNT_H */
