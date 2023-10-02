/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) 2015-2017  Dialog Semiconductor
 * Copyright (C) 2022  Raptor Engineering, LLC
 */

#ifndef __MFD_SIE_CRONOS_CORE_H__
#define __MFD_SIE_CRONOS_CORE_H__

#include <linux/mfd/sie/cronos/registers.h>

struct sie_cronos_cpld {
	struct device *dev;
	struct regmap *regmap;
};

#endif /* __MFD_SIE_CRONOS_H__ */
