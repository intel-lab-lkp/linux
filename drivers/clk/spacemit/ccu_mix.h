/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2024 SpacemiT Technology Co. Ltd
 * Copyright (c) 2024 Haylen Chu <heylenay@4d2.org>
 */

#ifndef _CCU_MIX_H_
#define _CCU_MIX_H_

#include <linux/clk-provider.h>

#include "ccu_common.h"

struct ccu_gate_config {
	u32 mask;
};

struct ccu_factor_config {
	u32 div;
	u32 mul;
};

struct ccu_mux_config {
	u8 shift;
	u8 width;
};

struct ccu_div_config {
	u8 shift;
	u8 width;
};

struct ccu_mix {
	struct ccu_factor_config factor;
	struct ccu_gate_config gate;
	struct ccu_div_config div;
	struct ccu_mux_config mux;
	struct ccu_common common;
};

#define CCU_GATE_INIT(_mask) { .mask = _mask }
#define CCU_FACTOR_INIT(_div, _mul) { .div = _div, .mul = _mul }
#define CCU_MUX_INIT(_shift, _width) { .shift = _shift, .width = _width }
#define CCU_DIV_INIT(_shift, _width) { .shift = _shift, .width = _width }

#define CCU_PARENT_HW(_parent)		{ .hw = &_parent.common.hw }
#define CCU_PARENT_NAME(_name)		{ .fw_name = #_name }

#define CCU_MIX_INITHW(_name, _parent, _ops, _flags)			\
	(&(struct clk_init_data) {					\
		.flags		= _flags,				\
		.name		= #_name,				\
		.parent_data	= (const struct clk_parent_data[])	\
					{ _parent },			\
		.num_parents	= 1,					\
		.ops		= &_ops,				\
	})

#define CCU_MIX_INITHW_PARENTS(_name, _parents, _ops, _flags)		\
	CLK_HW_INIT_PARENTS_DATA(#_name, _parents, &_ops, _flags)

#define CCU_GATE_DEFINE(_name, _parent, _reg, _gate_mask, _flags)		\
struct ccu_mix _name = {							\
	.gate	= CCU_GATE_INIT(_gate_mask),					\
	.common	= {								\
		.reg_ctrl	= _reg,						\
		.hw.init = CCU_MIX_INITHW(_name, _parent,			\
					  spacemit_ccu_gate_ops, _flags),	\
	}									\
}

#define CCU_FACTOR_DEFINE(_name, _parent, _div, _mul)				\
struct ccu_mix _name = {							\
	.factor	= CCU_FACTOR_INIT(_div, _mul),					\
	.common = {								\
		.hw.init = CCU_MIX_INITHW(_name, _parent,			\
					  spacemit_ccu_factor_ops, 0),		\
	}									\
}

#define CCU_MUX_DEFINE(_name, _parents, _reg, _shift, _width, _flags)		\
struct ccu_mix _name = {							\
	.mux	= CCU_MUX_INIT(_shift, _width),					\
	.common = {								\
		.reg_ctrl	= _reg,						\
		.hw.init = CCU_MIX_INITHW_PARENTS(_name, _parents,		\
						  spacemit_ccu_mux_ops,	_flags),\
	}									\
}

#define CCU_DIV_DEFINE(_name, _parent, _reg, _shift, _width, _flags)		\
struct ccu_mix _name = {							\
	.div	= CCU_DIV_INIT(_shift, _width),					\
	.common = {								\
		.reg_ctrl	= _reg,						\
		.hw.init = CCU_MIX_INITHW(_name, _parent,			\
					  spacemit_ccu_div_ops, _flags)		\
	}									\
}

#define CCU_GATE_FACTOR_DEFINE(_name, _parent,					\
			       _reg,						\
			       _gate_mask,					\
			       _div, _mul,					\
			       _flags)						\
struct ccu_mix _name = {							\
	.gate	= CCU_GATE_INIT(_gate_mask),					\
	.factor	= CCU_FACTOR_INIT(_div, _mul),					\
	.common = {								\
		.reg_ctrl	= _reg,						\
		.hw.init = CCU_MIX_INITHW(_name, _parent,			\
					  spacemit_ccu_gate_factor_ops, _flags)	\
	}									\
}

#define CCU_MUX_GATE_DEFINE(_name, _parents,					\
			    _reg,						\
			    _shift, _width,					\
			    _gate_mask,						\
			    _flags)						\
struct ccu_mix _name = {							\
	.gate	= CCU_GATE_INIT(_gate_mask),					\
	.mux	= CCU_MUX_INIT(_shift, _width),					\
	.common = {								\
		.reg_ctrl	= _reg,						\
		.hw.init = CCU_MIX_INITHW_PARENTS(_name, _parents,		\
						  spacemit_ccu_mux_gate_ops,	\
						  _flags),			\
	}									\
}

#define CCU_DIV_GATE_DEFINE(_name, _parent,					\
			    _reg,						\
			    _shift, _width,					\
			    _gate_mask,						\
			    _flags)						\
