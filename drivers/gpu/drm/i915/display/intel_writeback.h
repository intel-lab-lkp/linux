/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2025 Intel Corporation
 */

#ifndef __INTEL_WRITEBACK_H__
#define __INTEL_WRITEBACK_H__

#include <linux/types.h>

struct intel_display;
struct intel_writeback_connector;

int intel_writeback_init(struct intel_display *display);

#endif /* __INTEL_WRITEBACK_H__ */

