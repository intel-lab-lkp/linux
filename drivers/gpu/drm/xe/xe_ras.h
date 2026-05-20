/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2026 Intel Corporation
 */

#ifndef _XE_RAS_H_
#define _XE_RAS_H_

#include "xe_ras_types.h"

struct xe_device;
struct xe_sysctrl_event_response;

void xe_ras_counter_threshold_crossed(struct xe_device *xe,
				      struct xe_sysctrl_event_response *response);
void xe_ras_init(struct xe_device *xe);
enum xe_ras_recovery_action xe_ras_process_errors(struct xe_device *xe);
#endif
