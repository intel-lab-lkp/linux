/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2026 Intel Corporation
 */

#ifndef __INTEL_JOINER_H__
#define __INTEL_JOINER_H__

#include <linux/types.h>

struct intel_connector;
struct intel_display;

bool intel_joiner_needs_dsc(struct intel_display *display,
			    int num_joined_pipes);
bool intel_joiner_connector_can_join(struct intel_connector *connector);
bool intel_joiner_candidate_valid(struct intel_connector *connector,
				  int hdisplay,
				  int num_joined_pipes);

#define for_each_joiner_candidate(__connector, __mode, __num_joined_pipes) \
	for ((__num_joined_pipes) = 1; (__num_joined_pipes) <= (I915_MAX_PIPES); (__num_joined_pipes)++) \
		for_each_if(intel_joiner_candidate_valid(__connector, (__mode)->hdisplay, __num_joined_pipes))

u8 intel_joiner_valid_primary_pipe_mask(struct intel_display *display, int num_joined_pipes);

#endif /* __INTEL_JOINER_H__ */
