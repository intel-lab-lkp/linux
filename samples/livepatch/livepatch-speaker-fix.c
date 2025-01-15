// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2024 SUSE

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/livepatch.h>
#include <linux/init.h>

#include "livepatch-speaker.h"

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
 *
 *    - Support testing of more shadow variables and state callbacks. see
 *	"applause", and "applause2" module parameters.
 *
 *    - Allow to enable the atomic replace via "replace" parameter.
 */

#define APPLAUSE_NUM 2
#define APPLAUSE_START_ID 10
#define APPLAUSE_STR_SIZE 16
#define APPLAUSE_IDX_STR_SIZE 8

/* associate the shadow variable with NULL address */;
static void *shadow_object = NULL;

static bool add_applause[APPLAUSE_NUM];
module_param_named(applause, add_applause[0], bool, 0400);
MODULE_PARM_DESC(applause, "Use shadow variable to add applause (default=false)");
module_param_named(applause2, add_applause[1], bool, 0400);
MODULE_PARM_DESC(applause2, "Use shadow variable to add 2nd applause (default=false)");

static int pre_patch_ret;
module_param(pre_patch_ret, int, 0400);
MODULE_PARM_DESC(pre_patch_ret, "Allow to force failure for the pre_patch callback (default=0)");

static bool replace;
module_param(replace, bool, 0400);
MODULE_PARM_DESC(replace, "Enable the atomic replace feature when loading the livepatch. (default=false)");

/* Conversion between the index to the @add_applause table and state ID. */
#define __idx_to_state_id(idx) (idx + APPLAUSE_START_ID)
#define __state_id_to_idx(state_id) (state_id - APPLAUSE_START_ID)

static void __lp_speaker_welcome(const char *caller_func,
				 const char *speaker_id,
				 const char *context)
{
	char entire_applause[APPLAUSE_NUM * APPLAUSE_STR_SIZE + 1] = "";
	int idx, ret;
	int len = 0;

	for (idx = 0; idx < APPLAUSE_NUM ; idx++) {
		const char *applause;

		applause = (char *)klp_shadow_get(shadow_object,
						  __idx_to_state_id(idx));

		if (applause) {
			ret = strscpy(entire_applause + len, applause,
				       sizeof(entire_applause) - len);
			if (ret < 0) {
				pr_warn("Too small buffer for entire_applause. Truncating...\n");
				len = sizeof(entire_applause) - 1;
				break;
			}
			len += ret;
		}
	}

	if (len) {
		ret = strscpy(entire_applause + len, " ",
			       sizeof(entire_applause) - len);
		if (ret < 0) {
			pr_warn("Too small buffer for entire_applause. Truncating...\n");
			len = sizeof(entire_applause) - 1;
		} else {
			len += ret;
		}
	}

	pr_info("%s%s: %sLadies and gentleman, ...%s\n",
		caller_func, speaker_id, entire_applause, context);
}

static void lp_speaker_welcome(const char *context)
{
	__lp_speaker_welcome(__func__, "", context);
}

static char *state_id_to_idx_str(char *buf, size_t size,
				   unsigned long state_id)
{
	int idx;

	idx = __state_id_to_idx(state_id);

	if (idx < 0 || idx >= APPLAUSE_NUM) {
		pr_err("%s: Applause table index out of scope: %d\n", __func__, idx);
		return "";
	}

	if (idx == 0)
		return "";

	snprintf(buf, size, "%d", idx + 1);
	return buf;
}

static int allocate_applause(unsigned long id)
{
	char idx_str[APPLAUSE_IDX_STR_SIZE];
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

	snprintf(applause, APPLAUSE_STR_SIZE, "[%s]",
		 state_id_to_idx_str(idx_str, sizeof(idx_str), id));

	return 0;
}

static void set_applause(unsigned long id)
{
	char idx_str[APPLAUSE_IDX_STR_SIZE];
	char *applause;

	applause = (char *)klp_shadow_get(shadow_object, id);
	if (!applause) {
		pr_err("%s: failed to get shadow variable with the applause description: %lu\n",
		       __func__, id);
		return;
	}

	snprintf(applause, APPLAUSE_STR_SIZE, "[APPLAUSE%s]",
		 state_id_to_idx_str(idx_str, sizeof(idx_str), id));
}

static void unset_applause(unsigned long id)
{
	char idx_str[APPLAUSE_IDX_STR_SIZE];
	char *applause;

	applause = (char *)klp_shadow_get(shadow_object, id);
	if (!applause) {
		pr_err("%s: failed to get shadow variable with the applause description: %lu\n",
		       __func__, id);
		return;
	}

	snprintf(applause, APPLAUSE_STR_SIZE, "[%s]",
		 state_id_to_idx_str(idx_str, sizeof(idx_str), id));
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

static struct klp_func livepatch_speaker_mod_funcs[] = {
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

static struct klp_object objs[] = {
	{
		.name = "livepatch_speaker_mod",
		.funcs = livepatch_speaker_mod_funcs,
	},
	{ }
};

static struct klp_patch patch = {
	.mod = THIS_MODULE,
	.objs = objs,
};


/*
 * The array with states is dynamically allocated depending on which states
 * are enabled on the command line.
 */
static struct klp_state *applause_states;

static int applause_init(void)
{
	int idx, idx_allowed, id, enabled_cnt;

	enabled_cnt = 0;

	for (idx = 0, id = APPLAUSE_START_ID, enabled_cnt = 0;
	     idx < APPLAUSE_NUM;
	     idx++, id++) {
		if (add_applause[idx])
			enabled_cnt++;
	}

	if (enabled_cnt) {
		/* Allocate one more state as the trailing entry. */
		applause_states =
			kzalloc(sizeof(applause_states[0]) * (enabled_cnt + 1),	GFP_KERNEL);
		if (!applause_states)
			return -ENOMEM;

		patch.states = applause_states;

		for (idx = 0, idx_allowed = 0;
		     idx < APPLAUSE_NUM;
		     idx++) {
			struct klp_state *state;

			if (!add_applause[idx])
				continue;

			if (idx_allowed >= enabled_cnt) {
				pr_warn("Too many enabled applause states\n");
				continue;
			}

			state = &applause_states[idx_allowed++];

			state->id = __idx_to_state_id(idx);
			state->is_shadow = true;
			state->callbacks.pre_patch = applause_pre_patch_callback;
			state->callbacks.post_patch = applause_post_patch_callback;
			state->callbacks.pre_unpatch = applause_pre_unpatch_callback;
			state->callbacks.post_unpatch = applause_post_unpatch_callback;
			state->callbacks.shadow_dtor = applause_shadow_dtor;
		}
	}

	return 0;
}

static int livepatch_speaker_fix_init(void)
{
	int err;

	err = applause_init();
	if (err)
		return err;

	if (replace)
		patch.replace = true;

	return klp_enable_patch(&patch);
}

static void livepatch_speaker_fix_exit(void)
{
	kfree(applause_states);
}

module_init(livepatch_speaker_fix_init);
module_exit(livepatch_speaker_fix_exit);
MODULE_LICENSE("GPL");
MODULE_INFO(livepatch, "Y");
MODULE_DESCRIPTION("Livepatch sample: livepatch speaker module fix");
