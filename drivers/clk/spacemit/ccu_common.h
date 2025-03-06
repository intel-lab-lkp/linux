/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2024 SpacemiT Technology Co. Ltd
 * Copyright (c) 2024 Haylen Chu <heylenay@4d2.org>
 */

#ifndef _CCU_COMMON_H_
#define _CCU_COMMON_H_

#include <linux/regmap.h>

struct ccu_common {
	struct regmap *regmap;
	struct regmap *lock_regmap;

	union {
		/* For DDN and MIX */
		struct {
			u32 reg_ctrl;
			u32 reg_fc;
			u32 fc;
		};

		/* For PLL */
		struct {
			u32 reg_swcr1;
			u32 reg_swcr2;
			u32 reg_swcr3;
		};
	};

	struct clk_hw hw;
};

static inline struct ccu_common *hw_to_ccu_common(struct clk_hw *hw)
{
	return container_of(hw, struct ccu_common, hw);
}

#define ccu_read(reg, c, val)	regmap_read((c)->regmap, (c)->reg_##reg, val)
#define ccu_update(reg, c, mask, val) \
	regmap_update_bits((c)->regmap, (c)->reg_##reg, mask, val)
#define ccu_poll(reg, c, tmp, cond, sleep, timeout) \
	regmap_read_poll_timeout_atomic((c)->regmap, (c)->reg_##reg,	\
					tmp, cond, sleep, timeout)

#endif /* _CCU_COMMON_H_ */
