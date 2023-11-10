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

/* Check if the patch is able to deal with the existing system state. */
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
 * Check that the new livepatch will not break the existing system states.
 * The patch could replace existing patches only when the obsolete
 * states can be disabled.
 */
bool klp_is_patch_compatible(struct klp_patch *patch)
{
	struct klp_patch *old_patch;
	struct klp_state *old_state;

	/* Non-cumulative patches are always compatible. */
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

bool is_state_in_other_patches(struct klp_patch *patch, struct klp_state *state)
{
	struct klp_patch *old_patch;
	struct klp_state *old_state;

	klp_for_each_patch(old_patch) {
		if (old_patch == patch)
			continue;

		klp_for_each_state(old_patch, old_state) {
			if (old_state->id == state->id)
				return true;
		}
	}

	return false;
}

int klp_setup_states(struct klp_patch *patch)
{
	struct klp_state *state;
	int err;

	klp_for_each_state(patch, state) {
		if (!is_state_in_other_patches(patch, state) &&
		    state->callbacks.setup) {

			err = state->callbacks.setup(patch, state);
			if (err)
				goto err;
		}

		state->callbacks.setup_succeeded = true;
	}

	return 0;

err:
	klp_release_states(patch);
	return err;
}

void klp_enable_states(struct klp_patch *patch)
{
	struct klp_state *state;

	klp_for_each_state(patch, state) {
		if (is_state_in_other_patches(patch, state))
			continue;

		if (!state->callbacks.enable)
			continue;

		state->callbacks.enable(patch, state);
	}
}

void klp_disable_states(struct klp_patch *patch)
{
	struct klp_state *state;

	klp_for_each_state(patch, state) {
		if (is_state_in_other_patches(patch, state))
			continue;

		if (!state->callbacks.disable)
			continue;

		state->callbacks.disable(patch, state);
	}
}

void klp_release_states(struct klp_patch *patch)
{
	struct klp_state *state;

	klp_for_each_state(patch, state) {
		if (is_state_in_other_patches(patch, state))
			continue;

		if (state->callbacks.release && state->callbacks.setup_succeeded)
			state->callbacks.release(patch, state);

		if (state->is_shadow)
			klp_shadow_free_all(state->id, state->callbacks.shadow_dtor);

		/*
		 * The @release callback is supposed to restore the original
		 * state before the @setup callback was called.
		 */
		state->callbacks.setup_succeeded = 0;
	}
}

void klp_enable_obsolete_states(struct klp_patch *patch)
{
	struct klp_patch *old_patch;

	klp_for_each_patch(old_patch) {
		if (old_patch != patch)
			klp_enable_states(old_patch);
	}
}

void klp_disable_obsolete_states(struct klp_patch *patch)
{
	struct klp_patch *old_patch;

	klp_for_each_patch(old_patch) {
		if (old_patch != patch)
			klp_disable_states(old_patch);
	}
}

void klp_release_obsolete_states(struct klp_patch *patch)
{
	struct klp_patch *old_patch;

	klp_for_each_patch(old_patch) {
		if (old_patch != patch)
			klp_release_states(old_patch);
	}
}
