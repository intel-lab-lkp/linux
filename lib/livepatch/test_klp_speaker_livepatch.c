// SPDX-License-Identifier: GPL-2.0

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/livepatch.h>
#include <linux/init.h>

#include "test_klp_speaker.h"

#define APPLAUSE_ID 1
#define APPLAUSE_SIZE 64

/* associate the shadow variable with NULL address */;
void *shadow_object = NULL;

/* load/run-time control from sysfs writer  */
static bool add_applause;
module_param(add_applause, bool, 0600);
MODULE_PARM_DESC(add_applause, "Use shadow variable to add applause (default=false)");

/* load/run-time control from sysfs writer  */
static int setup_ret;
module_param(setup_ret, int, 0644);
MODULE_PARM_DESC(setup_ret, "Allow to force failure for the setup callback (default=0)");

/* load/run-time control from sysfs writer  */
static bool noreplace;
module_param(noreplace, bool, 0600);
MODULE_PARM_DESC(noreplace, "Allow to install the livepatch together with other livepatches. (default=false)");

#define LIVEPATCH_SPEAKER_WELCOME_FN(fn_name)					\
noinline									\
void fn_name(void)								\
{										\
	const char *applause;							\
										\
	applause = (char *)klp_shadow_get(shadow_object, APPLAUSE_ID);		\
										\
	if (!applause)								\
		applause = "";							\
										\
	pr_info("%s: %sLadies and gentleman, ...\n", __func__, applause);	\
}

LIVEPATCH_SPEAKER_WELCOME_FN(livepatch_speaker_welcome)
LIVEPATCH_SPEAKER_WELCOME_FN(livepatch_speaker_welcome2)

static int allocate_applause(void)
{
	char *applause;

	/*
	 * Attach the shadow variable to some well known address it stays
	 * even when the livepatch gets replaced with a newer version.
	 *
	 * Make sure that the shadow variable does not exist yet.
	 */
	applause = (char *)klp_shadow_alloc(shadow_object, APPLAUSE_ID,
					   APPLAUSE_SIZE, GFP_KERNEL,
					   NULL, NULL);

	if (!applause) {
		pr_err("%s: failed to allocated shadow variable for storing an applause description\n",
		       __func__);
		return -ENOMEM;
	}

	/*
	 * Fill the shadow target with an empty brackets before all processes
	 * get livepatched.
	 */
	strscpy(applause, "[] ", APPLAUSE_SIZE);

	return 0;
}

static void set_applause(void)
{
	char *applause;

	applause = (char *)klp_shadow_get(shadow_object, APPLAUSE_ID);
	if (!applause) {
		pr_err("%s: failed to get shadow variable with the applause description: %d\n",
		       __func__, APPLAUSE_ID);
		return;
	}

	strscpy(applause, "[APPLAUSE] ", APPLAUSE_SIZE);
}

static void unset_applause(void)
{
	char *applause;

	applause = (char *)klp_shadow_get(shadow_object, APPLAUSE_ID);
	if (!applause) {
		pr_err("%s: failed to get shadow variable with the applause description: %d\n",
		       __func__, APPLAUSE_ID);
		return;
	}

	applause[0] = '\0';
}

static void free_applause(void)
{
	char *applause;

	applause = (char *)klp_shadow_get(shadow_object, APPLAUSE_ID);
	if (!applause) {
		pr_err("%s: failed to get shadow variable with the applause description: %d\n",
		       __func__, APPLAUSE_ID);
		return;
	}

	klp_shadow_free(shadow_object, APPLAUSE_ID, NULL);
}

/* Executed before patching when the state is new. */
static int setup_applause_callback(struct klp_patch *patch, struct klp_state *state)
{
	pr_info("%s: state %lu\n", __func__, state->id);

	if (setup_ret) {
		pr_err("%s: forcing err: %pe\n", __func__, ERR_PTR(setup_ret));
		return setup_ret;
	}

	return allocate_applause();
}

/* Executed after patching when the state is new. */
static void enable_applause_callback(struct klp_patch *patch, struct klp_state *state)
{
	pr_info("%s: state %lu\n", __func__, state->id);
	set_applause();
}

/* Executed before unpatching when the state is obsoleted. */
static void disable_applause_callback(struct klp_patch *patch, struct klp_state *state)
{
	pr_info("%s: state %lu\n", __func__, state->id);
	unset_applause();
}

/* Executed after unpatching when the state is obsoleted. */
static void release_applause_callback(struct klp_patch *patch, struct klp_state *state)
{
	pr_info("%s: state %lu\n", __func__, state->id);
	free_applause();
}

#define LIVEPATCH_CALL_SPEAKER_FN(fn_name)			\
void fn_name(struct speaker *speaker)		\
{								\
	pr_info("%s: Calling speaker (fixed).\n", __func__);	\
	speaker->wait_and_welcome(speaker);			\
}

LIVEPATCH_CALL_SPEAKER_FN(livepatch_call_speaker)
LIVEPATCH_CALL_SPEAKER_FN(livepatch_call_speaker2)

static struct klp_func test_klp_speaker_funcs[] = {
	{
		.old_name = "speaker_welcome",
		.new_func = livepatch_speaker_welcome,
	},
	{
		.old_name = "call_speaker",
		.new_func = livepatch_call_speaker,
	},
	{ }
};

static struct klp_func test_klp_speaker2_funcs[] = {
	{
		.old_name = "speaker_welcome2",
		.new_func = livepatch_speaker_welcome2,
	},
	{
		.old_name = "call_speaker2",
		.new_func = livepatch_call_speaker2,
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
			.setup = setup_applause_callback,
			.enable = enable_applause_callback,
			.disable = disable_applause_callback,
			.release = release_applause_callback,
		},
	},
	{ }
};

/*
 * Use the atomic replace by default so that the APPLAUSE state
 * is correctly transferred when another version of the speaker
 * livepatch gets loaded.
 *
 * Can be overridden by "noreplace=1" parameter. But it can't
 * be used together with the "add_applause=1" parameter when
 * another speaker livepatch is already loaded with
 * the "add_applause=1" parameter.
 */
static struct klp_patch patch = {
	.mod = THIS_MODULE,
	.objs = objs,
	.replace = true,
};

static int test_klp_speaker_livepatch_init(void)
{
	if (add_applause)
		patch.states = states;

	if (noreplace) {
		if (add_applause)
			pr_warn("The speaker livepatch can't be loaded when both \"add_applause\" and \"noreplace\" are used and another speaker livepatch is already loaded with \"add_aplause\"\n");

		patch.replace = false;
	}

	return klp_enable_patch(&patch);
}

static void test_klp_speaker_livepatch_exit(void)
{
}

module_init(test_klp_speaker_livepatch_init);
module_exit(test_klp_speaker_livepatch_exit);
MODULE_LICENSE("GPL");
MODULE_INFO(livepatch, "Y");
