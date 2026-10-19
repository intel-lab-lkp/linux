/* SPDX-License-Identifier: GPL-2.0 */

#ifndef __LINUX_PCS_XPCS_REGMAP_H
#define __LINUX_PCS_XPCS_REGMAP_H

struct device;
struct regmap;
struct dw_xpcs;

struct dw_xpcs *devm_xpcs_regmap_register(struct device *dev,
					  struct regmap *regmap);

#endif /* __LINUX_PCS_XPCS_REGMAP_H */
