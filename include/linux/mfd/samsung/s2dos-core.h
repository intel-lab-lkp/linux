/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * s2dos-core.h
 *
 * Copyright (c) 2016 Samsung Electronics Co., Ltd
 *              http://www.samsung.com
 * Copyright (C) 2024 Dzmitry Sankouski <dsankouski@gmail.com>
 *
 */

#ifndef __LINUX_MFD_S2DOS_CORE_H
#define __LINUX_MFD_S2DOS_CORE_H
#include <linux/platform_device.h>
#include <linux/regmap.h>

struct s2dos_core {
	struct device *dev;
	struct regmap *regmap;
};

#endif /*  __LINUX_MFD_S2DOS_CORE_H */
