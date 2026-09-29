// SPDX-License-Identifier: GPL-2.0
#include <linux/kernel.h>
#include <linux/rv.h>
#include <rv/kunit.h>
#include <trace/events/sched.h>
#include "throttle_kunit.h"

#if IS_REACHABLE(CONFIG_RV_MON_THROTTLE)

static void rv_test_throttle(struct kunit *test)
{
	struct task_struct *target = rv_kunit_alloc_mock_task(test);
	struct task_struct *other = rv_kunit_alloc_mock_task(test);
	struct rv_kunit_ctx *ctx = test->priv;

	prepare_test(test, &rv_throttle_ops.mon);

	target->pid = 99;
	target->policy = SCHED_DEADLINE;
	target->dl.runtime = 10000;
	target->dl.deadline = 20000;

	rv_throttle_ops.handle_newtask(NULL, target, 0);

	/* Task gets throttled on time but switched back in without replenish */
	rv_throttle_ops.handle_sched_switch(NULL, 0, other, target, TASK_RUNNING);
	rv_throttle_ops.handle_dl_replenish(NULL, &target->dl, 0, DL_TASK);
	udelay(9);
	rv_throttle_ops.handle_dl_throttle(NULL, &target->dl, 0, DL_TASK);
	RV_KUNIT_EXPECT_NO_REACTION(test, ctx);
	rv_throttle_ops.handle_sched_switch(NULL, 0, other, target, TASK_RUNNING);
	RV_KUNIT_EXPECT_REACTION(test, ctx);

	/* Task runs longer than runtime */
	rv_throttle_ops.handle_sched_switch(NULL, 0, other, target, TASK_RUNNING);
	rv_throttle_ops.handle_dl_replenish(NULL, &target->dl, 0, DL_TASK);
	udelay(10 + TICK_USEC);
	rv_throttle_ops.handle_dl_throttle(NULL, &target->dl, 0, DL_TASK);
	RV_KUNIT_EXPECT_REACTION(test, ctx);
}

#else
#define rv_test_throttle rv_test_stub
#endif
