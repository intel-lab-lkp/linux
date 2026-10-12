/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2026 Intel Corporation
 */

#ifndef __INTEL_JOINER_H__
#define __INTEL_JOINER_H__

#include <linux/types.h>

struct intel_display;

bool intel_joiner_needs_dsc(struct intel_display *display,
			    int num_joined_pipes);

#endif /* __INTEL_JOINER_H__ */
