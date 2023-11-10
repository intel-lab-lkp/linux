// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2019 SUSE

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/slab.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/printk.h>
#include <linux/livepatch.h>

#define CONSOLE_LOGLEVEL_FIX_ID 1

/*
 * Version of the state which defines compatibility of livepaches.
 * The value is artificial. It set just for testing the compatibility
 * checks. In reality, all versions are compatible because all
 * the callbacks do nothing and the shadow variable clean up
 * is done by the core.
 */
#ifndef CONSOLE_LOGLEVEL_FIX_VERSION
#define CONSOLE_LOGLEVEL_FIX_VERSION 1
#endif

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

static struct klp_func no_funcs[] = {
	{}
};

static struct klp_object objs[] = {
	{
		.name = NULL,	/* vmlinux */
		.funcs = no_funcs,
	}, { }
};

static struct klp_state states[] = {
	{
		.id = CONSOLE_LOGLEVEL_FIX_ID,
		.version = CONSOLE_LOGLEVEL_FIX_VERSION,
		.callbacks = {
			.setup = setup_state_callback,
			.enable = enable_state_callback,
			.disable = disable_state_callback,
			.release = release_state_callback,
		},
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
