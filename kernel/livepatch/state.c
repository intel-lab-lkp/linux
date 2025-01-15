// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * system_state.c - State of the system modified by livepatches
 *
 * Copyright (C) 2019 SUSE
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/livepatch.h>
#include "core.h"
#include "state.h"
#include "transition.h"

#define klp_for_each_state(patch, state)		\
	for (state = patch->states; state && state->id; state++)

/**
 * klp_get_state() - get information about system state modified by
 *	the given patch
 * @patch:	livepatch that modifies the given system state
 * @id:		custom identifier of the modified system state
 *
 * Checks whether the given patch modifies the given system state.
 *
 * The function can be called either from pre/post (un)patch
 * callbacks or from the kernel code added by the livepatch.
 *
 * Return: pointer to struct klp_state when found, otherwise NULL.
 */
struct klp_state *klp_get_state(struct klp_patch *patch, unsigned long id)
{
	struct klp_state *state;

	klp_for_each_state(patch, state) {
		if (state->id == id)
			return state;
	}

	return NULL;
}
EXPORT_SYMBOL_GPL(klp_get_state);

/**
 * klp_get_prev_state() - get information about system state modified by
 *	the already installed livepatches
 * @id:		custom identifier of the modified system state
 *
 * Checks whether already installed livepatches modify the given
 * system state.
 *
 * The same system state can be modified by more non-cumulative
 * livepatches. It is expected that the latest livepatch has
 * the most up-to-date information.
 *
 * The function can be called only during transition when a new
 * livepatch is being enabled or when such a transition is reverted.
 * It is typically called only from pre/post (un)patch
 * callbacks.
 *
 * Return: pointer to the latest struct klp_state from already
 *	installed livepatches, NULL when not found.
 */
struct klp_state *klp_get_prev_state(unsigned long id)
{
	struct klp_patch *patch;
	struct klp_state *state, *last_state = NULL;

	if (WARN_ON_ONCE(!klp_transition_patch))
		return NULL;

	klp_for_each_patch(patch) {
		if (patch == klp_transition_patch)
			goto out;

		state = klp_get_state(patch, id);
		if (state)
			last_state = state;
	}

out:
	return last_state;
}
EXPORT_SYMBOL_GPL(klp_get_prev_state);

/*
 * Check if the new patch is able to deal with the existing system state.
 * Used only for livepatches with the atomic replace enabled. The patch either
 * has to support the existing state or the existing patch must be able
 * to disable it.
 */
static bool klp_is_state_compatible(struct klp_patch *patch,
				    struct klp_state *old_state)
{
	struct klp_state *state;

	state = klp_get_state(patch, old_state->id);

	if (!state && old_state->block_disable)
		return false;

	return true;
}

/*
 * Check if the new livepatch could atomically replace existing ones.
 * It must either support the existing states. Or the existing livepatches
 * must be able to disable the obsolete states.
 */
bool klp_is_patch_compatible(struct klp_patch *patch)
{
	struct klp_patch *old_patch;
	struct klp_state *old_state;

	if (!patch->replace)
		return true;

	klp_for_each_patch(old_patch) {
		klp_for_each_state(old_patch, old_state) {
			if (!klp_is_state_compatible(patch, old_state))
				return false;
		}
	}

	return true;
}

bool klp_patch_disable_blocked(struct klp_patch *patch)
{
	struct klp_state *state;

	klp_for_each_state(patch, state) {
		if (state->block_disable)
			return true;
	}

	return false;
}

static bool is_state_in_other_patches(struct klp_patch *patch,
				      struct klp_state *state)
{
	struct klp_patch *p;
	struct klp_state *s;

	klp_for_each_patch(p) {
		if (p == patch)
			continue;

		klp_for_each_state(p, s) {
			if (s->id == state->id)
				return true;
		}
	}

	return false;
}

int klp_states_pre_patch(struct klp_patch *patch)
{
	struct klp_state *state;

	klp_for_each_state(patch, state) {
		if (!is_state_in_other_patches(patch, state) &&
		    state->callbacks.pre_patch) {
			int err;

			err = state->callbacks.pre_patch(patch, state);
			if (err)
				return err;
		}

		state->callbacks.pre_patch_succeeded = true;
	}

	return 0;
}

void klp_states_post_patch(struct klp_patch *patch)
{
	struct klp_state *state;

	klp_for_each_state(patch, state) {
		if (is_state_in_other_patches(patch, state))
			continue;

		if (state->callbacks.post_patch)
			state->callbacks.post_patch(patch, state);
	}
}

void klp_states_pre_unpatch(struct klp_patch *patch)
{
	struct klp_state *state;

	klp_for_each_state(patch, state) {
		if (is_state_in_other_patches(patch, state))
			continue;

		if (state->callbacks.pre_unpatch)
			state->callbacks.pre_unpatch(patch, state);
	}
}

void klp_states_post_unpatch(struct klp_patch *patch)
{
	struct klp_state *state;

	klp_for_each_state(patch, state) {
		if (is_state_in_other_patches(patch, state))
			continue;

		/*
		 * This only occurs when a transition is canceled after
		 * a preparation step failed.
		 */
		if (!state->callbacks.pre_patch_succeeded)
			continue;

		if (state->callbacks.post_unpatch)
			state->callbacks.post_unpatch(patch, state);

		if (state->is_shadow)
			klp_shadow_free_all(state->id, state->callbacks.shadow_dtor);

		state->callbacks.pre_patch_succeeded = 0;
	}
}

/*
 * Make it clear when pre_unpatch() callbacks need to be reverted
 * in case of failure.
 */
static bool klp_states_pre_unpatch_replaced_called;

void klp_states_pre_unpatch_replaced(struct klp_patch *patch)
{
	struct klp_patch *old_patch;

	/* Make sure that it was cleared at the end of the last transition. */
	WARN_ON(klp_states_pre_unpatch_replaced_called);

	klp_for_each_patch(old_patch) {
		if (old_patch != patch)
			klp_states_pre_unpatch(old_patch);
	}

	klp_states_pre_unpatch_replaced_called = true;
}

void klp_states_post_unpatch_replaced(struct klp_patch *patch)
{
	struct klp_patch *old_patch;

	klp_for_each_patch(old_patch) {
		if (old_patch != patch)
			klp_states_post_unpatch(old_patch);
	}

	/* Reset for the next transition. */
	klp_states_pre_unpatch_replaced_called = false;
}

void klp_states_post_patch_replaced(struct klp_patch *patch)
{
	struct klp_patch *old_patch;

	/*
	 * This only occurs when a transition is canceled after
	 * a preparation step failed.
	 */
	if (!klp_states_pre_unpatch_replaced_called)
		return;

	klp_for_each_patch(old_patch) {
		if (old_patch != patch)
			klp_states_post_patch(old_patch);
	}

	/* Reset for the next transition. */
	klp_states_pre_unpatch_replaced_called = false;
}
