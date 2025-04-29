/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2025 Intel Corporation
 */

#ifndef _XE_LATE_BIND_FW_H_
#define _XE_LATE_BIND_FW_H_

#include <linux/types.h>

struct xe_late_bind;

int xe_late_bind_init(struct xe_late_bind *late_bind);
void xe_late_bind_remove(struct xe_late_bind *late_bind);
int xe_late_bind_fw_init(struct xe_late_bind *late_bind);
int xe_late_bind_fw_load(struct xe_late_bind *late_bind);

#endif
