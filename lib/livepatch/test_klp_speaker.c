// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2023 SUSE

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#ifndef _VER_NAME
#define _VER_NAME(name) name
#endif

#include <linux/module.h>
#include <linux/printk.h>
#include <linux/delay.h>
#include <linux/sysfs.h>

#include "test_klp_speaker.h"

noinline
void _VER_NAME(speaker_welcome)(void)
{
	pr_info("%s: Hello, World!\n", __func__);
}

static int welcome_get(char *buffer, const struct kernel_param *kp)
{
	_VER_NAME(speaker_welcome)();

	return 0;
}

static const struct kernel_param_ops welcome_ops = {
	.get	= welcome_get,
};

module_param_cb(welcome, &welcome_ops, NULL, 0400);
MODULE_PARM_DESC(welcome, "Print speaker's welcome message into the kernel log when reading the value.");

noinline
void speaker_wait_and_welcome(struct speaker *speaker)
{
	pr_info("%s: Speaker started waiting.\n", __func__);
	complete(&speaker->started_waiting);
	speaker->started_waiting_param = true;

	while (READ_ONCE(speaker->is_waiting)) {
		/*
		 * Busy-wait until the sysfs writer has acknowledged a
		 * blocked transition and clears the flag.
		 */
		msleep(20);
	}

	speaker->welcome();
}

noinline
void _VER_NAME(call_speaker)(struct speaker *speaker)
{
	pr_info("%s: Calling speaker.\n", __func__);
	speaker->wait_and_welcome(speaker);
}

static struct speaker test_klp_speaker = {
	.call = _VER_NAME(call_speaker),
	.welcome = _VER_NAME(speaker_welcome),
	.wait_and_welcome = speaker_wait_and_welcome,
};

static void speaker_func(struct work_struct *work)
{
	struct speaker *speaker = container_of(work, struct speaker, work);

	speaker->call(speaker);
}

/*
 * The work must be initialized when "waiting_welcome" parameter is proceed
 * during the module load. Which is done before calling the module init
 * callback.
 *
 * Also it must be initialized also when the parameter was not used because
 * the work must be flushed in the module exit callback.
 */
static void speaker_work_init(struct speaker *speaker)
{
	static bool speaker_work_initialized;

	if (speaker_work_initialized)
		return;

	INIT_WORK(&speaker->work, speaker_func);
	speaker_work_initialized = true;
}

static int waiting_welcome_get(char *buffer, const struct kernel_param *kp)
{
	if (test_klp_speaker.is_waiting)
		pr_info("Speaker is waiting.\n");
	else
		pr_info("Speaker is not waiting.\n");

	return 0;
}

static int waiting_welcome_set(const char *val, const struct kernel_param *kp)
{
	bool wait;
	int ret;

	ret = kstrtobool(val, &wait);
	if (ret)
		return ret;

	if (wait) {
		if (test_klp_speaker.is_waiting) {
			pr_err("%s: Speaker is already waiting.\n", __func__);
			return -EBUSY;
		}

		test_klp_speaker.started_waiting_param = false;
		init_completion(&test_klp_speaker.started_waiting);
		speaker_work_init(&test_klp_speaker);

		WRITE_ONCE(test_klp_speaker.is_waiting, true);
		schedule_work(&test_klp_speaker.work);

		/*
		 * To synchronize kernel messages, hold this callback from
		 * exiting until the work function's entry message has printed.
		 */
		wait_for_completion(&test_klp_speaker.started_waiting);
	} else {
		if (!test_klp_speaker.is_waiting) {
			pr_err("%s: Speaker has not been waiting.\n", __func__);
			return -EINVAL;
		}

		WRITE_ONCE(test_klp_speaker.is_waiting, false);
		flush_work(&test_klp_speaker.work);
	}

	return 0;
}

static const struct kernel_param_ops waiting_welcome_ops = {
	.set	= waiting_welcome_set,
	.get	= waiting_welcome_get,
};

module_param_cb(waiting_welcome, &waiting_welcome_ops, NULL, 0600);
MODULE_PARM_DESC(waiting_welcome, "Speaker will start waiting when set and will say welcome message when cleared.");

static int started_waiting_get(char *buffer, const struct kernel_param *kp)
{
	return sysfs_emit(buffer, test_klp_speaker.started_waiting_param ? "1" : "0");
}

static const struct kernel_param_ops started_waiting_ops = {
	.get	= started_waiting_get,
};

module_param_cb(started_waiting, &started_waiting_ops, NULL, 0400);
MODULE_PARM_DESC(started_waiting, "Read only parameter to check whether the asynchronously started speaker already started waiting.");

static int test_klp_speaker_init(void)
{
	pr_info("%s\n", __func__);

	speaker_work_init(&test_klp_speaker);

	return 0;
}

static void test_klp_speaker_exit(void)
{
	pr_info("%s\n", __func__);

	/* Make sure that wait_funtion() is not running. */
	WRITE_ONCE(test_klp_speaker.is_waiting, false);
	flush_work(&test_klp_speaker.work);
}

module_init(test_klp_speaker_init);
module_exit(test_klp_speaker_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Livepatch test: test functions");
