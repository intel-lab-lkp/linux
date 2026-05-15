/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * AMD SBTSI misc tsi device .
 *
 * Copyright (C) 2026 Advanced Micro Devices, Inc.
 */

#ifndef _LINUX_TSI_CORE_H_
#define _LINUX_TSI_CORE_H_

#include <linux/misc/tsi.h>

int create_misc_tsi_device(struct sbtsi_data *data, struct device *dev);

#endif /* _LINUX_TSI_CORE_H_ */
