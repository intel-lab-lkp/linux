/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __LINUX_SOC_RENESAS_RZ_SYSC_H__
#define __LINUX_SOC_RENESAS_RZ_SYSC_H__

#include <linux/device.h>
#include <linux/err.h>
#include <linux/regmap.h>

/**
 * struct rz_sysc_signal_map - RZ SYSC signal mapping (to be used by consummers)
 * @regmap: SYSC regmap
 * @offset: offset into the SYSC address space for accessing the signal
 * @mask: mask into the register at offset for accessing the signal
 */
struct rz_sysc_signal_map {
	struct regmap *regmap;
	u32 offset;
	u32 mask;
};

#ifdef CONFIG_SYSC_RZ
extern struct rz_sysc_signal_map *rz_sysc_get_signal_map(struct device *dev);
#else
static inline struct rz_sysc_signal_map *rz_sysc_get_signal_map(struct device *dev)
{
	return ERR_PTR(-EOPNOTSUPP);
}
#endif

#endif /* __LINUX_SOC_RENESAS_RZ_SYSC_H__ */
