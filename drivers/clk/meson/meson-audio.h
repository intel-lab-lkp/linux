/* SPDX-License-Identifier: (GPL-2.0 OR MIT) */

#ifndef __MESON_AUDIO_H__
#define __MESON_AUDIO_H__

#define AUD_PCLK_GATE(_name, _reg, _bit, _pdata) {			\
	.data = &(struct clk_regmap_gate_data){				\
		.offset = (_reg),					\
		.bit_idx = (_bit),					\
	},								\
	.hw.init = &(struct clk_init_data) {				\
		.name = "aud_"#_name,					\
		.ops = &clk_regmap_gate_ops,				\
		.parent_data = (_pdata),				\
		.num_parents = 1,					\
	},								\
}

#define AUD_GATE(_name, _reg, _bit, _pname, _iflags) {			\
	.data = &(struct clk_regmap_gate_data){				\
		.offset = (_reg),					\
		.bit_idx = (_bit),					\
	},								\
	.hw.init = &(struct clk_init_data) {				\
		.name = "aud_"#_name,					\
		.ops = &clk_regmap_gate_ops,				\
		.parent_names = (const char *[]){ #_pname },		\
		.num_parents = 1,					\
		.flags = CLK_DUTY_CYCLE_PARENT | (_iflags),		\
	},								\
}

#define AUD_MUX(_name, _reg, _mask, _shift, _dflags, _pdata, _iflags) {	\
	.data = &(struct clk_regmap_mux_data){				\
		.offset = (_reg),					\
		.mask = (_mask),					\
		.shift = (_shift),					\
		.flags = (_dflags),					\
	},								\
	.hw.init = &(struct clk_init_data){				\
		.name = "aud_"#_name,					\
		.ops = &clk_regmap_mux_ops,				\
		.parent_data = _pdata,					\
		.num_parents = ARRAY_SIZE(_pdata),			\
		.flags = CLK_DUTY_CYCLE_PARENT | (_iflags),		\
	},								\
}

#define AUD_DIV(_name, _reg, _shift, _width, _dflags, _pname, _iflags) { \
	.data = &(struct clk_regmap_div_data){				\
		.offset = (_reg),					\
		.shift = (_shift),					\
		.width = (_width),					\
		.flags = (_dflags),					\
	},								\
	.hw.init = &(struct clk_init_data){				\
		.name = "aud_"#_name,					\
		.ops = &clk_regmap_divider_ops,				\
		.parent_names = (const char *[]){ #_pname },		\
		.num_parents = 1,					\
		.flags = (_iflags),					\
	},								\
}

#define AUD_SCLK_DIV(_name, _reg, _div_shift, _div_width,		\
		     _hi_shift, _hi_width, _pname, _iflags) {		\
	.data = &(struct meson_sclk_div_data) {				\
		.div = {						\
			.reg_off = (_reg),				\
			.shift   = (_div_shift),			\
			.width   = (_div_width),			\
		},							\
		.hi = {							\
			.reg_off = (_reg),				\
			.shift   = (_hi_shift),				\
			.width   = (_hi_width),				\
		},							\
	},								\
	.hw.init = &(struct clk_init_data) {				\
		.name = "aud_"#_name,					\
		.ops = &meson_sclk_div_ops,				\
		.parent_names = (const char *[]){ #_pname },		\
		.num_parents = 1,					\
		.flags = (_iflags),					\
	},								\
}

#define AUD_TRIPHASE(_name, _reg, _width, _shift0, _shift1, _shift2,	\
		     _pname, _iflags) {					\
	.data = &(struct meson_clk_triphase_data) {			\
		.ph0 = {						\
			.reg_off = (_reg),				\
			.shift   = (_shift0),				\
			.width   = (_width),				\
		},							\
		.ph1 = {						\
			.reg_off = (_reg),				\
			.shift   = (_shift1),				\
			.width   = (_width),				\
		},							\
		.ph2 = {						\
			.reg_off = (_reg),				\
			.shift   = (_shift2),				\
			.width   = (_width),				\
		},							\
	},								\
	.hw.init = &(struct clk_init_data) {				\
		.name = "aud_"#_name,					\
		.ops = &meson_clk_triphase_ops,				\
		.parent_names = (const char *[]){ #_pname },		\
		.num_parents = 1,					\
		.flags = CLK_DUTY_CYCLE_PARENT | (_iflags),		\
	},								\
}

#define AUD_PHASE(_name, _reg, _width, _shift, _pname, _iflags) {	\
	.data = &(struct meson_clk_phase_data) {			\
		.ph = {							\
			.reg_off = (_reg),				\
			.shift   = (_shift),				\
			.width   = (_width),				\
		},							\
	},								\
	.hw.init = &(struct clk_init_data) {				\
		.name = "aud_"#_name,					\
		.ops = &meson_clk_phase_ops,				\
		.parent_names = (const char *[]){ #_pname },		\
		.num_parents = 1,					\
		.flags = (_iflags),					\
	},								\
}

#define AUD_SCLK_WS(_name, _reg, _width, _shift_ph, _shift_ws, _pname,	\
		    _iflags) {						\
	.data = &(struct meson_sclk_ws_inv_data) {			\
		.ph = {							\
			.reg_off = (_reg),				\
			.shift   = (_shift_ph),				\
			.width   = (_width),				\
		},							\
		.ws = {							\
			.reg_off = (_reg),				\
			.shift   = (_shift_ws),				\
			.width   = (_width),				\
		},							\
	},								\
	.hw.init = &(struct clk_init_data) {				\
		.name = "aud_"#_name,					\
		.ops = &meson_clk_phase_ops,				\
		.parent_names = (const char *[]){ #_pname },		\
		.num_parents = 1,					\
		.flags = (_iflags),					\
	},								\
}

#endif /* __MESON_AUDIO_H__ */
