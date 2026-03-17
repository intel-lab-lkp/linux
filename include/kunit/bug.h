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

#ifdef CONFIG_KUNIT_SUPPRESS_BACKTRACE

#include <linux/stringify.h>
#include <linux/types.h>

struct __suppressed_warning {
	struct list_head node;
	const char *function;
	int counter;
};

void __kunit_start_suppress_warning(struct __suppressed_warning *warning);
void __kunit_end_suppress_warning(struct __suppressed_warning *warning);
bool __kunit_is_suppressed_warning(const char *function);

#define KUNIT_DEFINE_SUPPRESSED_WARNING(func)	\
	struct __suppressed_warning __kunit_suppress_##func = \
		{ .function = __stringify(func), .counter = 0 }

#define KUNIT_START_SUPPRESSED_WARNING(func) \
	__kunit_start_suppress_warning(&__kunit_suppress_##func)

#define KUNIT_END_SUPPRESSED_WARNING(func) \
	__kunit_end_suppress_warning(&__kunit_suppress_##func)

#define KUNIT_IS_SUPPRESSED_WARNING(func) \
	__kunit_is_suppressed_warning(func)

#define KUNIT_SUPPRESSED_WARNING_COUNT(func) \
	(__kunit_suppress_##func.counter)

#define KUNIT_SUPPRESSED_WARNING_COUNT_RESET(func) \
	__kunit_suppress_##func.counter = 0

#else /* CONFIG_KUNIT_SUPPRESS_BACKTRACE */

#define KUNIT_DEFINE_SUPPRESSED_WARNING(func)
#define KUNIT_START_SUPPRESSED_WARNING(func)
#define KUNIT_END_SUPPRESSED_WARNING(func)
#define KUNIT_IS_SUPPRESSED_WARNING(func) ((void)(func), false)
#define KUNIT_SUPPRESSED_WARNING_COUNT(func) ((void)(func), 0)
#define KUNIT_SUPPRESSED_WARNING_COUNT_RESET(func)

#endif /* CONFIG_KUNIT_SUPPRESS_BACKTRACE */
#endif /* __ASSEMBLY__ */
#endif /* _KUNIT_BUG_H */
