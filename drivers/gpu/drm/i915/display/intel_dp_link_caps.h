/* SPDX-License-Identifier: MIT */
/* Copyright © 2026 Intel Corporation */

#ifndef __INTEL_DP_LINK_CAPS_H__
#define __INTEL_DP_LINK_CAPS_H__

#include <linux/types.h>

struct intel_connector;
struct intel_dp;
struct intel_dp_link_caps;
struct intel_dp_link_config;

int intel_dp_common_len_rate_limit(const struct intel_dp *intel_dp,
				   int max_rate);
int intel_dp_common_rate(struct intel_dp *intel_dp, int index);
int intel_dp_link_caps_common_rate_idx(struct intel_dp_link_caps *link_caps, int rate);
int intel_dp_max_common_rate(struct intel_dp *intel_dp);
int intel_dp_link_caps_num_common_rates(struct intel_dp_link_caps *link_caps);

void intel_dp_link_caps_print_common_rates(struct intel_dp_link_caps *link_caps);

void intel_dp_link_caps_get_forced_params(struct intel_dp_link_caps *link_caps,
					  struct intel_dp_link_config *forced_params);

int intel_dp_link_config_index(struct intel_dp *intel_dp, int link_rate, int lane_count);
void intel_dp_link_config_get(struct intel_dp *intel_dp, int idx, int *link_rate, int *lane_count);

bool intel_dp_link_caps_update(struct intel_dp *intel_dp,
			       const int *rates, int num_rates);

void intel_dp_link_caps_debugfs_add(struct intel_connector *connector);

struct intel_dp_link_caps *intel_dp_link_caps_init(struct intel_dp *intel_dp);
void intel_dp_link_caps_cleanup(struct intel_dp_link_caps *link_caps);

#endif /* __INTEL_DP_LINK_CAPS_H__ */
