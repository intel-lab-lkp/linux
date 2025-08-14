/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2025 Red Hat, Inc.
 *
 * Common definitions for DA and HA monitors.
 */
#ifndef _RV_DA_COMMON_H
#define _RV_DA_COMMON_H

#include <linux/sched.h>

/*
 * ID monitor types (per-task and per-object) have an opaque type, this is
 * defined by default for the per-task case but must be defined by the monitor
 * in case of per-object monitors.
 */
#if RV_MON_TYPE == RV_MON_PER_TASK
typedef struct task_struct *monitor_target;
#endif

#endif /* _RV_DA_COMMON_H */
