/* SPDX-License-Identifier: GPL-2.0-only */

/* SpacemiT reset driver core */

#ifndef __RESET_SPACEMIT_CORE_H__
#define __RESET_SPACEMIT_CORE_H__

#include <linux/device.h>
#include <linux/reset-controller.h>
#include <linux/types.h>

struct ccu_reset_data {
	u32 offset;
	u32 assert_mask;
	u32 deassert_mask;
};

#define RESET_DATA(_offset, _assert_mask, _deassert_mask)	\
	{							\
		.offset		= (_offset),			\
		.assert_mask	= (_assert_mask),		\
		.deassert_mask	= (_deassert_mask),		\
	}

struct ccu_reset_controller_data {
	const struct ccu_reset_data *reset_data;	/* array */
	size_t count;
};

struct ccu_reset_controller {
	struct reset_controller_dev rcdev;
	const struct ccu_reset_controller_data *data;
	struct regmap *regmap;
};

extern int spacemit_reset_controller_register(struct device *dev,
			      struct ccu_reset_controller *controller);

#endif /* __RESET_SPACEMIT_CORE_H__ */
