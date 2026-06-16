// SPDX-License-Identifier: MIT
/*
 * Copyright © 2026 Intel Corporation
 */

#include <linux/slab.h>

#include <drm/drm_print.h>

#include "intel_display_core.h"
#include "intel_display_types.h"
#include "intel_dp.h"
#include "intel_dp_link_caps.h"

struct intel_dp_link_caps {
	struct intel_dp *dp;
};

/* Get length of common rates array potentially limited by max_rate. */
int intel_dp_common_len_rate_limit(const struct intel_dp *intel_dp,
				   int max_rate)
{
	return intel_dp_rate_limit_len(intel_dp->common_rates,
				       intel_dp->num_common_rates, max_rate);
}

int intel_dp_common_rate(struct intel_dp *intel_dp, int index)
{
	struct intel_display *display = to_intel_display(intel_dp);

	if (drm_WARN_ON(display->drm,
			index < 0 || index >= intel_dp->num_common_rates))
		return 162000;

	return intel_dp->common_rates[index];
}

/* Theoretical max between source and sink */
int intel_dp_max_common_rate(struct intel_dp *intel_dp)
{
	return intel_dp_common_rate(intel_dp, intel_dp->num_common_rates - 1);
}

static int forced_lane_count(struct intel_dp *intel_dp)
{
	return clamp(intel_dp->link.force_lane_count, 1, intel_dp_max_common_lane_count(intel_dp));
}

static int forced_link_rate(struct intel_dp *intel_dp)
{
	int len = intel_dp_common_len_rate_limit(intel_dp, intel_dp->link.force_rate);

	if (len == 0)
		return intel_dp_common_rate(intel_dp, 0);

	return intel_dp_common_rate(intel_dp, len - 1);
}

void intel_dp_link_caps_get_forced_params(struct intel_dp_link_caps *link_caps,
					  struct intel_dp_link_config *forced_params)
{
	forced_params->rate = forced_link_rate(link_caps->dp);
	forced_params->lane_count = forced_lane_count(link_caps->dp);
}

struct intel_dp_link_caps *intel_dp_link_caps_init(struct intel_dp *intel_dp)
{
	struct intel_dp_link_caps *link_caps;

	link_caps = kzalloc_obj(*link_caps);
	if (!link_caps)
		return NULL;

	link_caps->dp = intel_dp;

	return link_caps;
}

void intel_dp_link_caps_cleanup(struct intel_dp_link_caps *link_caps)
{
	kfree(link_caps);
}
