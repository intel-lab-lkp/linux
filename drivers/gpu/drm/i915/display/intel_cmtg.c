// SPDX-License-Identifier: MIT
/*
 * Copyright (C) 2024 Intel Corporation
 */

#include <linux/string.h>
#include <linux/string_choices.h>
#include <linux/types.h>

#include "i915_drv.h"
#include "i915_reg.h"
#include "intel_crtc.h"
#include "intel_cmtg.h"
#include "intel_cmtg_regs.h"
#include "intel_de.h"
#include "intel_display_device.h"
#include "intel_display_types.h"
#include "intel_global_state.h"

/**
 * DOC: Common Primary Timing Generator (CMTG)
 *
 * The CMTG is a timing generator that runs in parallel to transcoders timing
 * generators (TG) to provide a synchronization mechanism where CMTG acts as
 * primary and transcoders TGs act as secondary to the CMTG. The CMTG outputs
 * its TG start and frame sync signals to the transcoders that are configured
 * as secondary, which use those signals to synchronize their own timing with
 * the CMTG's.
 *
 * The CMTG can be used only with eDP or MIPI command mode and supports the
 * following use cases:
 *
 * - Dual eDP: The CMTG can be used to keep two eDP TGs in sync when on a
 *   dual eDP configuration (with or without PSR/PSR2 enabled).
 *
 * - Single eDP as secondary: It is also possible to use a single eDP
 *   configuration with the transcoder TG as secondary to the CMTG. That would
 *   allow a flow that would not require a modeset on the existing eDP when a
 *   new eDP is added for a dual eDP configuration with CMTG.
 *
 * - DC6v: In DC6v, the transcoder might be off but the CMTG keeps running to
 *   maintain frame timings. When exiting DC6v, the transcoder TG then is
 *   synced back the CMTG.
 *
 * Currently, the driver does not use the CMTG, but we need to make sure that
 * we disable it in case we inherit a display configuration with it enabled.
 */

/*
 * We describe here only the minimum state required to allow us to properly
 * disable the CMTG if necessary.
 */
struct intel_cmtg_state {
	struct intel_global_state base;

	bool cmtg_a_enable;
	/*
	 * Xe3_LPD adds a second CMTG that can be used for dual eDP async mode.
	 */
	bool cmtg_b_enable;
	bool trans_a_secondary;
	bool trans_b_secondary;
};

static struct intel_cmtg_state *to_intel_cmtg_state(struct intel_global_state *obj_state)
{
	return container_of(obj_state, struct intel_cmtg_state, base);
}

static struct intel_global_state *
intel_cmtg_duplicate_state(struct intel_global_obj *obj)
{
	struct intel_cmtg_state *cmtg_state = to_intel_cmtg_state(obj->state);

	cmtg_state = kmemdup(cmtg_state, sizeof(*cmtg_state), GFP_KERNEL);
	if (!cmtg_state)
		return NULL;

	return &cmtg_state->base;
}

static void intel_cmtg_destroy_state(struct intel_global_obj *obj,
				     struct intel_global_state *state)
{
	kfree(state);
}

static const struct intel_global_state_funcs intel_cmtg_state_funcs = {
	.atomic_duplicate_state = intel_cmtg_duplicate_state,
	.atomic_destroy_state = intel_cmtg_destroy_state,
};

static bool intel_cmtg_has_cmtg_b(struct intel_display *display)
{
	return DISPLAY_VER(display) >= 20;
}

static bool intel_cmtg_has_clock_sel(struct intel_display *display)
{
	return DISPLAY_VER(display) >= 14;
}

static bool intel_cmtg_requires_modeset(struct intel_display *display)
{
	return DISPLAY_VER(display) < 20;
}

static void intel_cmtg_dump_state(struct intel_display *display,
				  struct intel_cmtg_state *cmtg_state)
{
	drm_dbg_kms(display->drm,
		    "CMTG state readout: CMTG A: %s, CMTG B: %s, Transcoder A secondary: %s, Transcoder B secondary: %s\n",
		    str_enabled_disabled(cmtg_state->cmtg_a_enable),
		    intel_cmtg_has_cmtg_b(display) ? str_enabled_disabled(cmtg_state->cmtg_b_enable) : "n/a",
		    str_yes_no(cmtg_state->trans_a_secondary),
		    str_yes_no(cmtg_state->trans_b_secondary));
}

int intel_cmtg_init(struct intel_display *display)
{
	struct drm_i915_private *i915 = to_i915(display->drm);
	struct intel_cmtg_state *cmtg_state;

	cmtg_state = kzalloc(sizeof(*cmtg_state), GFP_KERNEL);
	if (!cmtg_state)
		return -ENOMEM;

	intel_atomic_global_obj_init(i915, &display->cmtg.obj,
				     &cmtg_state->base,
				     &intel_cmtg_state_funcs);

	return 0;
}

