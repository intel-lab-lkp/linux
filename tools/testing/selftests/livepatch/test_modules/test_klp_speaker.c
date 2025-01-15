// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2024 SUSE

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/printk.h>
#include <linux/delay.h>
#include <linux/sysfs.h>
#include <linux/completion.h>

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
 *	block the module load.
 */

noinline
static void speaker_welcome(void)
{
	pr_info("%s%s: Hello, World!\n", __func__, SPEAKER_ID);
}

static int welcome_get(char *buffer, const struct kernel_param *kp)
{
	speaker_welcome();

	return 0;
}

static const struct kernel_param_ops welcome_ops = {
	.get	= welcome_get,
};

module_param_cb(welcome, &welcome_ops, NULL, 0400);
MODULE_PARM_DESC(welcome, "Print speaker's welcome message into the kernel log when reading the value.");

static DECLARE_COMPLETION(started_blocking_doors);
struct work_struct block_doors_work;
static bool block_doors;
static bool show_over;

noinline
static void do_block_doors(void)
{
	pr_info("%s: Started blocking doors.\n", __func__);
	complete(&started_blocking_doors);

	while (!READ_ONCE(show_over)) {
		/* Busy-wait until the module gets unloaded. */
		msleep(20);
	}
}

/*
 * Prevent tail call optimizations to make sure that this function
 * appears in the backtrace and blocks the transition.
 */
__attribute__((__optimize__("no-optimize-sibling-calls")))
static void block_doors_func(struct work_struct *work)
{
	pr_info("%s: Going to block doors%s.\n", __func__, SPEAKER_ID);
	do_block_doors();
}

static void block_doors_set(void)
{
	init_completion(&started_blocking_doors);
	INIT_WORK(&block_doors_work, block_doors_func);

	schedule_work(&block_doors_work);

	/*
	 * To synchronize kernel messages, hold this callback from
	 * exiting until the work function's entry message has got printed.
	 */
	wait_for_completion(&started_blocking_doors);

}

module_param(block_doors, bool, 0400);
MODULE_PARM_DESC(block_doors, "Block doors so that the audience could not enter. It blocks the livepatch transition. (default=false)");

static int test_klp_speaker_init(void)
{
	pr_info("%s\n", __func__);

	if (block_doors)
		block_doors_set();

	return 0;
}

static void test_klp_speaker_exit(void)
{
	pr_info("%s\n", __func__);

	if (block_doors) {
		WRITE_ONCE(show_over, true);
		flush_work(&block_doors_work);
	}
}

module_init(test_klp_speaker_init);
module_exit(test_klp_speaker_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Livepatch test: test functions");
