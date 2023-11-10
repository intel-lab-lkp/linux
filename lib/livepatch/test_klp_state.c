// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2019 SUSE

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/slab.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/printk.h>
#include <linux/livepatch.h>

#define CONSOLE_LOGLEVEL_FIX_ID 1

static struct klp_patch patch;

static int allocate_loglevel_state(void)
{
	int *shadow_console_loglevel;

	/* Make sure that the shadow variable does not exist yet. */
	shadow_console_loglevel =
		klp_shadow_alloc(&console_loglevel, CONSOLE_LOGLEVEL_FIX_ID,
				 sizeof(*shadow_console_loglevel), GFP_KERNEL,
				 NULL, NULL);

	if (!shadow_console_loglevel) {
		pr_err("%s: failed to allocated shadow variable for storing original loglevel\n",
		       __func__);
		return -ENOMEM;
	}

	pr_info("%s: allocating space to store console_loglevel\n",
		__func__);

	return 0;
}

static void fix_console_loglevel(void)
{
	int *shadow_console_loglevel;

	shadow_console_loglevel =
		(int *)klp_shadow_get(&console_loglevel, CONSOLE_LOGLEVEL_FIX_ID);
	if (!shadow_console_loglevel)
		return;

	pr_info("%s: fixing console_loglevel\n", __func__);
	*shadow_console_loglevel = console_loglevel;
	console_loglevel = CONSOLE_LOGLEVEL_MOTORMOUTH;
}

static void restore_console_loglevel(void)
{
	int *shadow_console_loglevel;

	shadow_console_loglevel =
		(int *)klp_shadow_get(&console_loglevel, CONSOLE_LOGLEVEL_FIX_ID);
	if (!shadow_console_loglevel)
		return;

	pr_info("%s: restoring console_loglevel\n", __func__);
	console_loglevel = *shadow_console_loglevel;
}

static void free_loglevel_state(void)
{
	int *shadow_console_loglevel;

	shadow_console_loglevel =
		(int *)klp_shadow_get(&console_loglevel, CONSOLE_LOGLEVEL_FIX_ID);
	if (!shadow_console_loglevel)
		return;

	pr_info("%s: freeing space for the stored console_loglevel\n",
		__func__);
	klp_shadow_free(&console_loglevel, CONSOLE_LOGLEVEL_FIX_ID, NULL);
}

/* Executed before patching when the state is new. */
static int setup_state_callback(struct klp_patch *patch, struct klp_state *state)
{
	pr_info("%s: state %lu\n", __func__, state->id);
	return allocate_loglevel_state();
}

/* Executed after patching when the state is new. */
static void enable_state_callback(struct klp_patch *patch, struct klp_state *state)
{
	pr_info("%s: state %lu\n", __func__, state->id);
	fix_console_loglevel();
}

/* Executed before unpatching when the state is obsoleted. */
static void disable_state_callback(struct klp_patch *patch, struct klp_state *state)
{
	pr_info("%s: state %lu\n", __func__, state->id);
	restore_console_loglevel();
}

/* Executed after unpatching when the state is obsoleted. */
static void release_state_callback(struct klp_patch *patch, struct klp_state *state)
{
	pr_info("%s: state %lu\n", __func__, state->id);
	free_loglevel_state();
}

static struct klp_state states[] = {
	{
		.id = CONSOLE_LOGLEVEL_FIX_ID,
		.callbacks = {
			.setup = setup_state_callback,
			.enable = enable_state_callback,
			.disable = disable_state_callback,
			.release = release_state_callback,
		},
	}, { }
};

static int block_state_disable_get(char *buffer, const struct kernel_param *kp)
{
	pr_info("%s: Disable transition is %s by state: %lu\n",
		__func__,
		states[0].block_disable ? "not supported" : "supported",
		states[0].id);

	return 0;
}

static int block_state_disable_set(const char *val, const struct kernel_param *kp)
{
	bool block;
	int ret;

	ret = kstrtobool(val, &block);
	if (ret)
		return ret;

	states[0].block_disable = block;

	return 0;
}

static const struct kernel_param_ops block_state_disable_ops = {
	.get	= block_state_disable_get,
	.set	= block_state_disable_set,
};

module_param_cb(block_state_disable, &block_state_disable_ops, NULL, 0600);
MODULE_PARM_DESC(block_state_disable, "Set to 1 to pretend that the state does not support disable operation (default = 0).");

bool no_state;

static int no_state_get(char *buffer, const struct kernel_param *kp)
{
	return sysfs_emit("%s", no_state ? "1" : "0");
}

static int no_state_set(const char *val, const struct kernel_param *kp)
{
	bool no;
	int ret;

	ret = kstrtobool(val, &no);
	if (ret)
		return ret;

	no_state = no;

	return 0;
}

static const struct kernel_param_ops no_state_ops = {
	.get	= no_state_get,
	.set	= no_state_set,
};

module_param_cb(no_state, &no_state_ops, NULL, 0400);
MODULE_PARM_DESC(no_state, "Set to 1 when the livepatch should not support the state. (default = 0).");


static struct klp_func no_funcs[] = {
	{}
};

static struct klp_object objs[] = {
	{
		.name = NULL,	/* vmlinux */
		.funcs = no_funcs,
	}, { }
};

static struct klp_patch patch = {
	.mod = THIS_MODULE,
	.objs = objs,
	.states = states,
	.replace = true,
};

static int test_klp_callbacks_demo_init(void)
{
	if (no_state)
		patch.states = NULL;

	return klp_enable_patch(&patch);
}

static void test_klp_callbacks_demo_exit(void)
{
}

module_init(test_klp_callbacks_demo_init);
module_exit(test_klp_callbacks_demo_exit);
MODULE_LICENSE("GPL");
MODULE_INFO(livepatch, "Y");
MODULE_AUTHOR("Petr Mladek <pmladek@suse.com>");
MODULE_DESCRIPTION("Livepatch test: system state modification");
