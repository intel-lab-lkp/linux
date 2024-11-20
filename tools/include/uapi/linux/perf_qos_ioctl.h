/* SPDX-License-Identifier: LGPL-2.0+ WITH Linux-syscall-note */
/*
 * Performance QoS device abstraction
 *
 * Copyright (2024) Linaro Ltd
 *
 * Author: Daniel Lezcano <daniel.lezcano@linaro.org>
 *
 */
#ifndef __PERF_QOS_IOCTL_H
#define __PERF_QOS_IOCTL_H

#include <linux/types.h>

enum {
	PERF_QOS_IOC_SET_MIN_CMD,
	PERF_QOS_IOC_GET_MIN_CMD,
	PERF_QOS_IOC_SET_MAX_CMD,
	PERF_QOS_IOC_GET_MAX_CMD,
	PERF_QOS_IOC_GET_UNIT_CMD,
	PERF_QOS_IOC_GET_LIMITS_CMD,
	PERF_QOS_IOC_MAX_CMD,
};

typedef enum {
	PERF_QOS_UNIT_NORMAL,
	PERF_QOS_UNIT_KBPS,
	PERF_QOS_UNIT_MAX
} perf_qos_unit_t;

struct perf_qos_ioctl_arg {
	int value;
	int limit_min;
	int limit_max;
	perf_qos_unit_t unit;
};

#define PERF_QOS_IOCTL_TYPE 'P'

#define PERF_QOS_IOC_SET_MIN	_IOW(PERF_QOS_IOCTL_TYPE, PERF_QOS_IOC_SET_MIN_CMD,	struct perf_qos_ioctl_arg *)
#define PERF_QOS_IOC_GET_MIN	_IOR(PERF_QOS_IOCTL_TYPE, PERF_QOS_IOC_GET_MIN_CMD,	struct perf_qos_ioctl_arg *)
#define PERF_QOS_IOC_SET_MAX	_IOW(PERF_QOS_IOCTL_TYPE, PERF_QOS_IOC_SET_MAX_CMD,	struct perf_qos_ioctl_arg *)
#define PERF_QOS_IOC_GET_MAX	_IOR(PERF_QOS_IOCTL_TYPE, PERF_QOS_IOC_GET_MAX_CMD,	struct perf_qos_ioctl_arg *)
#define PERF_QOS_IOC_GET_UNIT	_IOR(PERF_QOS_IOCTL_TYPE, PERF_QOS_IOC_GET_UNIT_CMD,	struct perf_qos_ioctl_arg *)
#define PERF_QOS_IOC_GET_LIMITS	_IOR(PERF_QOS_IOCTL_TYPE, PERF_QOS_IOC_GET_LIMITS_CMD,	struct perf_qos_ioctl_arg *)

#endif