void intel_cmtg_readout_hw_state(struct intel_display *display)
{
	struct intel_cmtg_state *cmtg_state = to_intel_cmtg_state(display->cmtg.obj.state);
	u32 val;

	if (!HAS_CMTG(display))
		return;

	val = intel_de_read(display, TRANS_CMTG_CTL_A);
	cmtg_state->cmtg_a_enable = val & CMTG_ENABLE;

	if (intel_cmtg_has_cmtg_b(display)) {
		val = intel_de_read(display, TRANS_CMTG_CTL_B);
		cmtg_state->cmtg_b_enable = val & CMTG_ENABLE;
	}

	if (intel_crtc_for_pipe(display, PIPE_A)) {
		val = intel_de_read(display, TRANS_DDI_FUNC_CTL2(display, TRANSCODER_A));
		cmtg_state->trans_a_secondary = val & CMTG_SECONDARY_MODE;
	}

	if (intel_crtc_for_pipe(display, PIPE_B)) {
		val = intel_de_read(display, TRANS_DDI_FUNC_CTL2(display, TRANSCODER_B));
		cmtg_state->trans_b_secondary = val & CMTG_SECONDARY_MODE;
	}

	intel_cmtg_dump_state(display, cmtg_state);
}

static bool intel_cmtg_state_changed(struct intel_cmtg_state *old_cmtg_state,
				     struct intel_cmtg_state *new_cmtg_state)
{
	if (!new_cmtg_state)
		return false;

	return old_cmtg_state->cmtg_a_enable != new_cmtg_state->cmtg_a_enable ||
		old_cmtg_state->cmtg_b_enable != new_cmtg_state->cmtg_b_enable ||
		old_cmtg_state->trans_a_secondary != new_cmtg_state->trans_a_secondary ||
		old_cmtg_state->trans_b_secondary != new_cmtg_state->trans_b_secondary;
}

static void intel_cmtg_state_set_disabled(struct intel_cmtg_state *cmtg_state)
{
	cmtg_state->cmtg_a_enable = false;
	cmtg_state->cmtg_b_enable = false;
	cmtg_state->trans_a_secondary = false;
	cmtg_state->trans_b_secondary = false;
}

static void intel_cmtg_disable(struct intel_display *display,
			       struct intel_cmtg_state *old_cmtg_state,
			       struct intel_cmtg_state *new_cmtg_state)
{
	if (!intel_cmtg_state_changed(old_cmtg_state, new_cmtg_state))
		return;

	drm_dbg_kms(display->drm, "Disabling CMTG\n");

	intel_de_rmw(display, TRANS_DDI_FUNC_CTL2(display, TRANSCODER_A), CMTG_SECONDARY_MODE, 0);
	intel_de_rmw(display, TRANS_DDI_FUNC_CTL2(display, TRANSCODER_B), CMTG_SECONDARY_MODE, 0);

	intel_de_rmw(display, TRANS_CMTG_CTL_A, CMTG_ENABLE, 0);

	if (intel_cmtg_has_cmtg_b(display))
		intel_de_rmw(display, TRANS_CMTG_CTL_B, CMTG_ENABLE, 0);

	if (intel_cmtg_has_clock_sel(display)) {
		u32 clk_sel_clr = CMTG_CLK_SEL_A_MASK;
		u32 clk_sel_set = CMTG_CLK_SEL_A_DISABLED;

		if (intel_cmtg_has_cmtg_b(display)) {
			clk_sel_clr |= CMTG_CLK_SEL_B_MASK;
			clk_sel_set |= CMTG_CLK_SEL_B_DISABLED;
		}

		intel_de_rmw(display, CMTG_CLK_SEL, clk_sel_clr, clk_sel_set);
	}
}

static u32 intel_cmtg_modeset_crtc_mask(struct intel_display *display,
					struct intel_cmtg_state *old_cmtg_state,
					struct intel_cmtg_state *new_cmtg_state)
{
	u32 crtc_mask;

	if (intel_cmtg_requires_modeset(display))
		return 0;

	crtc_mask = 0;

	if (old_cmtg_state->trans_a_secondary != new_cmtg_state->trans_a_secondary)
		crtc_mask |= drm_crtc_mask(&intel_crtc_for_pipe(display, PIPE_A)->base);

	if (old_cmtg_state->trans_b_secondary != new_cmtg_state->trans_b_secondary)
		crtc_mask |= drm_crtc_mask(&intel_crtc_for_pipe(display, PIPE_B)->base);

	return crtc_mask;
}

/*
 * Disable CMTG if enabled and return a mask of pipes that need to be disabled
 * (for platforms where disabling the CMTG requires a modeset).
 */
u32 intel_cmtg_sanitize_state(struct intel_display *display)
{
	struct intel_cmtg_state *cmtg_state = to_intel_cmtg_state(display->cmtg.obj.state);
	struct intel_cmtg_state old_cmtg_state;

	if (!HAS_CMTG(display))
		return 0;

	old_cmtg_state = *cmtg_state;
	intel_cmtg_state_set_disabled(cmtg_state);
	intel_cmtg_disable(display, &old_cmtg_state, cmtg_state);

	return intel_cmtg_modeset_crtc_mask(display, &old_cmtg_state, cmtg_state);
}
