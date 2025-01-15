// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2024 SUSE

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/printk.h>
#include <linux/delay.h>
#include <linux/sysfs.h>
#include <linux/completion.h>

#include "test_klp_speaker.h"


#ifndef SPEAKER_ID
#define SPEAKER_ID ""
#endif

/**
 * test_klp_speaker - test module for testing misc livepatching features
 *
 * The module provides a virtual speaker who can do:
 *
 *    - Start a show with a greeting, see speaker_welcome().
 *
 *    - Log the greeting by reading the "welcome" module parameter, see
 *	welcome_get().
 *
 *    - Reuse the module source for more speakers, see SPEAKER_ID.
 *
 *    - Add "block_doors" parameter which could block the livepatch transition.
 *	The stalled function is offloaded to a workqueue so that it does not
 *	block the module load. The transition can be unblocked by setting
 *	the parameter value back to "0" via the sysfs interface.
 */

noinline
static void speaker_welcome(const char *context)
{
	pr_info("%s%s: Hello, World!%s\n", __func__, SPEAKER_ID, context);
}

static int welcome_get(char *buffer, const struct kernel_param *kp)
{
	speaker_welcome("");

	return 0;
}

static const struct kernel_param_ops welcome_ops = {
	.get	= welcome_get,
};

module_param_cb(welcome, &welcome_ops, NULL, 0400);
MODULE_PARM_DESC(welcome, "Print speaker's welcome message into the kernel log when reading the value.");

static DECLARE_COMPLETION(started_blocking_doors);
static bool block_doors;
static bool show_over;

noinline
static void do_block_doors(void)
{
	pr_info("%s: Started blocking doors.\n", __func__);
	complete(&started_blocking_doors);

	while (READ_ONCE(block_doors) && !READ_ONCE(show_over)) {
		/*
		 * Busy-wait until the parameter "block_doors" is cleared or
		 * until the module gets unloaded.
		 */
		msleep(20);
	}

	if (!block_doors) {
		pr_info("%s: Stopped blocking doors.\n", __func__);
		/*
		 * Show how the livepatched message looks in the process which
		 * blocked the transition.
		 */
		speaker_welcome(" <--- from blocked doors");
	}
}

static struct hall hall = {
	.do_block_doors = do_block_doors,
};

/*
 * Prevent tail call optimizations to make sure that this function
 * appears in the backtrace and blocks the transition.
 */
__attribute__((__optimize__("no-optimize-sibling-calls")))
static void block_doors_func(struct work_struct *work)
{
	struct hall *hall = container_of(work, struct hall, block_doors_work);

	pr_info("%s: Going to block doors%s.\n", __func__, SPEAKER_ID);
	hall->do_block_doors();
}

/*
 * The work must be initialized when "bool" parameter is proceed
 * during the module load. Which is done before calling the module init
 * callback.
 *
 * Also it must be initialized even when the parameter was not used because
 * the work must be flushed in the module exit callback.
 */
static void block_doors_work_init(struct hall *hall)
{
	static bool block_doors_work_initialized;

	if (block_doors_work_initialized)
		return;

	INIT_WORK(&hall->block_doors_work, block_doors_func);
	block_doors_work_initialized = true;
}

static int block_doors_get(char *buffer, const struct kernel_param *kp)
{
	if (block_doors)
		pr_info("The doors are blocked.\n");
	else
		pr_info("The doors are not blocked.\n");

	return 0;
}

static int block_doors_set(const char *val, const struct kernel_param *kp)
{
	bool block;
	int ret;

	ret = kstrtobool(val, &block);
	if (ret)
		return ret;

	if (block == block_doors) {
		if (block) {
			pr_err("%s: The doors are already blocked.\n", __func__);
			return -EBUSY;
		}

		pr_err("%s: The doors are not being blocked.\n", __func__);
		return -EINVAL;
	}

	/*
	 * Update the global value before scheduling the work so that it
	 * stays blocked.
	 */
	block_doors = block;
	if (block) {
		init_completion(&started_blocking_doors);
		block_doors_work_init(&hall);

		schedule_work(&hall.block_doors_work);

		/*
		 * To synchronize kernel messages, hold this callback from
		 * exiting until the work function's entry message has got
		 * printed.
		 */
		wait_for_completion(&started_blocking_doors);
	} else {
		flush_work(&hall.block_doors_work);
	}

	return 0;
}

static const struct kernel_param_ops block_doors_ops = {
	.set	= block_doors_set,
	.get	= block_doors_get,
};

module_param_cb(block_doors, &block_doors_ops, NULL, 0600);
MODULE_PARM_DESC(block_doors, "Block doors so that the audience could not enter. It blocks the livepatch transition. (default=false)");

static int test_klp_speaker_init(void)
{
	pr_info("%s\n", __func__);

	block_doors_work_init(&hall);

	return 0;
}

static void test_klp_speaker_exit(void)
{
	pr_info("%s\n", __func__);

	/* Make sure that do_block_doors() is not running. */
	WRITE_ONCE(show_over, true);
	flush_work(&hall.block_doors_work);
}

module_init(test_klp_speaker_init);
module_exit(test_klp_speaker_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Livepatch test: test functions");
