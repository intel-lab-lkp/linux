// SPDX-License-Identifier: GPL-2.0+
/*
 * DEPT unit test
 *
 * Started by Byungchul Park <max.byungchul.park@gmail.com>:
 *
 *  Copyright (c) 2025 SK hynix, Inc., Byungchul Park
 */

#ifndef __LINUX_DEPT_UNIT_TEST_H
#define __LINUX_DEPT_UNIT_TEST_H

#if defined(CONFIG_DEPT_UNIT_TEST) || defined(CONFIG_DEPT_UNIT_TEST_MODULE)
struct dept_ut {
	bool circle_detected;
	bool recover_circle_detected;

	int ecxt_stack_total_cnt;
	int wait_stack_total_cnt;
	int evnt_stack_total_cnt;
	int ecxt_stack_valid_cnt;
	int wait_stack_valid_cnt;
	int evnt_stack_valid_cnt;
};

extern struct dept_ut dept_ut_results;

static inline void dept_ut_circle_detect(void)
{
	dept_ut_results.circle_detected = true;
}
static inline void dept_ut_recover_circle_detect(void)
{
	dept_ut_results.recover_circle_detected = true;
}
static inline void dept_ut_ecxt_stack_account(bool valid)
{
	dept_ut_results.ecxt_stack_total_cnt++;

	if (valid)
		dept_ut_results.ecxt_stack_valid_cnt++;
}
static inline void dept_ut_wait_stack_account(bool valid)
{
	dept_ut_results.wait_stack_total_cnt++;

	if (valid)
		dept_ut_results.wait_stack_valid_cnt++;
}
static inline void dept_ut_evnt_stack_account(bool valid)
{
	dept_ut_results.evnt_stack_total_cnt++;

	if (valid)
		dept_ut_results.evnt_stack_valid_cnt++;
}
#else
struct dept_ut {};

#define dept_ut_circle_detect() do { } while (0)
#define dept_ut_recover_circle_detect() do { } while (0)
#define dept_ut_ecxt_stack_account(v) do { } while (0)
#define dept_ut_wait_stack_account(v) do { } while (0)
#define dept_ut_evnt_stack_account(v) do { } while (0)

#endif
#endif /* __LINUX_DEPT_UNIT_TEST_H */
