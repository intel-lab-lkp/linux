/*
 * Copyright (c) 2017 Oracle and/or its affiliates. All rights reserved.
 */

#ifndef _ASM_SPARC_VDSO_H
#define _ASM_SPARC_VDSO_H

#include <linux/linkage.h>
#include <linux/time.h>
#include <linux/time_types.h>
#include <linux/types.h>

notrace int __vdso_clock_gettime(clockid_t clock, struct __kernel_old_timespec *ts);
notrace int __vdso_clock_gettime_stick(clockid_t clock, struct __kernel_old_timespec *ts);
notrace int __vdso_gettimeofday(struct __kernel_old_timeval *tv, struct timezone *tz);
notrace int __vdso_gettimeofday_stick(struct __kernel_old_timeval *tv, struct timezone *tz);

struct vdso_image {
	void *data;
	unsigned long size;   /* Always a multiple of PAGE_SIZE */

	long sym_vvar_start;  /* Negative offset to the vvar area */
};

#ifdef CONFIG_SPARC64
extern const struct vdso_image vdso_image_64_builtin;
#endif
#ifdef CONFIG_COMPAT
extern const struct vdso_image vdso_image_32_builtin;
#endif

#endif /* _ASM_SPARC_VDSO_H */