struct ccu_mix _name = {							\
	.gate	= CCU_GATE_INIT(_gate_mask),					\
	.div	= CCU_DIV_INIT(_shift, _width),					\
	.common = {								\
		.reg_ctrl	= _reg,						\
		.hw.init = CCU_MIX_INITHW(_name, _parent,			\
					  spacemit_ccu_div_gate_ops, _flags),	\
	}									\
}

#define CCU_DIV_MUX_GATE_DEFINE(_name, _parents,				\
				_reg_ctrl,					\
				_mshift, _mwidth, _muxshift, _muxwidth,		\
				_gate_mask,					\
				_flags)						\
struct ccu_mix _name = {							\
	.gate	= CCU_GATE_INIT(_gate_mask),					\
	.div	= CCU_DIV_INIT(_mshift, _mwidth),				\
	.mux	= CCU_MUX_INIT(_muxshift, _muxwidth),				\
	.common	= {								\
		.reg_ctrl	= _reg_ctrl,					\
		.hw.init = CCU_MIX_INITHW_PARENTS(_name, _parents,		\
						  spacemit_ccu_div_mux_gate_ops,\
						  _flags),			\
	},									\
}

#define CCU_DIV_SPLIT_FC_MUX_GATE_DEFINE(_name, _parents,			\
					 _reg_ctrl, _reg_fc,			\
					 _mshift, _mwidth,			\
					 _fc,					\
					 _muxshift, _muxwidth,			\
					 _gate_mask,				\
					 _flags)				\
struct ccu_mix _name = {							\
	.gate	= CCU_GATE_INIT(_gate_mask),					\
	.div	= CCU_DIV_INIT(_mshift, _mwidth),				\
	.mux	= CCU_MUX_INIT(_muxshift, _muxwidth),				\
	.common = {								\
		.reg_ctrl	= _reg_ctrl,					\
		.reg_fc		= _reg_fc,					\
		.fc		= _fc,						\
		.hw.init = CCU_MIX_INITHW_PARENTS(_name, _parents,		\
						  spacemit_ccu_div_mux_gate_ops,\
						  _flags),			\
	},									\
}

#define CCU_DIV_FC_MUX_GATE_DEFINE(_name, _parents,				\
				   _reg_ctrl,					\
				   _mshift, _mwidth,				\
				   _fc,						\
				   _muxshift, _muxwidth,			\
				   _gate_mask, _flags)		\
CCU_DIV_SPLIT_FC_MUX_GATE_DEFINE(_name, _parents, _reg_ctrl, _reg_ctrl,		\
				 _mshift, _mwidth, _fc, _muxshift, _muxwidth,		\
				 _gate_mask, _flags)

#define CCU_DIV_FC_MUX_DEFINE(_name, _parents,					\
			      _reg_ctrl,					\
			      _mshift, _mwidth,					\
			      _fc,						\
			      _muxshift, _muxwidth,				\
			      _flags)						\
struct ccu_mix _name = {							\
	.div	= CCU_DIV_INIT(_mshift, _mwidth),				\
	.mux	= CCU_MUX_INIT(_muxshift, _muxwidth),				\
	.common = {								\
		.reg_ctrl	= _reg_ctrl,					\
		.reg_fc		= _reg_ctrl,					\
		.fc		= _fc,						\
		.hw.init = CCU_MIX_INITHW_PARENTS(_name, _parents,		\
						  spacemit_ccu_div_mux_ops,	\
						  _flags),			\
	},									\
}

#define CCU_MUX_FC_DEFINE(_name, _parents,					\
			  _reg_ctrl,						\
			  _fc,							\
			  _muxshift, _muxwidth,					\
			  _flags)						\
struct ccu_mix _name = {							\
	.mux	= CCU_MUX_INIT(_muxshift, _muxwidth),				\
	.common = {								\
		.reg_ctrl	= _reg_ctrl,					\
		.reg_fc		= _reg_ctrl,					\
		.fc		= _fc,						\
		.hw.init = CCU_MIX_INITHW_PARENTS(_name, _parents,		\
						  spacemit_ccu_mux_ops,	_flags)	\
	},									\
}

static inline struct ccu_mix *hw_to_ccu_mix(struct clk_hw *hw)
{
	struct ccu_common *common = hw_to_ccu_common(hw);

	return container_of(common, struct ccu_mix, common);
}

extern const struct clk_ops spacemit_ccu_gate_ops, spacemit_ccu_factor_ops;
extern const struct clk_ops spacemit_ccu_mux_ops, spacemit_ccu_div_ops;

extern const struct clk_ops spacemit_ccu_gate_factor_ops;
extern const struct clk_ops spacemit_ccu_div_gate_ops;
extern const struct clk_ops spacemit_ccu_mux_gate_ops;
extern const struct clk_ops spacemit_ccu_div_mux_ops;

extern const struct clk_ops spacemit_ccu_div_mux_gate_ops;
#endif /* _CCU_DIV_H_ */
