// SPDX-License-Identifier: GPL-2.0+
/*
 * DEPT unit test
 *
 * Started by Byungchul Park <max.byungchul.park@gmail.com>:
 *
 *  Copyright (c) 2025 SK hynix, Inc., Byungchul Park
 */

#include <linux/module.h>
#include <linux/spinlock.h>
#include <linux/mutex.h>
#include <linux/dept.h>
#include <linux/dept_unit_test.h>

MODULE_DESCRIPTION("DEPT unit test");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Byungchul Park <max.byungchul.park@sk.com>");

struct unit {
	const char *name;
	bool (*func)(void);
	bool result;
};

static DEFINE_SPINLOCK(s1);
static DEFINE_SPINLOCK(s2);
static bool test_spin_lock_deadlock(void)
{
	dept_ut_results.circle_detected = false;

	spin_lock(&s1);
	spin_lock(&s2);
	spin_unlock(&s2);
	spin_unlock(&s1);

	spin_lock(&s2);
	spin_lock(&s1);
	spin_unlock(&s1);
	spin_unlock(&s2);

	return dept_ut_results.circle_detected;
}

static DEFINE_MUTEX(m1);
static DEFINE_MUTEX(m2);
static bool test_mutex_lock_deadlock(void)
{
	dept_ut_results.circle_detected = false;

	mutex_lock(&m1);
	mutex_lock(&m2);
	mutex_unlock(&m2);
	mutex_unlock(&m1);

	mutex_lock(&m2);
	mutex_lock(&m1);
	mutex_unlock(&m1);
	mutex_unlock(&m2);

	return dept_ut_results.circle_detected;
}

static bool test_wait_event_deadlock(void)
{
	struct dept_map dmap1;
	struct dept_map dmap2;

	sdt_map_init(&dmap1);
	sdt_map_init(&dmap2);

	dept_ut_results.circle_detected = false;

	sdt_request_event(&dmap1); /* [S] */
	sdt_wait(&dmap2); /* [W] */
	sdt_event(&dmap1); /* [E] */

	sdt_request_event(&dmap2); /* [S] */
	sdt_wait(&dmap1); /* [W] */
	sdt_event(&dmap2); /* [E] */

	return dept_ut_results.circle_detected;
}

static void dummy_event(void)
{
	/* Do nothing. */
}

static DEFINE_DEPT_EVENT_SITE(es1);
static DEFINE_DEPT_EVENT_SITE(es2);
static bool test_recover_deadlock(void)
{
	dept_ut_results.recover_circle_detected = false;

	dept_recover_event(&es1, &es2);
	dept_recover_event(&es2, &es1);

	event_site(&es1, dummy_event);
	event_site(&es2, dummy_event);

	return dept_ut_results.recover_circle_detected;
}

static struct unit units[] = {
	{
		.name = "spin lock deadlock test",
		.func = test_spin_lock_deadlock,
	},
	{
		.name = "mutex lock deadlock test",
		.func = test_mutex_lock_deadlock,
	},
	{
		.name = "wait event deadlock test",
		.func = test_wait_event_deadlock,
	},
	{
		.name = "event recover deadlock test",
		.func = test_recover_deadlock,
	},
};

static int __init dept_ut_init(void)
{
	int i;

	lockdep_off();

	dept_ut_results.ecxt_stack_valid_cnt = 0;
	dept_ut_results.ecxt_stack_total_cnt = 0;
	dept_ut_results.wait_stack_valid_cnt = 0;
	dept_ut_results.wait_stack_total_cnt = 0;
	dept_ut_results.evnt_stack_valid_cnt = 0;
	dept_ut_results.evnt_stack_total_cnt = 0;

	for (i = 0; i < ARRAY_SIZE(units); i++)
		units[i].result = units[i].func();

	pr_info("\n");
	pr_info("******************************************\n");
	pr_info("DEPT unit test results\n");
	pr_info("******************************************\n");
	for (i = 0; i < ARRAY_SIZE(units); i++) {
		pr_info("(%s) %s\n", units[i].result ? "pass" : "fail",
				units[i].name);
	}
	pr_info("ecxt stack valid count = %d/%d\n",
			dept_ut_results.ecxt_stack_valid_cnt,
			dept_ut_results.ecxt_stack_total_cnt);
	pr_info("wait stack valid count = %d/%d\n",
			dept_ut_results.wait_stack_valid_cnt,
			dept_ut_results.wait_stack_total_cnt);
	pr_info("event stack valid count = %d/%d\n",
			dept_ut_results.evnt_stack_valid_cnt,
			dept_ut_results.evnt_stack_total_cnt);
	pr_info("******************************************\n");
	pr_info("\n");

	lockdep_on();

	return 0;
}

static void dept_ut_cleanup(void)
{
	/*
	 * Do nothing for now.
	 */
}

module_init(dept_ut_init);
module_exit(dept_ut_cleanup);
