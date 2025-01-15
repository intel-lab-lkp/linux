// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2024 SUSE

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/livepatch.h>
#include <linux/init.h>

#include "test_klp_speaker.h"

/**
 * test_klp_speaker_livepatch - test livepatch for testing various livepatching
 *	features.
 *
 * The livepatch modifies the behavior of a virtual speaker provided by
 * the module test_klp_speaker. It can do:
 *
 *    - Improve the speaker's greeting from "Hello, World!" to
 *	"Ladies and gentleman, ..."
 *
 *    - Support more speaker modules, see __lp_speaker_welcome().
 *
 *    - Livepatch block_doors_func() which can block the transition.
 */

#define APPLAUSE_ID 10
#define APPLAUSE_STR_SIZE 16

/* associate the shadow variable with NULL address */;
static void *shadow_object = NULL;

static bool add_applause;
module_param_named(applause, add_applause, bool, 0400);
MODULE_PARM_DESC(applause, "Use shadow variable to add applause (default=false)");

static int pre_patch_ret;
module_param(pre_patch_ret, int, 0400);
MODULE_PARM_DESC(pre_patch_ret, "Allow to force failure for the pre_patch callback (default=0)");

static void __lp_speaker_welcome(const char *caller_func,
				 const char *speaker_id,
				 const char *context)
{
	char entire_applause[APPLAUSE_STR_SIZE + 1] = "";
	const char *applause;

	applause = (char *)klp_shadow_get(shadow_object, APPLAUSE_ID);
	if (applause)
		snprintf(entire_applause, sizeof(entire_applause), "%s ", applause);

	pr_info("%s%s: %sLadies and gentleman, ...%s\n",
		caller_func, speaker_id, entire_applause, context);
}

static void lp_speaker_welcome(const char *context)
{
	__lp_speaker_welcome(__func__, "", context);
}

static void lp_speaker2_welcome(const char *context)
{
	__lp_speaker_welcome(__func__, "(2)", context);
}

static int allocate_applause(unsigned long id)
{
	char *applause;

	/*
	 * Attach the shadow variable to some well known address it stays
	 * even when the livepatch gets replaced with a newer version.
	 *
	 * Make sure that the shadow variable does not exist yet.
	 */
	applause = (char *)klp_shadow_alloc(shadow_object, id,
					   APPLAUSE_STR_SIZE, GFP_KERNEL,
					   NULL, NULL);

	if (!applause) {
		pr_err("%s: failed to allocated shadow variable for storing an applause description\n",
		       __func__);
		return -ENOMEM;
	}

	strscpy(applause, "[]", APPLAUSE_STR_SIZE);

	return 0;
}

static void set_applause(unsigned long id)
{
	char *applause;

	applause = (char *)klp_shadow_get(shadow_object, id);
	if (!applause) {
		pr_err("%s: failed to get shadow variable with the applause description: %lu\n",
		       __func__, id);
		return;
	}

	strscpy(applause, "[APPLAUSE]", APPLAUSE_STR_SIZE);
}

static void unset_applause(unsigned long id)
{
	char *applause;

	applause = (char *)klp_shadow_get(shadow_object, id);
	if (!applause) {
		pr_err("%s: failed to get shadow variable with the applause description: %lu\n",
		       __func__, id);
		return;
	}

	strscpy(applause, "[]", APPLAUSE_STR_SIZE);
}

static void check_applause(unsigned long id)
{
	char *applause;

	applause = (char *)klp_shadow_get(shadow_object, id);
	if (!applause) {
		pr_err("%s: failed to get shadow variable with the applause description: %lu\n",
		       __func__, id);
		return;
	}
}

/* Executed before patching when the state is being enabled. */
static int applause_pre_patch_callback(struct klp_patch *patch, struct klp_state *state)
{
	pr_info("%s: state %lu\n", __func__, state->id);

	if (pre_patch_ret) {
		pr_err("%s: forcing err: %pe\n", __func__, ERR_PTR(pre_patch_ret));
		return pre_patch_ret;
	}

	return allocate_applause(state->id);
}

