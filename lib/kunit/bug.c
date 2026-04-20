// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit helpers for backtrace suppression
 *
 * Copyright (C) 2025 Alessandro Carminati <acarmina@redhat.com>
 * Copyright (C) 2024 Guenter Roeck <linux@roeck-us.net>
 */

#include <kunit/bug.h>
#include <kunit/resource.h>
#include <linux/atomic.h>
#include <linux/export.h>
#include <linux/rculist.h>
#include <linux/sched.h>

#ifdef CONFIG_KUNIT_SUPPRESS_BACKTRACE

static LIST_HEAD(suppressed_warnings);
static atomic_t suppressed_warnings_cnt = ATOMIC_INIT(0);

static void __kunit_suppress_warning_remove(struct __suppressed_warning *warning)
{
	list_del_rcu(&warning->node);
	synchronize_rcu(); /* Wait for readers to finish */
	atomic_dec(&suppressed_warnings_cnt);
}

KUNIT_DEFINE_ACTION_WRAPPER(__kunit_suppress_warning_cleanup,
			    __kunit_suppress_warning_remove,
			    struct __suppressed_warning *);

struct __suppressed_warning *
__kunit_start_suppress_warning(struct kunit *test)
{
	struct __suppressed_warning *warning;
	int ret;

	warning = kunit_kzalloc(test, sizeof(*warning), GFP_KERNEL);
	if (!warning)
		return NULL;

	warning->task = current;
	atomic_inc(&suppressed_warnings_cnt);
	list_add_rcu(&warning->node, &suppressed_warnings);

	ret = kunit_add_action_or_reset(test,
					__kunit_suppress_warning_cleanup,
					warning);
	if (ret)
		return NULL;

	return warning;
}
EXPORT_SYMBOL_GPL(__kunit_start_suppress_warning);

void __kunit_end_suppress_warning(struct kunit *test,
				  struct __suppressed_warning *warning)
{
	if (!warning)
		return;
	kunit_release_action(test, __kunit_suppress_warning_cleanup, warning);
}
EXPORT_SYMBOL_GPL(__kunit_end_suppress_warning);

int __kunit_suppressed_warning_count(struct __suppressed_warning *warning)
{
	return warning ? warning->counter : 0;
}
EXPORT_SYMBOL_GPL(__kunit_suppressed_warning_count);

bool __kunit_is_suppressed_warning(void)
{
	struct __suppressed_warning *warning;

	if (!atomic_read(&suppressed_warnings_cnt))
		return false;

	rcu_read_lock();
	list_for_each_entry_rcu(warning, &suppressed_warnings, node) {
		if (warning->task == current) {
			warning->counter++;
			rcu_read_unlock();
			return true;
		}
	}
	rcu_read_unlock();

	return false;
}

#endif /* CONFIG_KUNIT_SUPPRESS_BACKTRACE */
