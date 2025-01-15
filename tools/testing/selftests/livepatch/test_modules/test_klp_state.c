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
		pr_err("%s: failed to allocate shadow variable for the original loglevel\n",
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

/* Executed before patching, when the state is being enabled. */
static int pre_patch_callback(struct klp_patch *patch, struct klp_state *state)
{
	pr_info("%s: state %lu\n", __func__, state->id);
	return allocate_loglevel_state();
}

/* Executed after patching, when the state being enabled. */
static void post_patch_callback(struct klp_patch *patch, struct klp_state *state)
{
	pr_info("%s: state %lu\n", __func__, state->id);
	fix_console_loglevel();
}

/* Executed before unpatching, when the state is being disabled. */
static void pre_unpatch_callback(struct klp_patch *patch, struct klp_state *state)
{
	pr_info("%s: state %lu\n", __func__, state->id);
	restore_console_loglevel();
}

/*
 * Executed after unpatching, when the state is being disabled.
 *
 * The callback is not really needed. It is added just to check that
 * the optional callback is called at the right time.
 *
 * The shadow variable will be freed automatically because state->is_shadow
 * is set.
 */
static void post_unpatch_callback(struct klp_patch *patch, struct klp_state *state)
{
	pr_info("%s: state %lu (nope)\n", __func__, state->id);
}

/*
 * The shadow_dtor callback is not really needed. It is added just to show that
 * it is called automatically when disabling a klp_state with .is_shadow set.
 */
static void shadow_conosle_loglevel_dtor(void *obj, void *shadow_data)
{
	pr_info("%s: freeing space for the stored console_loglevel\n",
		__func__);
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
		.is_shadow = true,
		.callbacks = {
			.pre_patch = pre_patch_callback,
			.post_patch = post_patch_callback,
			.pre_unpatch = pre_unpatch_callback,
			.post_unpatch = post_unpatch_callback,
			.shadow_dtor = shadow_conosle_loglevel_dtor,
		},
	}, { }
};

static struct klp_patch patch = {
	.mod = THIS_MODULE,
	.objs = objs,
	.states = states,
	.replace = true,
};

static bool state_block_disable;

module_param(state_block_disable, bool, 0600);
MODULE_PARM_DESC(state_block_disable, "Set to 1 to pretend that the livepatch is not capable of disabling the state (default = 0).");

static bool no_state;

module_param(no_state, bool, 0600);
MODULE_PARM_DESC(no_state, "Set to 1 when the livepatch should not support the state (default = 0).");

static int test_klp_callbacks_demo_init(void)
{
	states[0].block_disable = state_block_disable;

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
