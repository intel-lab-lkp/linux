/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#ifndef __LINUX_CONTEXT_BUS_H
#define __LINUX_CONTEXT_BUS_H

#include <linux/device.h>

#ifdef CONFIG_CONTEXT_DEVICE_BUS
extern const struct bus_type context_device_bus_type;
#endif

#endif /* __LINUX_CONTEXT_BUS_H */
