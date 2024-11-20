/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Performance QoS device abstraction
 *
 * Copyright (2024) Linaro Ltd
 *
 * Author: Daniel Lezcano <daniel.lezcano@linaro.org>
 *
 */
#ifndef __PERF_QOS_H
#define __PERF_QOS_H

#include <uapi/linux/perf_qos_ioctl.h>

struct perf_qos;

/**
 * struct perf_qos_value_descr - Performance constraint description
 *
 * @unit: the unit used for the constraint (normalized, throughput, ...)
 * @limit_min: the minimal constraint limit to be set
 * @limit_max: the maximal constraint limit to be set
 */
struct perf_qos_value_descr {
	perf_qos_unit_t unit;
	int limit_min;
	int limit_max;
};

typedef int (*set_perf_limit_cb_t)(int);

struct perf_qos_ops {
	set_perf_limit_cb_t set_perf_limit_max;
	set_perf_limit_cb_t set_perf_limit_min;
};

extern struct perf_qos *perf_qos_device_create(const char *name,
					       struct perf_qos_ops *ops,
					       struct perf_qos_value_descr *descr);

extern int perf_qos_is_allowed(struct perf_qos *pq, int performance);

extern void perf_qos_device_destroy(struct perf_qos *pq);

#endif
