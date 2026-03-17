// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit helpers for backtrace suppression
 *
 * Copyright (C) 2025 Alessandro Carminati <acarmina@redhat.com>
 * Copyright (C) 2024 Guenter Roeck <linux@roeck-us.net>
 */

#include <kunit/bug.h>
#include <linux/atomic.h>
#include <linux/export.h>
#include <linux/instrumentation.h>
#include <linux/jump_label.h>
#include <linux/rculist.h>
#include <linux/string.h>

#ifdef CONFIG_KUNIT_SUPPRESS_BACKTRACE

static LIST_HEAD(suppressed_warnings);
static atomic_t suppressed_symbols_cnt = ATOMIC_INIT(0);

DEFINE_STATIC_KEY_FALSE(kunit_suppress_warnings_key);
EXPORT_SYMBOL_GPL(kunit_suppress_warnings_key);

void __kunit_start_suppress_warning(struct __suppressed_warning *warning)
{
	if (atomic_inc_return(&suppressed_symbols_cnt) == 1)
		static_branch_enable(&kunit_suppress_warnings_key);
	list_add_rcu(&warning->node, &suppressed_warnings);
}
EXPORT_SYMBOL_GPL(__kunit_start_suppress_warning);

void __kunit_end_suppress_warning(struct __suppressed_warning *warning)
{
	list_del_rcu(&warning->node);
	synchronize_rcu(); /* Wait for readers to finish */
	if (atomic_dec_return(&suppressed_symbols_cnt) == 0)
		static_branch_disable(&kunit_suppress_warnings_key);
}
EXPORT_SYMBOL_GPL(__kunit_end_suppress_warning);

static bool __kunit_check_suppress(const char *function)
{
	struct __suppressed_warning *warning;

	if (!function)
		return false;

	list_for_each_entry(warning, &suppressed_warnings, node) {
		if (!strcmp(function, warning->function)) {
			warning->counter++;
			return true;
		}
	}
	return false;
}

noinstr bool __kunit_is_suppressed_warning(const char *function)
{
	bool ret;

	if (!static_branch_unlikely(&kunit_suppress_warnings_key))
		return false;
	instrumentation_begin();
	ret = __kunit_check_suppress(function);
	instrumentation_end();
	return ret;
}
EXPORT_SYMBOL_GPL(__kunit_is_suppressed_warning);

#endif /* CONFIG_KUNIT_SUPPRESS_BACKTRACE */
