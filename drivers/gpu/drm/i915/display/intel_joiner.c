// SPDX-License-Identifier: MIT
/*
 * Copyright © 2026 Intel Corporation
 */

#include "intel_display_core.h"
#include "intel_display_types.h"
#include "intel_dp.h"
#include "intel_hdmi.h"
#include "intel_joiner.h"

bool intel_joiner_needs_dsc(struct intel_display *display,
			    int num_joined_pipes)
{
	/*
	 * Pipe joiner needs compression up to display 12 due to bandwidth
	 * limitation. DG2 onwards pipe joiner can be enabled without
	 * compression.
	 * Ultrajoiner always needs compression.
	 */
	return (!HAS_UNCOMPRESSED_JOINER(display) && num_joined_pipes == 2) ||
		num_joined_pipes == 4;
}

static
int intel_joiner_max_hdisplay_per_pipe(struct intel_display *display)
{
	return DISPLAY_VER(display) >= 30 ? 6144 : 5120;
}

bool intel_joiner_connector_can_join(struct intel_connector *connector)
{
	switch (connector->base.connector_type) {
	case DRM_MODE_CONNECTOR_DisplayPort:
	case DRM_MODE_CONNECTOR_eDP:
		return intel_dp_has_joiner(intel_attached_dp(connector));
	case DRM_MODE_CONNECTOR_HDMIA:
	case DRM_MODE_CONNECTOR_HDMIB:
		return intel_hdmi_has_joiner(intel_attached_hdmi(connector));
	default:
		return false;
	}
}

bool intel_joiner_candidate_valid(struct intel_connector *connector,
				  int hdisplay,
				  int num_joined_pipes)
{
	struct intel_display *display = to_intel_display(connector);

	if (hdisplay > num_joined_pipes * intel_joiner_max_hdisplay_per_pipe(display))
		return false;

	if (connector->force_joined_pipes && connector->force_joined_pipes != num_joined_pipes)
		return false;

	if (num_joined_pipes > 1 && !intel_joiner_connector_can_join(connector))
		return false;

	return intel_joiner_valid_primary_pipe_mask(display, num_joined_pipes);
}

/*
 * Return a bitmask of all the start indices of consecutive bitfields of size
 * width in mask.
 */
static unsigned long find_consecutive_bits(unsigned long mask, int width)
{
	unsigned long bit, out_mask = 0;

	if (!width)
		return 0;

	for_each_set_bit(bit, &mask, BITS_PER_TYPE(mask)) {
		/* For each set bit, see if the following bits are set also */
		unsigned long bitfield = GENMASK(bit + width - 1, bit);

		if ((mask & bitfield) == bitfield)
			out_mask |= BIT(bit);
	}

	return out_mask;
}

/*
 * Return a bitmask of all valid joiner primary pipes for joining
 * num_joined_pipes pipes. For completeness, return all valid pipes for
 * num_joined_pipes == 1.
 *
 * Return 0 if the platform doesn't support joining for the requested number of
 * pipes, or there are not enough consecutive pipes available.
 */
u8 intel_joiner_valid_primary_pipe_mask(struct intel_display *display, int num_joined_pipes)
{
	if (num_joined_pipes == 1) {
		return DISPLAY_RUNTIME_INFO(display)->pipe_mask;
	} else if (num_joined_pipes == 2) {
		if (!HAS_UNCOMPRESSED_JOINER(display) && !HAS_BIGJOINER(display))
			return 0;
	} else if (num_joined_pipes == 4) {
		if (!HAS_ULTRAJOINER(display))
			return 0;
	} else {
		return 0;
	}

	return find_consecutive_bits(DISPLAY_RUNTIME_INFO(display)->pipe_mask, num_joined_pipes);
}
