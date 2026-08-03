/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2026 Juan Manuel López Carrillo
 */

#ifndef _CCU_MASKDIV_H_
#define _CCU_MASKDIV_H_

#include <linux/clk-provider.h>

#include "ccu_common.h"
#include "ccu_mux.h"

/*
 * struct ccu_maskdiv - cycle-masking ("fractional") divider
 *
 * This divider does not divide the parent clock: it masks (swallows) M
 * pulses out of every 2^width parent cycles, so the average output rate
 * is
 *
 *	rate = parent * (2^width - M) / 2^width
 *
 * with the remaining pulses keeping the parent period. The A523/T527
 * GPU clock (GPU_CLK_REG, 0x670) is such a divider: "FACTOR_M: mask M
 * cycles at 16 cycles", GPU_CLK = Clock Source * ((16-M)/16) (T527 user
 * manual v0.92, section 2.7.6.58).
 *
 * This type does not support CLK_SET_RATE_PARENT: determine_rate
 * evaluates parents at their current rate and does not propagate rate
 * requests upstream.  If a future user needs parent rate propagation,
 * switch to clk_hw_round_rate() in the determine_rate loop.
 *
 * @shift:	shift of the M field in the register
 * @width:	width of the M field; the mask window is 2^width cycles
 */
struct ccu_maskdiv {
	u32			enable;

	u8			shift;
	u8			width;

	struct ccu_mux_internal	mux;
	struct ccu_common	common;
};

#define SUNXI_CCU_MASKDIV_HW_WITH_MUX_TABLE_GATE(_struct, _name,	\
						 _parents, _table,	\
						 _reg,			\
						 _mshift, _mwidth,	\
						 _muxshift, _muxwidth,	\
						 _gate, _flags)		\
	struct ccu_maskdiv _struct = {					\
		.enable	= _gate,					\
		.shift	= _mshift,					\
		.width	= _mwidth,					\
		.mux	= _SUNXI_CCU_MUX_TABLE(_muxshift, _muxwidth,	\
					       _table),			\
		.common	= {						\
			.reg		= _reg,				\
			.hw.init	= CLK_HW_INIT_PARENTS_HW(_name,	\
								 _parents, \
								 &ccu_maskdiv_ops, \
								 _flags), \
		},							\
	}

static inline struct ccu_maskdiv *hw_to_ccu_maskdiv(struct clk_hw *hw)
{
	struct ccu_common *common = hw_to_ccu_common(hw);

	return container_of(common, struct ccu_maskdiv, common);
}

extern const struct clk_ops ccu_maskdiv_ops;

#endif /* _CCU_MASKDIV_H_ */
