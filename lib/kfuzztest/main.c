// SPDX-License-Identifier: GPL-2.0
/*
 * KFuzzTest core module initialization and debugfs interface.
 *
 * Copyright 2025 Google LLC
 */
#include <linux/err.h>
#include <linux/atomic.h>
#include <linux/debugfs.h>
#include <linux/fs.h>
#include <linux/kfuzztest.h>
#include <linux/module.h>
#include <linux/printk.h>
#include <linux/kasan.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Ethan Graham <ethangraham@google.com>");
MODULE_AUTHOR("Ethan Graham <ethan.w.s.graham@gmail.com>");
MODULE_DESCRIPTION("Kernel Fuzz Testing Framework (KFuzzTest)");

/*
 * Enforce a fixed struct size to ensure a consistent stride when iterating over
 * the array of these structs in the dedicated ELF section.
 */
static_assert(sizeof(struct kfuzztest_target) == 32, "struct kfuzztest_target should have size 32");
static_assert(sizeof(struct kfuzztest_simple_target) == 32, "struct kfuzztest_target should have size 32");
static_assert(sizeof(struct kfuzztest_constraint) == 64, "struct kfuzztest_constraint should have size 64");
static_assert(sizeof(struct kfuzztest_annotation) == 32, "struct kfuzztest_annotation should have size 32");

extern const struct kfuzztest_target __kfuzztest_targets_start[];
extern const struct kfuzztest_target __kfuzztest_targets_end[];
extern const struct kfuzztest_simple_target __kfuzztest_simple_targets_start[];
extern const struct kfuzztest_simple_target __kfuzztest_simple_targets_end[];

struct target_fops {
	struct file_operations target;
	struct file_operations target_simple;
};

/**
 * struct kfuzztest_state - global state for the KFuzzTest module
 *
 * @kfuzztest_dir: The root debugfs directory, /sys/kernel/debug/kfuzztest/.
 * @num_invocations: total number of target invocations.
 * @num_targets: number of registered targets.
 * @target_fops: array of file operations for each registered target.
 * @minalign_fops: file operations for the /_config/minalign file.
 * @num_invocations_fops: file operations for the /_config/num_invocations file.
 */
struct kfuzztest_state {
	struct dentry *kfuzztest_dir;
	atomic_t num_invocations;
	size_t num_targets;

	struct target_fops *target_fops;
	struct file_operations minalign_fops;
	struct file_operations num_invocations_fops;
};

static struct kfuzztest_state state;

void record_invocation(void)
{
	atomic_inc(&state.num_invocations);
}

static void cleanup_kfuzztest_state(struct kfuzztest_state *st)
{
	debugfs_remove_recursive(st->kfuzztest_dir);
	st->num_targets = 0;
	st->num_invocations = (atomic_t)ATOMIC_INIT(0);
	kfree(st->target_fops);
	st->target_fops = NULL;
}

static const umode_t KFUZZTEST_INPUT_PERMS = 0222;
static const umode_t KFUZZTEST_MINALIGN_PERMS = 0444;

static ssize_t read_cb_integer(struct file *filp, char __user *buf, size_t count, loff_t *f_pos, size_t value)
{
	char buffer[64];
	int len;

	len = scnprintf(buffer, sizeof(buffer), "%zu\n", value);
	return simple_read_from_buffer(buf, count, f_pos, buffer, len);
}

/*
 * Callback for /sys/kernel/debug/kfuzztest/_config/minalign. Minalign
 * corresponds to the minimum alignment that regions in a KFuzzTest input must
 * satisfy. This callback returns that value in string format.
 */
static ssize_t minalign_read_cb(struct file *filp, char __user *buf, size_t count, loff_t *f_pos)
{
	int minalign = MAX(KFUZZTEST_POISON_SIZE, ARCH_KMALLOC_MINALIGN);
	return read_cb_integer(filp, buf, count, f_pos, minalign);
}

/*
 * Callback for /sys/kernel/debug/kfuzztest/_config/num_invocations, which
 * returns the value in string format.
 */
static ssize_t num_invocations_read_cb(struct file *filp, char __user *buf, size_t count, loff_t *f_pos)
{
	return read_cb_integer(filp, buf, count, f_pos, atomic_read(&state.num_invocations));
}

