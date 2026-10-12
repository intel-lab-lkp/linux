// SPDX-License-Identifier: MIT
/*
 * Copyright © 2026 Intel Corporation
 */

#include "intel_display_core.h"
#include "intel_display_types.h"
#include "intel_dp.h"
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
