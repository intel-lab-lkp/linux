/* SPDX-License-Identifier: GPL-2.0 */
/*
 * KUnit helpers for backtrace suppression
 *
 * Copyright (C) 2025 Alessandro Carminati <acarmina@redhat.com>
 * Copyright (C) 2024 Guenter Roeck <linux@roeck-us.net>
 */

#ifndef _KUNIT_BUG_H
#define _KUNIT_BUG_H

#ifndef __ASSEMBLY__

#include <linux/kconfig.h>

struct kunit;

#ifdef CONFIG_KUNIT_SUPPRESS_BACKTRACE

#include <linux/types.h>

struct task_struct;

struct __suppressed_warning {
	struct list_head node;
	struct task_struct *task;
	int counter;
};

struct __suppressed_warning *
__kunit_start_suppress_warning(struct kunit *test);
void __kunit_end_suppress_warning(struct kunit *test,
				  struct __suppressed_warning *warning);
int __kunit_suppressed_warning_count(struct __suppressed_warning *warning);
bool __kunit_is_suppressed_warning(void);

#define KUNIT_START_SUPPRESSED_WARNING(test) \
	struct __suppressed_warning *__kunit_suppress =	\
		__kunit_start_suppress_warning(test)

#define KUNIT_END_SUPPRESSED_WARNING(test) \
	__kunit_end_suppress_warning(test, __kunit_suppress)

#define KUNIT_SUPPRESSED_WARNING_COUNT() \
	__kunit_suppressed_warning_count(__kunit_suppress)

#else /* CONFIG_KUNIT_SUPPRESS_BACKTRACE */

#define KUNIT_START_SUPPRESSED_WARNING(test)
#define KUNIT_END_SUPPRESSED_WARNING(test)
#define KUNIT_SUPPRESSED_WARNING_COUNT() 0
static inline bool __kunit_is_suppressed_warning(void) { return false; }

#endif /* CONFIG_KUNIT_SUPPRESS_BACKTRACE */
#endif /* __ASSEMBLY__ */
#endif /* _KUNIT_BUG_H */
