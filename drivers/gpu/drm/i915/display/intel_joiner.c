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
