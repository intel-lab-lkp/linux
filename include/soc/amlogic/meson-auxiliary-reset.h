/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __SOC_AMLOGIC_MESON_AUX_RESET_H
#define __SOC_AMLOGIC_MESON_AUX_RESET_H

#include <linux/err.h>

struct device;
struct regmap;

#ifdef CONFIG_RESET_MESON
int devm_meson_rst_aux_register(struct device *dev,
				struct regmap *map,
				const char *adev_name);
#else
static inline int devm_meson_rst_aux_register(struct device *dev,
					      struct regmap *map,
					      const char *adev_name)
{
	return -EOPNOTSUPP;
}
#endif

#endif /* __SOC_AMLOGIC_MESON8B_AUX_RESET_H */
