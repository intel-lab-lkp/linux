// SPDX-License-Identifier: GPL-2.0
/*
 * KFuzzTest core module initialization and debugfs interface.
 *
 * Copyright 2025 Google LLC
 */
#include <linux/atomic.h>
#include <linux/debugfs.h>
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/kasan.h>
#include <linux/kfuzztest.h>
#include <linux/module.h>
#include <linux/printk.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Ethan Graham <ethan.w.s.graham@gmail.com>");
MODULE_DESCRIPTION("Kernel Fuzz Testing Framework (KFuzzTest)");

extern const struct kfuzztest_simple_target __kfuzztest_simple_targets_start[];
extern const struct kfuzztest_simple_target __kfuzztest_simple_targets_end[];

struct target_fops {
	struct file_operations target_simple;
};

/**
 * struct kfuzztest_state - global state for the KFuzzTest module
 *
 * @kfuzztest_dir: The root debugfs directory, /sys/kernel/debug/kfuzztest/.
 * @num_targets: number of registered targets.
 * @target_fops: array of file operations for each registered target.
 */
struct kfuzztest_state {
	struct dentry *kfuzztest_dir;
	struct target_fops *target_fops;
	size_t num_targets;
};

static struct kfuzztest_state state;

static void cleanup_kfuzztest_state(struct kfuzztest_state *st)
{
	debugfs_remove_recursive(st->kfuzztest_dir);
	st->num_targets = 0;
	kfree(st->target_fops);
	st->target_fops = NULL;
}

static const umode_t KFUZZTEST_INPUT_PERMS = 0222;

static int initialize_target_dir(struct kfuzztest_state *st, const struct kfuzztest_simple_target *targ,
				 struct target_fops *fops)
{
	struct dentry *dir, *input_simple;
	int err = 0;

	dir = debugfs_create_dir(targ->name, st->kfuzztest_dir);
	if (!dir)
		err = -ENOMEM;
	else if (IS_ERR(dir))
		err = PTR_ERR(dir);
	if (err) {
		pr_info("kfuzztest: failed to create /kfuzztest/%s dir", targ->name);
		goto out;
	}

	input_simple = debugfs_create_file("input_simple", KFUZZTEST_INPUT_PERMS, dir, NULL, &fops->target_simple);
	if (!input_simple)
		err = -ENOMEM;
	else if (IS_ERR(input_simple))
		err = PTR_ERR(input_simple);
	if (err)
		pr_info("kfuzztest: failed to create /kfuzztest/%s/input_simple", targ->name);
out:
	return err;
}

/**
 * kfuzztest_init - initializes the debug filesystem for KFuzzTest
 *
 * Each registered target in the ".kfuzztest_simple_target" section gets its own
 * subdirectory under "/sys/kernel/debug/kfuzztest/<test-name>" containing one
 * write-only "input_simple" file used for receiving binary inputs from
 * userspace.
 *
 * @return 0 on success or an error
 */
static int __init kfuzztest_init(void)
{
	const struct kfuzztest_simple_target *targ;
	int err = 0;
	int i = 0;

	state.num_targets = __kfuzztest_simple_targets_end - __kfuzztest_simple_targets_start;
	state.target_fops = kzalloc(sizeof(struct target_fops) * state.num_targets, GFP_KERNEL);
	if (!state.target_fops)
		return -ENOMEM;

	/* Create the main "kfuzztest" directory in /sys/kernel/debug. */
	state.kfuzztest_dir = debugfs_create_dir("kfuzztest", NULL);
	if (!state.kfuzztest_dir) {
		pr_warn("kfuzztest: could not create 'kfuzztest' debugfs directory");
		return -ENOMEM;
	}
	if (IS_ERR(state.kfuzztest_dir)) {
		pr_warn("kfuzztest: could not create 'kfuzztest' debugfs directory");
		err = PTR_ERR(state.kfuzztest_dir);
		state.kfuzztest_dir = NULL;
		return err;
	}

	for (targ = __kfuzztest_simple_targets_start; targ < __kfuzztest_simple_targets_end; targ++, i++) {
		state.target_fops[i].target_simple = (struct file_operations){
			.owner = THIS_MODULE,
			.write = targ->write_input_cb,
		};
		err = initialize_target_dir(&state, targ, &state.target_fops[i]);
		/*
		 * Bail out if a single target fails to initialize. This avoids
		 * partial setup, and a failure here likely indicates an issue
		 * with debugfs.
		 */
		if (err)
			goto cleanup_failure;
		pr_info("kfuzztest: registered target %s", targ->name);
	}
	return 0;

cleanup_failure:
	cleanup_kfuzztest_state(&state);
	return err;
}

static void __exit kfuzztest_exit(void)
{
	pr_info("kfuzztest: exiting");
	cleanup_kfuzztest_state(&state);
}

module_init(kfuzztest_init);
module_exit(kfuzztest_exit);