static int create_read_only_file(struct dentry *parent, const char *name, struct file_operations *fops)
{
	struct dentry *file;
	int err = 0;

	file = debugfs_create_file(name, KFUZZTEST_MINALIGN_PERMS, parent, NULL, fops);
	if (!file)
		err = -ENOMEM;
	else if (IS_ERR(file))
		err = PTR_ERR(file);
	return err;
}

static int initialize_config_dir(struct kfuzztest_state *st)
{
	struct dentry *dir;
	int err = 0;

	dir = debugfs_create_dir("_config", st->kfuzztest_dir);
	if (!dir)
		err = -ENOMEM;
	else if (IS_ERR(dir))
		err = PTR_ERR(dir);
	if (err) {
		pr_info("kfuzztest: failed to create /_config dir");
		goto out;
	}

	st->minalign_fops = (struct file_operations){
		.owner = THIS_MODULE,
		.read = minalign_read_cb,
	};
	err = create_read_only_file(dir, "minalign", &st->minalign_fops);
	if (err) {
		pr_info("kfuzztest: failed to create /_config/minalign");
		goto out;
	}

	st->num_invocations_fops = (struct file_operations){
		.owner = THIS_MODULE,
		.read = num_invocations_read_cb,
	};
	err = create_read_only_file(dir, "num_invocations", &st->num_invocations_fops);
	if (err)
		pr_info("kfuzztest: failed to create /_config/num_invocations");
out:
	return err;
}

static int initialize_target_dir(struct kfuzztest_state *st, const struct kfuzztest_target *targ,
				 struct target_fops *fops)
{
	const struct kfuzztest_simple_target *simple_targ;
	struct dentry *dir, *input, *input_simple;
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

	input = debugfs_create_file("input", KFUZZTEST_INPUT_PERMS, dir, NULL, &fops->target);
	if (!input)
		err = -ENOMEM;
	else if (IS_ERR(input))
		err = PTR_ERR(input);
	if (err) {
		pr_info("kfuzztest: failed to create /kfuzztest/%s/input", targ->name);
		goto out;
	}

	/* Check if a simple target exists for this target. */
	for (simple_targ = __kfuzztest_simple_targets_start; simple_targ < __kfuzztest_simple_targets_end;
	     simple_targ++) {
		if (strcmp(targ->name, simple_targ->name) != 0)
			continue;
		fops->target_simple = (struct file_operations){
			.owner = THIS_MODULE,
			.write = simple_targ->write_input_cb,
		};
		input_simple =
			debugfs_create_file("input_simple", KFUZZTEST_INPUT_PERMS, dir, NULL, &fops->target_simple);
		if (!input_simple)
			err = -ENOMEM;
		else if (IS_ERR(input_simple))
			err = PTR_ERR(input_simple);
		if (err) {
			pr_info("kfuzztest: failed to create /kfuzztest/%s/input_simple", targ->name);
			goto out;
		}
		break;
	}
out:
	return err;
}

/**
 * kfuzztest_init - initializes the debug filesystem for KFuzzTest
 *
 * Each registered target in the ".kfuzztest_targets" section gets its own
 * subdirectory under "/sys/kernel/debug/kfuzztest/<test-name>" containing one
 * write-only "input" and optional "input_simple" files used for receiving
 * inputs from userspace.
 * Furthermore, a directory "/sys/kernel/debug/kfuzztest/_config" is created,
 * containing two read-only files "minalign" and "num_invocations", that return
 * the minimum required region alignment and number of successful target
 * invocations respectively.
 *
 * @return 0 on success or an error
 */
static int __init kfuzztest_init(void)
{
	const struct kfuzztest_target *targ;
	int err = 0;
	int i = 0;

	state.num_targets = __kfuzztest_targets_end - __kfuzztest_targets_start;
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

	err = initialize_config_dir(&state);
	if (err)
		goto cleanup_failure;

	for (targ = __kfuzztest_targets_start; targ < __kfuzztest_targets_end; targ++, i++) {
		state.target_fops[i].target = (struct file_operations){
			.owner = THIS_MODULE,
			.write = targ->write_input_cb,
		};
		err = initialize_target_dir(&state, targ, &state.target_fops[i]);
		/* Bail out if a single target fails to initialize. This avoids
		 * partial setup, and a failure here likely indicates an issue
		 * with debugfs. */
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
