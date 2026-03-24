/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __LINUX_RESET_REALTEK_H__
#define __LINUX_RESET_REALTEK_H__

#include <linux/types.h>

struct device;
struct regmap;

struct rtk_reset_desc {
	u32 ofs;
	u32 bit;
	u32 write_en;
};

struct rtk_reset_initdata {
	struct rtk_reset_desc *descs;
	u32 num_descs;
	struct regmap *regmap;
};

int rtk_reset_controller_add(struct device *dev,
			     struct rtk_reset_initdata *initdata);

#endif /* __LINUX_RESET_REALTEK_H__ */