/* Executed after patching when the state being enabled. */
static void applause_post_patch_callback(struct klp_patch *patch, struct klp_state *state)
{
	pr_info("%s: state %lu\n", __func__, state->id);
	set_applause(state->id);
}

/* Executed before unpatching when the state is being disabled. */
static void applause_pre_unpatch_callback(struct klp_patch *patch, struct klp_state *state)
{
	pr_info("%s: state %lu\n", __func__, state->id);
	unset_applause(state->id);
}

/* Executed after unpatching when the state is being disabled. */
static void applause_post_unpatch_callback(struct klp_patch *patch, struct klp_state *state)
{
	/*
	 * Just check that the shadow variable still exist. It will be
	 * freed automatically because state->is_shadow is set.
	 */
	pr_info("%s: state %lu (nope)\n", __func__, state->id);
	check_applause(state->id);
}

/*
 * The shadow_dtor callback is not really needed. The space for the string
 * has been allocated as part of struct klp_shadow. The callback is added
 * just to check that the shadow variable is freed automatically because of
 * state->is_shadow is set.
 */
static void applause_shadow_dtor(void *obj, void *shadow_data)
{
	char *applause = (char *)shadow_data;

	/*
	 * It would be better to print the related state->id. And it would be
	 * easy to get the pointer to struct klp_shadow via the @shadow_data
	 * pointer. But struct klp_state is not defined in a public header.
	 */
	pr_info("%s: freeing applause %s (nope)\n",
		__func__, applause);
}

static void __lp_block_doors_func(struct work_struct *work, const char *caller_func,
		       const char *speaker_id)
{
	struct hall *hall = container_of(work, struct hall, block_doors_work);

	pr_info("%s: Going to block doors%s (fixed).\n", caller_func, speaker_id);
	hall->do_block_doors();
}

/*
 * Prevent tail call optimizations to make sure that this function
 * appears in the backtrace and can block the disable transition.
 */
__attribute__((__optimize__("no-optimize-sibling-calls")))
static void lp_block_doors_func(struct work_struct *work)
{
	__lp_block_doors_func(work, __func__, "");
}

/*
 * Prevent tail call optimizations to make sure that this function
 * appears in the backtrace and can block the disable transition.
 */
__attribute__((__optimize__("no-optimize-sibling-calls")))
static void lp_block_doors_func2(struct work_struct *work)
{
	__lp_block_doors_func(work, __func__, "(2)");
}

static struct klp_func test_klp_speaker_funcs[] = {
	{
		.old_name = "speaker_welcome",
		.new_func = lp_speaker_welcome,
	},
	{
		.old_name = "block_doors_func",
		.new_func = lp_block_doors_func,
	},
	{ }
};

static struct klp_func test_klp_speaker2_funcs[] = {
	{
		.old_name = "speaker_welcome",
		.new_func = lp_speaker2_welcome,
	},
	{
		.old_name = "block_doors_func",
		.new_func = lp_block_doors_func2,
	},
	{ }
};

static struct klp_object objs[] = {
	{
		.name = "test_klp_speaker",
		.funcs = test_klp_speaker_funcs,
	},
	{
		.name = "test_klp_speaker2",
		.funcs = test_klp_speaker2_funcs,
	},
	{ }
};

static struct klp_state states[] = {
	{
		.id = APPLAUSE_ID,
		.is_shadow = true,
		.callbacks = {
			.pre_patch = applause_pre_patch_callback,
			.post_patch = applause_post_patch_callback,
			.pre_unpatch = applause_pre_unpatch_callback,
			.post_unpatch = applause_post_unpatch_callback,
			.shadow_dtor = applause_shadow_dtor,
		},
	},
	{}
};

static struct klp_patch patch = {
	.mod = THIS_MODULE,
	.objs = objs,
};

static int test_klp_speaker_livepatch_init(void)
{
	if (add_applause)
		patch.states = states;

	return klp_enable_patch(&patch);
}

static void test_klp_speaker_livepatch_exit(void)
{
}

module_init(test_klp_speaker_livepatch_init);
module_exit(test_klp_speaker_livepatch_exit);
MODULE_LICENSE("GPL");
MODULE_INFO(livepatch, "Y");
MODULE_DESCRIPTION("Livepatch test: livepatch test_klp_speaker test module");
