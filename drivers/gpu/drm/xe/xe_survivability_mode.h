/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2025 Intel Corporation
 */

#ifndef _XE_SURVIVABILITY_MODE_H_
#define _XE_SURVIVABILITY_MODE_H_

#include <linux/types.h>

struct xe_device;
enum xe_survivability_type;

int xe_survivability_mode_enable(struct xe_device *xe, const enum xe_survivability_type);
bool xe_survivability_mode_is_enabled(struct xe_device *xe);
bool xe_survivability_mode_is_runtime(struct xe_device *xe);
bool xe_survivability_mode_is_requested(struct xe_device *xe);

#endif /* _XE_SURVIVABILITY_MODE_H_ */
