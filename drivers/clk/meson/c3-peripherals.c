// SPDX-License-Identifier: GPL-2.0-only
/*
 * Amlogic C3 Peripherals Clock Controller Driver
 *
 * Copyright (c) 2023 Amlogic, inc.
 * Author: Chuan Liu <chuan.liu@amlogic.com>
 */

#include <linux/clk-provider.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/clk.h>
#include "clk-regmap.h"
#include "clk-dualdiv.h"
#include "c3-peripherals.h"
#include "meson-clkc-utils.h"
#include <dt-bindings/clock/amlogic,c3-peripherals-clkc.h>

#define __AML_CLK_MUX(_name, _reg, _mask, _shift, _table, _dflags,	\
			_ops, _pname, _pdata, _phw, _pnub, _iflags)	\
static struct clk_regmap _name = {					\
	.data = &(struct clk_regmap_mux_data){				\
		.offset = _reg,						\
		.mask = _mask,						\
		.shift = _shift,					\
		.table = _table,					\
		.flags = _dflags,					\
	},								\
	.hw.init = &(struct clk_init_data){				\
		.name = #_name,						\
		.ops = _ops,						\
		.parent_names = _pname,					\
		.parent_data = _pdata,					\
		.parent_hws = _phw,					\
		.num_parents = _pnub,					\
		.flags = _iflags,					\
	},								\
}

#define __AML_CLK_DIV(_name, _reg, _shift, _width, _table, _dflags,	\
			_ops, _pname, _pdata, _phw, _iflags)		\
static struct clk_regmap _name = {					\
	.data = &(struct clk_regmap_div_data){				\
		.offset = _reg,						\
		.shift = _shift,					\
		.width = _width,					\
		.table = _table,					\
		.flags = _dflags,					\
	},								\
	.hw.init = &(struct clk_init_data){				\
		.name = #_name,						\
		.ops = _ops,						\
		.parent_names = _pname,					\
		.parent_data = _pdata,					\
		.parent_hws = (const struct clk_hw *[]) { _phw },	\
		.num_parents = 1,					\
		.flags = _iflags,					\
	},								\
}

#define __AML_CLK_GATE(_name, _reg, _bit, _dflags,			\
			 _ops, _pname, _pdata, _phw, _iflags)		\
static struct clk_regmap _name = {					\
	.data = &(struct clk_regmap_gate_data){				\
		.offset = _reg,						\
		.bit_idx = _bit,					\
		.flags = _dflags,					\
	},								\
	.hw.init = &(struct clk_init_data) {				\
		.name = #_name,						\
		.ops = _ops,						\
		.parent_names = _pname,					\
		.parent_data = _pdata,					\
		.parent_hws = (const struct clk_hw *[]) { _phw },	\
		.num_parents = 1,					\
		.flags = _iflags,					\
	},								\
}

#define AML_REG_PARM(_member_name, _reg, _shift, _width)		\
	._member_name = {						\
		.reg_off = _reg,					\
		.shift   = _shift,					\
		.width   = _width,					\
	}

#define __AML_CLK_DUALDIV(_name, _n1_reg, _n1_shift, _n1_width,	\
			    _n2_reg, _n2_shift, _n2_width,		\
			    _m1_reg, _m1_shift, _m1_width,		\
			    _m2_reg, _m2_shift, _m2_width,		\
			    _d_reg, _d_shift, _d_width, _dualdiv_table,	\
			    _ops, _pname, _pdata, _phw, _iflags)	\
static struct clk_regmap _name = {					\
	.data = &(struct meson_clk_dualdiv_data){			\
		AML_REG_PARM(n1,					\
			_n1_reg, _n1_shift, _n1_width),			\
		AML_REG_PARM(n2,					\
			_n2_reg, _n2_shift, _n2_width),			\
		AML_REG_PARM(m1,					\
			_m1_reg, _m1_shift, _m1_width),			\
		AML_REG_PARM(m2,					\
			_m2_reg, _m2_shift, _m2_width),			\
		AML_REG_PARM(dual,					\
			_d_reg, _d_shift, _d_width),			\
		.table = _dualdiv_table,				\
	},								\
	.hw.init = &(struct clk_init_data) {				\
		.name = #_name,						\
		.ops = _ops,						\
		.parent_names = _pname,					\
		.parent_data = _pdata,					\
		.parent_hws = (const struct clk_hw *[]) { _phw },	\
		.num_parents = 1,					\
		.flags = _iflags,					\
	},								\
}

#define __AML_CLK_FIXED_FACTOR(_name, _mult, _div, _ops,		\
				 _pname, _pdata, _phw, _iflags)		\
static struct clk_fixed_factor _name = {				\
	.mult = _mult,							\
	.div = _div,							\
	.hw.init = &(struct clk_init_data){				\
		.name = #_name,						\
		.ops = _ops,						\
		.parent_names = _pname,					\
		.parent_data = _pdata,					\
		.parent_hws = (const struct clk_hw *[]) { _phw },	\
		.num_parents = 1,					\
		.flags = _iflags,					\
	},								\
}

#define AML_CLK_MUX_RW(_name, _reg, _mask, _shift, _table, _dflags,	\
			 _pdata, _iflags)				\
	__AML_CLK_MUX(_name, _reg, _mask, _shift, _table, _dflags,	\
			&clk_regmap_mux_ops, NULL, _pdata, NULL,	\
			ARRAY_SIZE(_pdata), _iflags)

#define AML_CLK_DIV_RW(_name, _reg, _shift, _width, _table, _dflags,	\
			 _phw, _iflags)					\
	__AML_CLK_DIV(_name, _reg, _shift, _width, _table, _dflags,	\
			&clk_regmap_divider_ops, NULL, NULL,		\
			_phw, _iflags)

#define AML_CLK_GATE_RW(_name, _reg, _bit, _dflags, _phw, _iflags)	\
	__AML_CLK_GATE(_name, _reg, _bit, _dflags,			\
			 &clk_regmap_gate_ops, NULL, NULL, _phw,	\
			 _iflags)

#define AML_CLK_FIXED_FACTOR(_name, _mult, _div, _phw, _iflags)	\
	__AML_CLK_FIXED_FACTOR(_name, _mult, _div,			\
				 &clk_fixed_factor_ops, NULL, NULL,	\
				 _phw, _iflags)

#define AML_CLK_DUALDIV_RW(_name, _n1_reg, _n1_shift, _n1_width,	\
			     _n2_reg, _n2_shift, _n2_width,		\
			     _m1_reg, _m1_shift, _m1_width,		\
			     _m2_reg, _m2_shift, _m2_width,		\
			     _d_reg, _d_shift, _d_width, _dualdiv_table,\
			     _phw, _iflags)				\
	__AML_CLK_DUALDIV(_name, _n1_reg, _n1_shift, _n1_width,		\
			    _n2_reg, _n2_shift, _n2_width,		\
			    _m1_reg, _m1_shift, _m1_width,		\
			    _m2_reg, _m2_shift, _m2_width,		\
			    _d_reg, _d_shift, _d_width, _dualdiv_table,	\
			    &meson_clk_dualdiv_ops,			\
			    NULL, NULL, _phw, _iflags)

static struct clk_regmap pll_in = {
	.data = &(struct clk_regmap_gate_data){
		.offset = OSCIN_CTRL,
		.bit_idx = 4,
	},
	.hw.init = &(struct clk_init_data) {
		.name = "pll_in",
		.ops = &clk_regmap_gate_ro_ops,
		.parent_data = &(const struct clk_parent_data) {
			.fw_name = "xtal",
		},
		.num_parents = 1,
	},
};

static struct clk_regmap mclk_pll_in = {
	.data = &(struct clk_regmap_gate_data){
		.offset = OSCIN_CTRL,
		.bit_idx = 8,
	},
	.hw.init = &(struct clk_init_data) {
		.name = "mclk_pll_in",
		.ops = &clk_regmap_gate_ro_ops,
		.parent_data = &(const struct clk_parent_data) {
			.fw_name = "xtal",
		},
		.num_parents = 1,
	},
};

static const struct clk_parent_data rtc_xtal_clkin_parent = {
	.fw_name = "xtal",
};

__AML_CLK_GATE(rtc_xtal_clkin, RTC_BY_OSCIN_CTRL0, 31, 0,
		 &clk_regmap_gate_ops, NULL, &rtc_xtal_clkin_parent, NULL, 0);

static const struct meson_clk_dualdiv_param rtc_32k_div_table[] = {
	{ 733, 732, 8, 11, 1 },
	{ /* sentinel */ }
};

AML_CLK_DUALDIV_RW(rtc_32k_div, RTC_BY_OSCIN_CTRL0, 0,  12,
			RTC_BY_OSCIN_CTRL0, 12, 12,
			RTC_BY_OSCIN_CTRL1, 0,  12,
			RTC_BY_OSCIN_CTRL1, 12, 12,
			RTC_BY_OSCIN_CTRL0, 28, 1, rtc_32k_div_table,
			&rtc_xtal_clkin.hw, 0);

static const struct clk_parent_data rtc_32k_mux_parent_data[] = {
	{ .hw = &rtc_32k_div.hw },
	{ .hw = &rtc_xtal_clkin.hw }
};

AML_CLK_MUX_RW(rtc_32k_mux, RTC_BY_OSCIN_CTRL1, 0x1, 24, NULL, 0,
	       rtc_32k_mux_parent_data, CLK_SET_RATE_PARENT);

AML_CLK_GATE_RW(rtc_32k, RTC_BY_OSCIN_CTRL0, 30, 0,
		&rtc_32k_mux.hw, CLK_SET_RATE_PARENT);

static const struct clk_parent_data rtc_clk_mux_parent_data[] = {
	{ .fw_name = "xtal" },
	{ .hw = &rtc_32k.hw }
};

AML_CLK_MUX_RW(rtc_clk, RTC_CTRL, 0x3, 0, NULL, 0,
	       rtc_clk_mux_parent_data, CLK_SET_RATE_PARENT);

/* sys_clk */
#define AML_CLK_GATE_SYS_CLK(_name, _reg, _bit)			\
	AML_CLK_GATE_RW(_name, _reg, _bit, 0, &sys_clk.hw, CLK_IGNORE_UNUSED)

/* axi_clk */
#define AML_CLK_GATE_AXI_CLK(_name, _reg, _bit)			\
	AML_CLK_GATE_RW(_name, _reg, _bit, 0, &axi_clk.hw, CLK_IGNORE_UNUSED)

/*
 * Some clocks have multiple clock sources, and the parent clock and index are
 * discontinuous, Some channels corresponding to the clock index are not
 * actually connected inside the chip, or the clock source is invalid.
 */
static u32 sys_axi_parent_table[] = { 0, 2, 3, 4, 7 };
static const struct clk_parent_data sys_axi_parent_data[] = {
	{ .fw_name = "xtal" },
	{ .fw_name = "fclk_div3" },
	{ .fw_name = "fclk_div4" },
	{ .fw_name = "fclk_div5" },
	{ .hw = &rtc_clk.hw }
};

AML_CLK_MUX_RW(sys_a_sel, SYS_CLK_CTRL0, 0x7, 10,
		 sys_axi_parent_table, 0, sys_axi_parent_data, 0);
AML_CLK_DIV_RW(sys_a_div, SYS_CLK_CTRL0, 0, 10, NULL, 0,
		 &sys_a_sel.hw, 0);
AML_CLK_GATE_RW(sys_a, SYS_CLK_CTRL0, 13, 0,
		  &sys_a_div.hw, CLK_IGNORE_UNUSED);

AML_CLK_MUX_RW(sys_b_sel, SYS_CLK_CTRL0, 0x7, 26,
		 sys_axi_parent_table, 0, sys_axi_parent_data, 0);
AML_CLK_DIV_RW(sys_b_div, SYS_CLK_CTRL0, 16, 10, NULL, 0,
		 &sys_b_sel.hw, 0);
AML_CLK_GATE_RW(sys_b, SYS_CLK_CTRL0, 29, 0,
		  &sys_b_div.hw, CLK_IGNORE_UNUSED);

static const struct clk_parent_data sys_clk_parent_data[] = {
	{ .hw = &sys_a.hw },
	{ .hw = &sys_b.hw }
};

AML_CLK_MUX_RW(sys_clk, SYS_CLK_CTRL0, 1, 15, NULL, 0,
		 sys_clk_parent_data, 0);

AML_CLK_MUX_RW(axi_a_sel, AXI_CLK_CTRL0, 0x7, 10,
		 sys_axi_parent_table, 0, sys_axi_parent_data, 0);
AML_CLK_DIV_RW(axi_a_div, AXI_CLK_CTRL0, 0, 10, NULL, 0,
		 &axi_a_sel.hw, 0);
AML_CLK_GATE_RW(axi_a, AXI_CLK_CTRL0, 13, 0,
		  &axi_a_div.hw, CLK_IGNORE_UNUSED);

AML_CLK_MUX_RW(axi_b_sel, AXI_CLK_CTRL0, 0x7, 26,
		 sys_axi_parent_table, 0, sys_axi_parent_data, 0);
AML_CLK_DIV_RW(axi_b_div, AXI_CLK_CTRL0, 16, 10, NULL, 0,
		 &axi_b_sel.hw, 0);
AML_CLK_GATE_RW(axi_b, AXI_CLK_CTRL0, 29, 0,
		  &axi_b_div.hw, CLK_IGNORE_UNUSED);

static const struct clk_parent_data axi_clk_parent_data[] = {
	{ .hw = &axi_a.hw },
	{ .hw = &axi_b.hw }
};

AML_CLK_MUX_RW(axi_clk, AXI_CLK_CTRL0, 1, 15, NULL, 0,
		 axi_clk_parent_data, 0);

AML_CLK_GATE_SYS_CLK(sys_reset_ctrl, SYS_CLK_EN0_REG0, 1);
AML_CLK_GATE_SYS_CLK(sys_pwr_ctrl, SYS_CLK_EN0_REG0, 3);
AML_CLK_GATE_SYS_CLK(sys_pad_ctrl, SYS_CLK_EN0_REG0, 4);
AML_CLK_GATE_SYS_CLK(sys_ctrl, SYS_CLK_EN0_REG0, 5);
AML_CLK_GATE_SYS_CLK(sys_ts_pll, SYS_CLK_EN0_REG0, 6);
AML_CLK_GATE_SYS_CLK(sys_dev_arb, SYS_CLK_EN0_REG0, 7);
AML_CLK_GATE_SYS_CLK(sys_mmc_pclk, SYS_CLK_EN0_REG0, 8);
AML_CLK_GATE_SYS_CLK(sys_capu, SYS_CLK_EN0_REG0, 9);
AML_CLK_GATE_SYS_CLK(sys_cpu_ctrl, SYS_CLK_EN0_REG0, 11);
AML_CLK_GATE_SYS_CLK(sys_jtag_ctrl, SYS_CLK_EN0_REG0, 12);
AML_CLK_GATE_SYS_CLK(sys_ir_ctrl, SYS_CLK_EN0_REG0, 13);
AML_CLK_GATE_SYS_CLK(sys_irq_ctrl, SYS_CLK_EN0_REG0, 14);
AML_CLK_GATE_SYS_CLK(sys_msr_clk, SYS_CLK_EN0_REG0, 15);
AML_CLK_GATE_SYS_CLK(sys_rom, SYS_CLK_EN0_REG0, 16);
AML_CLK_GATE_SYS_CLK(sys_uart_f, SYS_CLK_EN0_REG0, 17);
AML_CLK_GATE_SYS_CLK(sys_cpu_apb, SYS_CLK_EN0_REG0, 18);
AML_CLK_GATE_SYS_CLK(sys_rsa, SYS_CLK_EN0_REG0, 19);
AML_CLK_GATE_SYS_CLK(sys_sar_adc, SYS_CLK_EN0_REG0, 20);
AML_CLK_GATE_SYS_CLK(sys_startup, SYS_CLK_EN0_REG0, 21);
AML_CLK_GATE_SYS_CLK(sys_secure, SYS_CLK_EN0_REG0, 22);
AML_CLK_GATE_SYS_CLK(sys_spifc, SYS_CLK_EN0_REG0, 23);
AML_CLK_GATE_SYS_CLK(sys_nna, SYS_CLK_EN0_REG0, 25);
AML_CLK_GATE_SYS_CLK(sys_eth_mac, SYS_CLK_EN0_REG0, 26);
AML_CLK_GATE_SYS_CLK(sys_gic, SYS_CLK_EN0_REG0, 27);
AML_CLK_GATE_SYS_CLK(sys_rama, SYS_CLK_EN0_REG0, 28);
AML_CLK_GATE_SYS_CLK(sys_big_nic, SYS_CLK_EN0_REG0, 29);
AML_CLK_GATE_SYS_CLK(sys_ramb, SYS_CLK_EN0_REG0, 30);
AML_CLK_GATE_SYS_CLK(sys_audio_pclk, SYS_CLK_EN0_REG0, 31);
AML_CLK_GATE_SYS_CLK(sys_pwm_kl, SYS_CLK_EN0_REG1, 0);
AML_CLK_GATE_SYS_CLK(sys_pwm_ij, SYS_CLK_EN0_REG1, 1);
AML_CLK_GATE_SYS_CLK(sys_usb, SYS_CLK_EN0_REG1, 2);
AML_CLK_GATE_SYS_CLK(sys_sd_emmc_a, SYS_CLK_EN0_REG1, 3);
AML_CLK_GATE_SYS_CLK(sys_sd_emmc_c, SYS_CLK_EN0_REG1, 4);
AML_CLK_GATE_SYS_CLK(sys_pwm_ab, SYS_CLK_EN0_REG1, 5);
AML_CLK_GATE_SYS_CLK(sys_pwm_cd, SYS_CLK_EN0_REG1, 6);
AML_CLK_GATE_SYS_CLK(sys_pwm_ef, SYS_CLK_EN0_REG1, 7);
AML_CLK_GATE_SYS_CLK(sys_pwm_gh, SYS_CLK_EN0_REG1, 8);
AML_CLK_GATE_SYS_CLK(sys_spicc_1, SYS_CLK_EN0_REG1, 9);
AML_CLK_GATE_SYS_CLK(sys_spicc_0, SYS_CLK_EN0_REG1, 10);
AML_CLK_GATE_SYS_CLK(sys_uart_a, SYS_CLK_EN0_REG1, 11);
AML_CLK_GATE_SYS_CLK(sys_uart_b, SYS_CLK_EN0_REG1, 12);
AML_CLK_GATE_SYS_CLK(sys_uart_c, SYS_CLK_EN0_REG1, 13);
AML_CLK_GATE_SYS_CLK(sys_uart_d, SYS_CLK_EN0_REG1, 14);
AML_CLK_GATE_SYS_CLK(sys_uart_e, SYS_CLK_EN0_REG1, 15);
AML_CLK_GATE_SYS_CLK(sys_i2c_m_a, SYS_CLK_EN0_REG1, 16);
AML_CLK_GATE_SYS_CLK(sys_i2c_m_b, SYS_CLK_EN0_REG1, 17);
AML_CLK_GATE_SYS_CLK(sys_i2c_m_c, SYS_CLK_EN0_REG1, 18);
AML_CLK_GATE_SYS_CLK(sys_i2c_m_d, SYS_CLK_EN0_REG1, 19);
AML_CLK_GATE_SYS_CLK(sys_i2c_s_a, SYS_CLK_EN0_REG1, 20);
AML_CLK_GATE_SYS_CLK(sys_rtc, SYS_CLK_EN0_REG1, 21);
AML_CLK_GATE_SYS_CLK(sys_ge2d, SYS_CLK_EN0_REG1, 22);
AML_CLK_GATE_SYS_CLK(sys_isp, SYS_CLK_EN0_REG1, 23);
AML_CLK_GATE_SYS_CLK(sys_gpv_isp_nic, SYS_CLK_EN0_REG1, 24);
AML_CLK_GATE_SYS_CLK(sys_gpv_cve_nic, SYS_CLK_EN0_REG1, 25);
AML_CLK_GATE_SYS_CLK(sys_mipi_dsi_host, SYS_CLK_EN0_REG1, 26);
AML_CLK_GATE_SYS_CLK(sys_mipi_dsi_phy, SYS_CLK_EN0_REG1, 27);
AML_CLK_GATE_SYS_CLK(sys_eth_phy, SYS_CLK_EN0_REG1, 28);
AML_CLK_GATE_SYS_CLK(sys_acodec, SYS_CLK_EN0_REG1, 29);
AML_CLK_GATE_SYS_CLK(sys_dwap, SYS_CLK_EN0_REG1, 30);
AML_CLK_GATE_SYS_CLK(sys_dos, SYS_CLK_EN0_REG1, 31);
AML_CLK_GATE_SYS_CLK(sys_cve, SYS_CLK_EN0_REG2, 0);
AML_CLK_GATE_SYS_CLK(sys_vout, SYS_CLK_EN0_REG2, 1);
AML_CLK_GATE_SYS_CLK(sys_vc9000e, SYS_CLK_EN0_REG2, 2);
AML_CLK_GATE_SYS_CLK(sys_pwm_mn, SYS_CLK_EN0_REG2, 3);
AML_CLK_GATE_SYS_CLK(sys_sd_emmc_b, SYS_CLK_EN0_REG2, 4);
AML_CLK_GATE_AXI_CLK(axi_sys_nic, AXI_CLK_EN0, 2);
AML_CLK_GATE_AXI_CLK(axi_isp_nic, AXI_CLK_EN0, 3);
AML_CLK_GATE_AXI_CLK(axi_cve_nic, AXI_CLK_EN0, 4);
AML_CLK_GATE_AXI_CLK(axi_ramb, AXI_CLK_EN0, 5);
AML_CLK_GATE_AXI_CLK(axi_rama, AXI_CLK_EN0, 6);
AML_CLK_GATE_AXI_CLK(axi_cpu_dmc, AXI_CLK_EN0, 7);
AML_CLK_GATE_AXI_CLK(axi_nic, AXI_CLK_EN0, 8);
AML_CLK_GATE_AXI_CLK(axi_dma, AXI_CLK_EN0, 9);
AML_CLK_GATE_AXI_CLK(axi_mux_nic, AXI_CLK_EN0, 10);
AML_CLK_GATE_AXI_CLK(axi_capu, AXI_CLK_EN0, 11);
AML_CLK_GATE_AXI_CLK(axi_cve, AXI_CLK_EN0, 12);
AML_CLK_GATE_AXI_CLK(axi_dev1_dmc, AXI_CLK_EN0, 13);
AML_CLK_GATE_AXI_CLK(axi_dev0_dmc, AXI_CLK_EN0, 14);
AML_CLK_GATE_AXI_CLK(axi_dsp_dmc, AXI_CLK_EN0, 15);

/*
 * clk_12_24m model
 *
 *          |------|     |-----| clk_12m_24m |-----|
 * xtal---->| gate |---->| div |------------>| pad |
 *          |------|     |-----|             |-----|
 */
static const struct clk_parent_data clk_12_24m_in_parent = {
	.fw_name = "xtal",
};

__AML_CLK_GATE(clk_12_24m_in, CLK12_24_CTRL, 11, 0, &clk_regmap_gate_ops,
		 NULL, &clk_12_24m_in_parent, NULL, 0);
AML_CLK_DIV_RW(clk_12_24m, CLK12_24_CTRL, 10, 1, NULL, 0, &clk_12_24m_in.hw, 0);

static const struct clk_parent_data fclk_25m_div_parent = {
	.fw_name = "fixed_pll",
};

__AML_CLK_DIV(fclk_25m_div, CLK12_24_CTRL, 0, 8, NULL, 0,
		&clk_regmap_divider_ops, NULL, &fclk_25m_div_parent, NULL, 0);
AML_CLK_GATE_RW(fclk_25m, CLK12_24_CTRL, 12, 0, &fclk_25m_div.hw,
		  CLK_SET_RATE_PARENT);

/* generate clock output */
static u32 gen_parent_table[] = { 0, 1, 5, 7 };
static const struct clk_parent_data gen_parent_data[] = {
	{ .fw_name = "xtal" },
	{ .hw = &rtc_clk.hw },
	{ .fw_name = "gp0_pll" },
	{ .fw_name = "hifi_pll" }
};

AML_CLK_MUX_RW(gen_sel, GEN_CLK_CTRL, 0x1f, 12,
		 gen_parent_table, 0, gen_parent_data, 0);
AML_CLK_DIV_RW(gen_div, GEN_CLK_CTRL, 0, 11, NULL, 0,
		 &gen_sel.hw, CLK_SET_RATE_PARENT);
AML_CLK_GATE_RW(gen, GEN_CLK_CTRL, 11, 0,
		  &gen_div.hw, CLK_SET_RATE_PARENT);

static const struct clk_parent_data saradc_parent_data[] = {
	{ .fw_name = "xtal" },
	{ .hw = &sys_clk.hw }
};

AML_CLK_MUX_RW(saradc_sel, SAR_CLK_CTRL0, 0x1, 9,
		 NULL, 0, saradc_parent_data, 0);
AML_CLK_DIV_RW(saradc_div, SAR_CLK_CTRL0, 0, 8, NULL, 0,
		 &saradc_sel.hw, CLK_SET_RATE_PARENT);
AML_CLK_GATE_RW(saradc, SAR_CLK_CTRL0, 8, 0,
		  &saradc_div.hw, CLK_SET_RATE_PARENT);

static u32 pwm_parent_table[] = { 0, 2, 3 };
static const struct clk_parent_data pwm_parent_data[] = {
	{ .fw_name = "xtal" },
	{ .fw_name = "fclk_div4" },
	{ .fw_name = "fclk_div3" }
};

AML_CLK_MUX_RW(pwm_a_sel, PWM_CLK_AB_CTRL, 0x3, 9,
		 pwm_parent_table, 0, pwm_parent_data, 0);
AML_CLK_DIV_RW(pwm_a_div, PWM_CLK_AB_CTRL, 0, 8, NULL, 0,
		 &pwm_a_sel.hw, CLK_SET_RATE_PARENT);
AML_CLK_GATE_RW(pwm_a, PWM_CLK_AB_CTRL, 8, 0,
		  &pwm_a_div.hw, CLK_SET_RATE_PARENT);

AML_CLK_MUX_RW(pwm_b_sel, PWM_CLK_AB_CTRL, 0x3, 25,
		 pwm_parent_table, 0, pwm_parent_data, 0);
AML_CLK_DIV_RW(pwm_b_div, PWM_CLK_AB_CTRL, 16, 8, NULL, 0,
		 &pwm_b_sel.hw, CLK_SET_RATE_PARENT);
AML_CLK_GATE_RW(pwm_b, PWM_CLK_AB_CTRL, 24, 0,
		  &pwm_b_div.hw, CLK_SET_RATE_PARENT);

AML_CLK_MUX_RW(pwm_c_sel, PWM_CLK_CD_CTRL, 0x3, 9,
		 pwm_parent_table, 0, pwm_parent_data, 0);
AML_CLK_DIV_RW(pwm_c_div, PWM_CLK_CD_CTRL, 0, 8, NULL, 0,
		 &pwm_c_sel.hw, CLK_SET_RATE_PARENT);
AML_CLK_GATE_RW(pwm_c, PWM_CLK_CD_CTRL, 8, 0,
		  &pwm_c_div.hw, CLK_SET_RATE_PARENT);

AML_CLK_MUX_RW(pwm_d_sel, PWM_CLK_CD_CTRL, 0x3, 25,
		 pwm_parent_table, 0, pwm_parent_data, 0);
AML_CLK_DIV_RW(pwm_d_div, PWM_CLK_CD_CTRL, 16, 8, NULL, 0,
		 &pwm_d_sel.hw, CLK_SET_RATE_PARENT);
AML_CLK_GATE_RW(pwm_d, PWM_CLK_CD_CTRL, 24, 0,
		  &pwm_d_div.hw, CLK_SET_RATE_PARENT);

AML_CLK_MUX_RW(pwm_e_sel, PWM_CLK_EF_CTRL, 0x3, 9,
		 pwm_parent_table, 0, pwm_parent_data, 0);
AML_CLK_DIV_RW(pwm_e_div, PWM_CLK_EF_CTRL, 0, 8, NULL, 0,
		 &pwm_e_sel.hw, CLK_SET_RATE_PARENT);
AML_CLK_GATE_RW(pwm_e, PWM_CLK_EF_CTRL, 8, 0,
		  &pwm_e_div.hw, CLK_SET_RATE_PARENT);

AML_CLK_MUX_RW(pwm_f_sel, PWM_CLK_EF_CTRL, 0x3, 25,
		 pwm_parent_table, 0, pwm_parent_data, 0);
AML_CLK_DIV_RW(pwm_f_div, PWM_CLK_EF_CTRL, 16, 8, NULL, 0,
		 &pwm_f_sel.hw, CLK_SET_RATE_PARENT);
AML_CLK_GATE_RW(pwm_f, PWM_CLK_EF_CTRL, 24, 0,
		  &pwm_f_div.hw, CLK_SET_RATE_PARENT);

AML_CLK_MUX_RW(pwm_g_sel, PWM_CLK_GH_CTRL, 0x3, 9,
		 pwm_parent_table, 0, pwm_parent_data, 0);
AML_CLK_DIV_RW(pwm_g_div, PWM_CLK_GH_CTRL, 0, 8, NULL, 0,
		 &pwm_g_sel.hw, CLK_SET_RATE_PARENT);
AML_CLK_GATE_RW(pwm_g, PWM_CLK_GH_CTRL, 8, 0,
		  &pwm_g_div.hw, CLK_SET_RATE_PARENT);

AML_CLK_MUX_RW(pwm_h_sel, PWM_CLK_GH_CTRL, 0x3, 25,
		 pwm_parent_table, 0, pwm_parent_data, 0);
AML_CLK_DIV_RW(pwm_h_div, PWM_CLK_GH_CTRL, 16, 8, NULL, 0,
		 &pwm_h_sel.hw, CLK_SET_RATE_PARENT);
AML_CLK_GATE_RW(pwm_h, PWM_CLK_GH_CTRL, 24, 0,
		  &pwm_h_div.hw, CLK_SET_RATE_PARENT);

AML_CLK_MUX_RW(pwm_i_sel, PWM_CLK_IJ_CTRL, 0x3, 9,
		 pwm_parent_table, 0, pwm_parent_data, 0);
AML_CLK_DIV_RW(pwm_i_div, PWM_CLK_IJ_CTRL, 0, 8, NULL, 0,
		 &pwm_i_sel.hw, CLK_SET_RATE_PARENT);
AML_CLK_GATE_RW(pwm_i, PWM_CLK_IJ_CTRL, 8, 0,
		  &pwm_i_div.hw, CLK_SET_RATE_PARENT);

AML_CLK_MUX_RW(pwm_j_sel, PWM_CLK_IJ_CTRL, 0x3, 25,
		 pwm_parent_table, 0, pwm_parent_data, 0);
AML_CLK_DIV_RW(pwm_j_div, PWM_CLK_IJ_CTRL, 16, 8, NULL, 0,
		 &pwm_j_sel.hw, CLK_SET_RATE_PARENT);
AML_CLK_GATE_RW(pwm_j, PWM_CLK_IJ_CTRL, 24, 0,
		  &pwm_j_div.hw, CLK_SET_RATE_PARENT);

AML_CLK_MUX_RW(pwm_k_sel, PWM_CLK_KL_CTRL, 0x3, 9,
		 pwm_parent_table, 0, pwm_parent_data, 0);
AML_CLK_DIV_RW(pwm_k_div, PWM_CLK_KL_CTRL, 0, 8, NULL, 0,
		 &pwm_k_sel.hw, CLK_SET_RATE_PARENT);
AML_CLK_GATE_RW(pwm_k, PWM_CLK_KL_CTRL, 8, 0,
		  &pwm_k_div.hw, CLK_SET_RATE_PARENT);

AML_CLK_MUX_RW(pwm_l_sel, PWM_CLK_KL_CTRL, 0x3, 25,
		 pwm_parent_table, 0, pwm_parent_data, 0);
AML_CLK_DIV_RW(pwm_l_div, PWM_CLK_KL_CTRL, 16, 8, NULL, 0,
		 &pwm_l_sel.hw, CLK_SET_RATE_PARENT);
AML_CLK_GATE_RW(pwm_l, PWM_CLK_KL_CTRL, 24, 0,
		  &pwm_l_div.hw, CLK_SET_RATE_PARENT);

AML_CLK_MUX_RW(pwm_m_sel, PWM_CLK_MN_CTRL, 0x3, 9,
		 pwm_parent_table, 0, pwm_parent_data, 0);
AML_CLK_DIV_RW(pwm_m_div, PWM_CLK_MN_CTRL, 0, 8, NULL, 0,
		 &pwm_m_sel.hw, CLK_SET_RATE_PARENT);
AML_CLK_GATE_RW(pwm_m, PWM_CLK_MN_CTRL, 8, 0,
		  &pwm_m_div.hw, CLK_SET_RATE_PARENT);

AML_CLK_MUX_RW(pwm_n_sel, PWM_CLK_MN_CTRL, 0x3, 25,
		 pwm_parent_table, 0, pwm_parent_data, 0);
AML_CLK_DIV_RW(pwm_n_div, PWM_CLK_MN_CTRL, 16, 8, NULL, 0,
		 &pwm_n_sel.hw, CLK_SET_RATE_PARENT);
AML_CLK_GATE_RW(pwm_n, PWM_CLK_MN_CTRL, 24, 0,
		  &pwm_n_div.hw, CLK_SET_RATE_PARENT);

static const struct clk_parent_data spicc_parent_data[] = {
	{ .fw_name = "xtal" },
	{ .hw = &sys_clk.hw },
	{ .fw_name = "fclk_div4" },
	{ .fw_name = "fclk_div3" },
	{ .fw_name = "fclk_div2" },
	{ .fw_name = "fclk_div5" },
	{ .fw_name = "fclk_div7" }
};

AML_CLK_MUX_RW(spicc_a_sel, SPICC_CLK_CTRL, 0x7, 7,
		 NULL, 0, spicc_parent_data, 0);
AML_CLK_DIV_RW(spicc_a_div, SPICC_CLK_CTRL, 0, 6, NULL, 0,
		 &spicc_a_sel.hw, CLK_SET_RATE_PARENT);
AML_CLK_GATE_RW(spicc_a, SPICC_CLK_CTRL, 6, 0,
		  &spicc_a_div.hw, CLK_SET_RATE_PARENT);

AML_CLK_MUX_RW(spicc_b_sel, SPICC_CLK_CTRL, 0x7, 23,
		 NULL, 0, spicc_parent_data, 0);
AML_CLK_DIV_RW(spicc_b_div, SPICC_CLK_CTRL, 16, 6, NULL, 0,
		 &spicc_b_sel.hw, CLK_SET_RATE_PARENT);
AML_CLK_GATE_RW(spicc_b, SPICC_CLK_CTRL, 22, 0,
		  &spicc_b_div.hw, CLK_SET_RATE_PARENT);

static const struct clk_parent_data spifc_parent_data[] = {
	{ .fw_name = "gp0_pll" },
	{ .fw_name = "fclk_div2" },
	{ .fw_name = "fclk_div3" },
	{ .fw_name = "fclk_div2p5" },
	{ .fw_name = "hifi_pll" },
	{ .fw_name = "fclk_div4" },
	{ .fw_name = "fclk_div5" },
	{ .fw_name = "fclk_div7" }
};

AML_CLK_MUX_RW(spifc_sel, SPIFC_CLK_CTRL, 0x7, 9,
		 NULL, 0, spifc_parent_data, 0);
AML_CLK_DIV_RW(spifc_div, SPIFC_CLK_CTRL, 0, 7, NULL, 0,
		 &spifc_sel.hw, CLK_SET_RATE_PARENT);
AML_CLK_GATE_RW(spifc, SPIFC_CLK_CTRL, 8, 0,
		  &spifc_div.hw, CLK_SET_RATE_PARENT);

static u32 emmc_parent_table[] = { 0, 1, 2, 3, 4, 5, 7 };
static const struct clk_parent_data emmc_parent_data[] = {
	{ .fw_name = "xtal" },
	{ .fw_name = "fclk_div2" },
	{ .fw_name = "fclk_div3" },
	{ .fw_name = "hifi_pll" },
	{ .fw_name = "fclk_div2p5" },
	{ .fw_name = "fclk_div4" },
	{ .fw_name = "gp0_pll" }
};

AML_CLK_MUX_RW(sd_emmc_a_sel, SD_EMMC_CLK_CTRL, 0x7, 9,
		 emmc_parent_table, 0, emmc_parent_data, 0);
AML_CLK_DIV_RW(sd_emmc_a_div, SD_EMMC_CLK_CTRL, 0, 7, NULL, 0,
		 &sd_emmc_a_sel.hw, CLK_SET_RATE_PARENT);
AML_CLK_GATE_RW(sd_emmc_a, SD_EMMC_CLK_CTRL, 7, 0,
		  &sd_emmc_a_div.hw, CLK_SET_RATE_PARENT);

AML_CLK_MUX_RW(sd_emmc_b_sel, SD_EMMC_CLK_CTRL, 0x7, 25,
		 emmc_parent_table, 0, emmc_parent_data, 0);
AML_CLK_DIV_RW(sd_emmc_b_div, SD_EMMC_CLK_CTRL, 16, 7, NULL, 0,
		 &sd_emmc_b_sel.hw, CLK_SET_RATE_PARENT);
AML_CLK_GATE_RW(sd_emmc_b, SD_EMMC_CLK_CTRL, 23, 0,
		  &sd_emmc_b_div.hw, CLK_SET_RATE_PARENT);

AML_CLK_MUX_RW(sd_emmc_c_sel, NAND_CLK_CTRL, 0x7, 9,
		 emmc_parent_table, 0, emmc_parent_data, 0);
AML_CLK_DIV_RW(sd_emmc_c_div, NAND_CLK_CTRL, 0, 7, NULL, 0,
		 &sd_emmc_c_sel.hw, CLK_SET_RATE_PARENT);
AML_CLK_GATE_RW(sd_emmc_c, NAND_CLK_CTRL, 7, 0,
		  &sd_emmc_c_div.hw, CLK_SET_RATE_PARENT);

/* temperature sensor */
static const struct clk_parent_data ts_parent = {
	.fw_name = "xtal",
};

__AML_CLK_DIV(ts_div, TS_CLK_CTRL, 0, 8, NULL, 0,
		&clk_regmap_divider_ops, NULL, &ts_parent, NULL, 0);
AML_CLK_GATE_RW(ts, TS_CLK_CTRL, 8, 0, &ts_div.hw, CLK_SET_RATE_PARENT);

static const struct clk_parent_data eth_parent = {
	.fw_name = "fclk_div2",
};

__AML_CLK_FIXED_FACTOR(eth_125m_div, 1, 8, &clk_fixed_factor_ops,
			 NULL, &eth_parent, NULL, 0);
AML_CLK_GATE_RW(eth_125m, ETH_CLK_CTRL, 7, 0, &eth_125m_div.hw,
		  CLK_SET_RATE_PARENT);
__AML_CLK_DIV(eth_rmii_div, ETH_CLK_CTRL, 0, 7, NULL, 0,
		&clk_regmap_divider_ops, NULL, &eth_parent, NULL, 0);
AML_CLK_GATE_RW(eth_rmii, ETH_CLK_CTRL, 8, 0, &eth_rmii_div.hw,
		  CLK_SET_RATE_PARENT);

static u32 mipi_dsi_meas_parent_table[] = { 0, 1, 2, 3, 5, 6, 7 };
static const struct clk_parent_data mipi_dsi_meas_parent_data[] = {
	{ .fw_name = "xtal" },
	{ .fw_name = "fclk_div4" },
	{ .fw_name = "fclk_div3" },
	{ .fw_name = "fclk_div5" },
	{ .fw_name = "gp0_pll" },
	{ .fw_name = "fclk_div2" },
	{ .fw_name = "fclk_div7" }
};

AML_CLK_MUX_RW(mipi_dsi_meas_sel, VDIN_MEAS_CLK_CTRL, 0x7, 21,
		 mipi_dsi_meas_parent_table, 0, mipi_dsi_meas_parent_data, 0);
AML_CLK_DIV_RW(mipi_dsi_meas_div, VDIN_MEAS_CLK_CTRL, 12, 7, NULL, 0,
		 &mipi_dsi_meas_sel.hw, CLK_SET_RATE_PARENT);
AML_CLK_GATE_RW(mipi_dsi_meas, VDIN_MEAS_CLK_CTRL, 20, 0,
		  &mipi_dsi_meas_div.hw, CLK_SET_RATE_PARENT);

static u32 dsi_phy_parent_table[] = { 1, 2, 3, 4, 5, 6, 7 };
static const struct clk_parent_data dsi_phy_parent_data[] = {
	{ .fw_name = "gp0_pll" },
	{ .fw_name = "hifi_pll" },
	{ .fw_name = "fclk_div3" },
	{ .fw_name = "fclk_div2" },
	{ .fw_name = "fclk_div2p5" },
	{ .fw_name = "fclk_div4" },
	{ .fw_name = "fclk_div7" }
};

AML_CLK_MUX_RW(dsi_phy_sel, MIPIDSI_PHY_CLK_CTRL, 0x7, 12,
		 dsi_phy_parent_table, 0, dsi_phy_parent_data, 0);
AML_CLK_DIV_RW(dsi_phy_div, MIPIDSI_PHY_CLK_CTRL, 0, 7, NULL, 0,
		 &dsi_phy_sel.hw, CLK_SET_RATE_PARENT);
AML_CLK_GATE_RW(dsi_phy, MIPIDSI_PHY_CLK_CTRL, 8, 0,
		  &dsi_phy_div.hw, CLK_SET_RATE_PARENT);

static u32 vout_parent_table[] = { 1, 2, 3, 4, 5, 6, 7 };
static const struct clk_parent_data vout_parent_data[] = {
	{ .fw_name = "fclk_div3" },
	{ .fw_name = "fclk_div4" },
	{ .fw_name = "fclk_div5" },
	{ .fw_name = "gp0_pll" },
	{ .fw_name = "hifi_pll" },
	{ .fw_name = "fclk_div2p5" },
	{ .fw_name = "fclk_div7" }
};

AML_CLK_MUX_RW(vout_mclk_sel, VOUTENC_CLK_CTRL, 0x7, 9,
		 vout_parent_table, 0, vout_parent_data, 0);
AML_CLK_DIV_RW(vout_mclk_div, VOUTENC_CLK_CTRL, 0, 7, NULL, 0,
		 &vout_mclk_sel.hw, CLK_SET_RATE_PARENT);
AML_CLK_GATE_RW(vout_mclk, VOUTENC_CLK_CTRL, 8, 0,
		  &vout_mclk_div.hw, CLK_SET_RATE_PARENT);

AML_CLK_MUX_RW(vout_enc_sel, VOUTENC_CLK_CTRL, 0x7, 25,
		 vout_parent_table, 0, vout_parent_data, 0);
AML_CLK_DIV_RW(vout_enc_div, VOUTENC_CLK_CTRL, 16, 7, NULL, 0,
		 &vout_enc_sel.hw, CLK_SET_RATE_PARENT);
AML_CLK_GATE_RW(vout_enc, VOUTENC_CLK_CTRL, 24, 0,
		  &vout_enc_div.hw, CLK_SET_RATE_PARENT);

static const struct clk_parent_data hcodec_pre_parent_data[] = {
	{ .fw_name = "fclk_div2p5" },
	{ .fw_name = "fclk_div3" },
	{ .fw_name = "fclk_div4" },
	{ .fw_name = "fclk_div5" },
	{ .fw_name = "fclk_div7" },
	{ .fw_name = "hifi_pll" },
	{ .fw_name = "gp0_pll" },
	{ .fw_name = "xtal" }
};

AML_CLK_MUX_RW(hcodec_0_sel, VDEC_CLK_CTRL, 0x7, 9,
		 NULL, 0, hcodec_pre_parent_data, 0);
AML_CLK_DIV_RW(hcodec_0_div, VDEC_CLK_CTRL, 0, 7, NULL, 0,
		 &hcodec_0_sel.hw, CLK_SET_RATE_PARENT);
AML_CLK_GATE_RW(hcodec_0, VDEC_CLK_CTRL, 8, 0,
		  &hcodec_0_div.hw, CLK_SET_RATE_PARENT);

AML_CLK_MUX_RW(hcodec_1_sel, VDEC3_CLK_CTRL, 0x7, 9,
		 NULL, 0, hcodec_pre_parent_data, 0);
AML_CLK_DIV_RW(hcodec_1_div, VDEC3_CLK_CTRL, 0, 7, NULL, 0,
		 &hcodec_1_sel.hw, CLK_SET_RATE_PARENT);
AML_CLK_GATE_RW(hcodec_1, VDEC3_CLK_CTRL, 8, 0,
		  &hcodec_1_div.hw, CLK_SET_RATE_PARENT);

static const struct clk_parent_data hcodec_parent_data[] = {
	{ .hw = &hcodec_0.hw },
	{ .hw = &hcodec_1.hw }
};

AML_CLK_MUX_RW(hcodec, VDEC3_CLK_CTRL, 0x1, 15,
		 NULL, 0, hcodec_parent_data, 0);

static const struct clk_parent_data vc9000e_parent_data[] = {
	{ .fw_name = "xtal" },
	{ .fw_name = "fclk_div4" },
	{ .fw_name = "fclk_div3" },
	{ .fw_name = "fclk_div5" },
	{ .fw_name = "fclk_div7" },
	{ .fw_name = "fclk_div2p5" },
	{ .fw_name = "hifi_pll" },
	{ .fw_name = "gp0_pll" }
};

AML_CLK_MUX_RW(vc9000e_aclk_sel, VC9000E_CLK_CTRL, 0x7, 9,
		 NULL, 0, vc9000e_parent_data, 0);
AML_CLK_DIV_RW(vc9000e_aclk_div, VC9000E_CLK_CTRL, 0, 7, NULL, 0,
		 &vc9000e_aclk_sel.hw, CLK_SET_RATE_PARENT);
AML_CLK_GATE_RW(vc9000e_aclk, VC9000E_CLK_CTRL, 8, 0,
		  &vc9000e_aclk_div.hw, CLK_SET_RATE_PARENT);

AML_CLK_MUX_RW(vc9000e_core_sel, VC9000E_CLK_CTRL, 0x7, 25,
		 NULL, 0, vc9000e_parent_data, 0);
AML_CLK_DIV_RW(vc9000e_core_div, VC9000E_CLK_CTRL, 16, 7, NULL, 0,
		 &vc9000e_core_sel.hw, CLK_SET_RATE_PARENT);
AML_CLK_GATE_RW(vc9000e_core, VC9000E_CLK_CTRL, 24, 0,
		  &vc9000e_core_div.hw, CLK_SET_RATE_PARENT);

static u32 csi_phy_parent_table[] = { 0, 1, 2, 3, 4, 5, 7 };
static const struct clk_parent_data csi_phy_parent_data[] = {
	{ .fw_name = "fclk_div2p5" },
	{ .fw_name = "fclk_div3" },
	{ .fw_name = "fclk_div4" },
	{ .fw_name = "fclk_div5" },
	{ .fw_name = "gp0_pll" },
	{ .fw_name = "hifi_pll" },
	{ .fw_name = "xtal" }
};

AML_CLK_MUX_RW(csi_phy0_sel, ISP0_CLK_CTRL, 0x7, 25,
		 csi_phy_parent_table, 0, csi_phy_parent_data, 0);
AML_CLK_DIV_RW(csi_phy0_div, ISP0_CLK_CTRL, 16, 7, NULL, 0,
		 &csi_phy0_sel.hw, CLK_SET_RATE_PARENT);
AML_CLK_GATE_RW(csi_phy0, ISP0_CLK_CTRL, 24, 0,
		  &csi_phy0_div.hw, CLK_SET_RATE_PARENT);

static u32 dewarpa_parent_table[] = { 0, 1, 2, 3, 4, 5, 7 };
static const struct clk_parent_data dewarpa_parent_data[] = {
	{ .fw_name = "fclk_div2p5" },
	{ .fw_name = "fclk_div3" },
	{ .fw_name = "fclk_div4" },
	{ .fw_name = "fclk_div5" },
	{ .fw_name = "gp0_pll" },
	{ .fw_name = "hifi_pll" },
	{ .fw_name = "fclk_div7" }
};

AML_CLK_MUX_RW(dewarpa_sel, DEWARPA_CLK_CTRL, 0x7, 9,
		 dewarpa_parent_table, 0, dewarpa_parent_data, 0);
AML_CLK_DIV_RW(dewarpa_div, DEWARPA_CLK_CTRL, 0, 7, NULL, 0,
		 &dewarpa_sel.hw, CLK_SET_RATE_PARENT);
AML_CLK_GATE_RW(dewarpa, DEWARPA_CLK_CTRL, 8, 0,
		  &dewarpa_div.hw, CLK_SET_RATE_PARENT);

static u32 isp_parent_table[] = { 0, 1, 2, 3, 4, 5, 7 };
static const struct clk_parent_data isp_parent_data[] = {
	{ .fw_name = "fclk_div2p5" },
	{ .fw_name = "fclk_div3" },
	{ .fw_name = "fclk_div4" },
	{ .fw_name = "fclk_div5" },
	{ .fw_name = "gp0_pll" },
	{ .fw_name = "hifi_pll" },
	{ .fw_name = "xtal" }
};

AML_CLK_MUX_RW(isp0_sel, ISP0_CLK_CTRL, 0x7, 9,
		 isp_parent_table, 0, isp_parent_data, 0);
AML_CLK_DIV_RW(isp0_div, ISP0_CLK_CTRL, 0, 7, NULL, 0,
		 &isp0_sel.hw, CLK_SET_RATE_PARENT);
AML_CLK_GATE_RW(isp0, ISP0_CLK_CTRL, 8, 0,
		  &isp0_div.hw, CLK_SET_RATE_PARENT);

static u32 nna_core_parent_table[] = { 0, 1, 2, 3, 4, 5, 7 };
static const struct clk_parent_data nna_core_parent_data[] = {
	{ .fw_name = "xtal" },
	{ .fw_name = "fclk_div2p5" },
	{ .fw_name = "fclk_div4" },
	{ .fw_name = "fclk_div3" },
	{ .fw_name = "fclk_div5" },
	{ .fw_name = "fclk_div2" },
	{ .fw_name = "hifi_pll" }
};

AML_CLK_MUX_RW(nna_core_sel, NNA_CLK_CTRL, 0x7, 9,
		 nna_core_parent_table, 0, nna_core_parent_data, 0);
AML_CLK_DIV_RW(nna_core_div, NNA_CLK_CTRL, 0, 7, NULL, 0,
		 &nna_core_sel.hw, CLK_SET_RATE_PARENT);
AML_CLK_GATE_RW(nna_core, NNA_CLK_CTRL, 8, 0,
		  &nna_core_div.hw, CLK_SET_RATE_PARENT);

static const struct clk_parent_data ge2d_parent_data[] = {
	{ .fw_name = "xtal" },
	{ .fw_name = "fclk_div2p5" },
	{ .fw_name = "fclk_div3" },
	{ .fw_name = "fclk_div4" },
	{ .fw_name = "hifi_pll" },
	{ .fw_name = "fclk_div5" },
	{ .fw_name = "gp0_pll" },
	{ .hw = &rtc_clk.hw }
};

AML_CLK_MUX_RW(ge2d_sel, GE2D_CLK_CTRL, 0x7, 9,
		 NULL, 0, ge2d_parent_data, 0);
AML_CLK_DIV_RW(ge2d_div, GE2D_CLK_CTRL, 0, 7, NULL, 0,
		 &ge2d_sel.hw, CLK_SET_RATE_PARENT);
AML_CLK_GATE_RW(ge2d, GE2D_CLK_CTRL, 8, 0,
		  &ge2d_div.hw, CLK_SET_RATE_PARENT);

static u32 c3_vapb_parent_table[] = { 0, 1, 2, 3, 4, 5, 7 };
static const struct clk_parent_data vapb_parent_data[] = {
	{ .fw_name = "fclk_div2p5" },
	{ .fw_name = "fclk_div3" },
	{ .fw_name = "fclk_div4" },
	{ .fw_name = "fclk_div5" },
	{ .fw_name = "gp0_pll" },
	{ .fw_name = "hifi_pll" },
	{ .fw_name = "xtal" }
};

AML_CLK_MUX_RW(vapb_sel, VAPB_CLK_CTRL, 0x7, 9,
		 c3_vapb_parent_table, 0, vapb_parent_data, 0);
AML_CLK_DIV_RW(vapb_div, VAPB_CLK_CTRL, 0, 7, NULL, 0,
		 &vapb_sel.hw, CLK_SET_RATE_PARENT);
AML_CLK_GATE_RW(vapb, VAPB_CLK_CTRL, 8, 0,
		  &vapb_div.hw, CLK_SET_RATE_PARENT);

static struct clk_hw *c3_periphs_hw_clks[] = {
	[CLKID_PLL_IN]			= &pll_in.hw,
	[CLKID_MCLK_PLL_IN]		= &mclk_pll_in.hw,
	[CLKID_RTC_XTAL_CLKIN]		= &rtc_xtal_clkin.hw,
	[CLKID_RTC_32K_DIV]		= &rtc_32k_div.hw,
	[CLKID_RTC_32K_MUX]		= &rtc_32k_mux.hw,
	[CLKID_RTC_32K]			= &rtc_32k.hw,
	[CLKID_RTC_CLK]			= &rtc_clk.hw,
	[CLKID_SYS_A_SEL]		= &sys_a_sel.hw,
	[CLKID_SYS_A_DIV]		= &sys_a_div.hw,
	[CLKID_SYS_A]			= &sys_a.hw,
	[CLKID_SYS_B_SEL]		= &sys_b_sel.hw,
	[CLKID_SYS_B_DIV]		= &sys_b_div.hw,
	[CLKID_SYS_B]			= &sys_b.hw,
	[CLKID_SYS_CLK]			= &sys_clk.hw,
	[CLKID_AXI_A_SEL]		= &axi_a_sel.hw,
	[CLKID_AXI_A_DIV]		= &axi_a_div.hw,
	[CLKID_AXI_A]			= &axi_a.hw,
	[CLKID_AXI_B_SEL]		= &axi_b_sel.hw,
	[CLKID_AXI_B_DIV]		= &axi_b_div.hw,
	[CLKID_AXI_B]			= &axi_b.hw,
	[CLKID_AXI_CLK]			= &axi_clk.hw,
	[CLKID_SYS_RESET_CTRL]		= &sys_reset_ctrl.hw,
	[CLKID_SYS_PAD_CTRL]		= &sys_pwr_ctrl.hw,
	[CLKID_SYS_CTRL]		= &sys_ctrl.hw,
	[CLKID_SYS_TS_PLL]		= &sys_ts_pll.hw,
	[CLKID_SYS_DEV_ARB]		= &sys_dev_arb.hw,
	[CLKID_SYS_MMC_PCLK]		= &sys_mmc_pclk.hw,
	[CLKID_SYS_CAPU]		= &sys_capu.hw,
	[CLKID_SYS_CPU_CTRL]		= &sys_cpu_ctrl.hw,
	[CLKID_SYS_JTAG_CTRL]		= &sys_jtag_ctrl.hw,
	[CLKID_SYS_IR_CTRL]		= &sys_ir_ctrl.hw,
	[CLKID_SYS_IRQ_CTRL]		= &sys_irq_ctrl.hw,
	[CLKID_SYS_MSR_CLK]		= &sys_msr_clk.hw,
	[CLKID_SYS_ROM]			= &sys_rom.hw,
	[CLKID_SYS_UART_F]		= &sys_uart_f.hw,
	[CLKID_SYS_CPU_ARB]		= &sys_cpu_apb.hw,
	[CLKID_SYS_RSA]			= &sys_rsa.hw,
	[CLKID_SYS_SAR_ADC]		= &sys_sar_adc.hw,
	[CLKID_SYS_STARTUP]		= &sys_startup.hw,
	[CLKID_SYS_SECURE]		= &sys_secure.hw,
	[CLKID_SYS_SPIFC]		= &sys_spifc.hw,
	[CLKID_SYS_NNA]			= &sys_nna.hw,
	[CLKID_SYS_ETH_MAC]		= &sys_eth_mac.hw,
	[CLKID_SYS_GIC]			= &sys_gic.hw,
	[CLKID_SYS_RAMA]		= &sys_rama.hw,
	[CLKID_SYS_BIG_NIC]		= &sys_big_nic.hw,
	[CLKID_SYS_RAMB]		= &sys_ramb.hw,
	[CLKID_SYS_AUDIO_PCLK]		= &sys_audio_pclk.hw,
	[CLKID_SYS_PWM_KL]		= &sys_pwm_kl.hw,
	[CLKID_SYS_PWM_IJ]		= &sys_pwm_ij.hw,
	[CLKID_SYS_USB]			= &sys_usb.hw,
	[CLKID_SYS_SD_EMMC_A]		= &sys_sd_emmc_a.hw,
	[CLKID_SYS_SD_EMMC_C]		= &sys_sd_emmc_c.hw,
	[CLKID_SYS_PWM_AB]		= &sys_pwm_ab.hw,
	[CLKID_SYS_PWM_CD]		= &sys_pwm_cd.hw,
	[CLKID_SYS_PWM_EF]		= &sys_pwm_ef.hw,
	[CLKID_SYS_PWM_GH]		= &sys_pwm_gh.hw,
	[CLKID_SYS_SPICC_1]		= &sys_spicc_1.hw,
	[CLKID_SYS_SPICC_0]		= &sys_spicc_0.hw,
	[CLKID_SYS_UART_A]		= &sys_uart_a.hw,
	[CLKID_SYS_UART_B]		= &sys_uart_b.hw,
	[CLKID_SYS_UART_C]		= &sys_uart_c.hw,
	[CLKID_SYS_UART_D]		= &sys_uart_d.hw,
	[CLKID_SYS_UART_E]		= &sys_uart_e.hw,
	[CLKID_SYS_I2C_M_A]		= &sys_i2c_m_a.hw,
	[CLKID_SYS_I2C_M_B]		= &sys_i2c_m_b.hw,
	[CLKID_SYS_I2C_M_C]		= &sys_i2c_m_c.hw,
	[CLKID_SYS_I2C_M_D]		= &sys_i2c_m_d.hw,
	[CLKID_SYS_I2S_S_A]		= &sys_i2c_s_a.hw,
	[CLKID_SYS_RTC]			= &sys_rtc.hw,
	[CLKID_SYS_GE2D]		= &sys_ge2d.hw,
	[CLKID_SYS_ISP]			= &sys_isp.hw,
	[CLKID_SYS_GPV_ISP_NIC]		= &sys_gpv_isp_nic.hw,
	[CLKID_SYS_GPV_CVE_NIC]		= &sys_gpv_cve_nic.hw,
	[CLKID_SYS_MIPI_DSI_HOST]	= &sys_mipi_dsi_host.hw,
	[CLKID_SYS_MIPI_DSI_PHY]	= &sys_mipi_dsi_phy.hw,
	[CLKID_SYS_ETH_PHY]		= &sys_eth_phy.hw,
	[CLKID_SYS_ACODEC]		= &sys_acodec.hw,
	[CLKID_SYS_DWAP]		= &sys_dwap.hw,
	[CLKID_SYS_DOS]			= &sys_dos.hw,
	[CLKID_SYS_CVE]			= &sys_cve.hw,
	[CLKID_SYS_VOUT]		= &sys_vout.hw,
	[CLKID_SYS_VC9000E]		= &sys_vc9000e.hw,
	[CLKID_SYS_PWM_MN]		= &sys_pwm_mn.hw,
	[CLKID_SYS_SD_EMMC_B]		= &sys_sd_emmc_b.hw,
	[CLKID_AXI_SYS_NIC]		= &axi_sys_nic.hw,
	[CLKID_AXI_ISP_NIC]		= &axi_isp_nic.hw,
	[CLKID_AXI_CVE_NIC]		= &axi_cve_nic.hw,
	[CLKID_AXI_RAMB]		= &axi_ramb.hw,
	[CLKID_AXI_RAMA]		= &axi_rama.hw,
	[CLKID_AXI_CPU_DMC]		= &axi_cpu_dmc.hw,
	[CLKID_AXI_NIC]			= &axi_nic.hw,
	[CLKID_AXI_DMA]			= &axi_dma.hw,
	[CLKID_AXI_MUX_NIC]		= &axi_mux_nic.hw,
	[CLKID_AXI_CAPU]		= &axi_capu.hw,
	[CLKID_AXI_CVE]			= &axi_cve.hw,
	[CLKID_AXI_DEV1_DMC]		= &axi_dev1_dmc.hw,
	[CLKID_AXI_DEV0_DMC]		= &axi_dev0_dmc.hw,
	[CLKID_AXI_DSP_DMC]		= &axi_dsp_dmc.hw,
	[CLKID_12_24M_IN]		= &clk_12_24m_in.hw,
	[CLKID_12M_24M]			= &clk_12_24m.hw,
	[CLKID_FCLK_25M_DIV]		= &fclk_25m_div.hw,
	[CLKID_FCLK_25M]		= &fclk_25m.hw,
	[CLKID_GEN_SEL]			= &gen_sel.hw,
	[CLKID_GEN_DIV]			= &gen_div.hw,
	[CLKID_GEN]			= &gen.hw,
	[CLKID_SARADC_SEL]		= &saradc_sel.hw,
	[CLKID_SARADC_DIV]		= &saradc_div.hw,
	[CLKID_SARADC]			= &saradc.hw,
	[CLKID_PWM_A_SEL]		= &pwm_a_sel.hw,
	[CLKID_PWM_A_DIV]		= &pwm_a_div.hw,
	[CLKID_PWM_A]			= &pwm_a.hw,
	[CLKID_PWM_B_SEL]		= &pwm_b_sel.hw,
	[CLKID_PWM_B_DIV]		= &pwm_b_div.hw,
	[CLKID_PWM_B]			= &pwm_b.hw,
	[CLKID_PWM_C_SEL]		= &pwm_c_sel.hw,
	[CLKID_PWM_C_DIV]		= &pwm_c_div.hw,
	[CLKID_PWM_C]			= &pwm_c.hw,
	[CLKID_PWM_D_SEL]		= &pwm_d_sel.hw,
	[CLKID_PWM_D_DIV]		= &pwm_d_div.hw,
	[CLKID_PWM_D]			= &pwm_d.hw,
	[CLKID_PWM_E_SEL]		= &pwm_e_sel.hw,
	[CLKID_PWM_E_DIV]		= &pwm_e_div.hw,
	[CLKID_PWM_E]			= &pwm_e.hw,
	[CLKID_PWM_F_SEL]		= &pwm_f_sel.hw,
	[CLKID_PWM_F_DIV]		= &pwm_f_div.hw,
	[CLKID_PWM_F]			= &pwm_f.hw,
	[CLKID_PWM_G_SEL]		= &pwm_g_sel.hw,
	[CLKID_PWM_G_DIV]		= &pwm_g_div.hw,
	[CLKID_PWM_G]			= &pwm_g.hw,
	[CLKID_PWM_H_SEL]		= &pwm_h_sel.hw,
	[CLKID_PWM_H_DIV]		= &pwm_h_div.hw,
	[CLKID_PWM_H]			= &pwm_h.hw,
	[CLKID_PWM_I_SEL]		= &pwm_i_sel.hw,
	[CLKID_PWM_I_DIV]		= &pwm_i_div.hw,
	[CLKID_PWM_I]			= &pwm_i.hw,
	[CLKID_PWM_J_SEL]		= &pwm_j_sel.hw,
	[CLKID_PWM_J_DIV]		= &pwm_j_div.hw,
	[CLKID_PWM_J]			= &pwm_j.hw,
	[CLKID_PWM_K_SEL]		= &pwm_k_sel.hw,
	[CLKID_PWM_K_DIV]		= &pwm_k_div.hw,
	[CLKID_PWM_K]			= &pwm_k.hw,
	[CLKID_PWM_L_SEL]		= &pwm_l_sel.hw,
	[CLKID_PWM_L_DIV]		= &pwm_l_div.hw,
	[CLKID_PWM_L]			= &pwm_l.hw,
	[CLKID_PWM_M_SEL]		= &pwm_m_sel.hw,
	[CLKID_PWM_M_DIV]		= &pwm_m_div.hw,
	[CLKID_PWM_M]			= &pwm_m.hw,
	[CLKID_PWM_N_SEL]		= &pwm_n_sel.hw,
	[CLKID_PWM_N_DIV]		= &pwm_n_div.hw,
	[CLKID_PWM_N]			= &pwm_n.hw,
	[CLKID_SPICC_A_SEL]		= &spicc_a_sel.hw,
	[CLKID_SPICC_A_DIV]		= &spicc_a_div.hw,
	[CLKID_SPICC_A]			= &spicc_a.hw,
	[CLKID_SPICC_B_SEL]		= &spicc_b_sel.hw,
	[CLKID_SPICC_B_DIV]		= &spicc_b_div.hw,
	[CLKID_SPICC_B]			= &spicc_b.hw,
	[CLKID_SPIFC_SEL]		= &spifc_sel.hw,
	[CLKID_SPIFC_DIV]		= &spifc_div.hw,
	[CLKID_SPIFC]			= &spifc.hw,
	[CLKID_SD_EMMC_A_SEL]		= &sd_emmc_a_sel.hw,
	[CLKID_SD_EMMC_A_DIV]		= &sd_emmc_a_div.hw,
	[CLKID_SD_EMMC_A]		= &sd_emmc_a.hw,
	[CLKID_SD_EMMC_B_SEL]		= &sd_emmc_b_sel.hw,
	[CLKID_SD_EMMC_B_DIV]		= &sd_emmc_b_div.hw,
	[CLKID_SD_EMMC_B]		= &sd_emmc_b.hw,
	[CLKID_SD_EMMC_C_SEL]		= &sd_emmc_c_sel.hw,
	[CLKID_SD_EMMC_C_DIV]		= &sd_emmc_c_div.hw,
	[CLKID_SD_EMMC_C]		= &sd_emmc_c.hw,
	[CLKID_TS_DIV]			= &ts_div.hw,
	[CLKID_TS]			= &ts.hw,
	[CLKID_ETH_125M_DIV]		= &eth_125m_div.hw,
	[CLKID_ETH_125M]		= &eth_125m.hw,
	[CLKID_ETH_RMII_DIV]		= &eth_rmii_div.hw,
	[CLKID_ETH_RMII]		= &eth_rmii.hw,
	[CLKID_MIPI_DSI_MEAS_SEL]	= &mipi_dsi_meas_sel.hw,
	[CLKID_MIPI_DSI_MEAS_DIV]	= &mipi_dsi_meas_div.hw,
	[CLKID_MIPI_DSI_MEAS]		= &mipi_dsi_meas.hw,
	[CLKID_DSI_PHY_SEL]		= &dsi_phy_sel.hw,
	[CLKID_DSI_PHY_DIV]		= &dsi_phy_div.hw,
	[CLKID_DSI_PHY]			= &dsi_phy.hw,
	[CLKID_VOUT_MCLK_SEL]		= &vout_mclk_sel.hw,
	[CLKID_VOUT_MCLK_DIV]		= &vout_mclk_div.hw,
	[CLKID_VOUT_MCLK]		= &vout_mclk.hw,
	[CLKID_VOUT_ENC_SEL]		= &vout_enc_sel.hw,
	[CLKID_VOUT_ENC_DIV]		= &vout_enc_div.hw,
	[CLKID_VOUT_ENC]		= &vout_enc.hw,
	[CLKID_HCODEC_0_SEL]		= &hcodec_0_sel.hw,
	[CLKID_HCODEC_0_DIV]		= &hcodec_0_div.hw,
	[CLKID_HCODEC_0]		= &hcodec_0.hw,
	[CLKID_HCODEC_1_SEL]		= &hcodec_1_sel.hw,
	[CLKID_HCODEC_1_DIV]		= &hcodec_1_div.hw,
	[CLKID_HCODEC_1]		= &hcodec_1.hw,
	[CLKID_HCODEC]			= &hcodec.hw,
	[CLKID_VC9000E_ACLK_SEL]	= &vc9000e_aclk_sel.hw,
	[CLKID_VC9000E_ACLK_DIV]	= &vc9000e_aclk_div.hw,
	[CLKID_VC9000E_ACLK]		= &vc9000e_aclk.hw,
	[CLKID_VC9000E_CORE_SEL]	= &vc9000e_core_sel.hw,
	[CLKID_VC9000E_CORE_DIV]	= &vc9000e_core_div.hw,
	[CLKID_VC9000E_CORE]		= &vc9000e_core.hw,
	[CLKID_CSI_PHY0_SEL]		= &csi_phy0_sel.hw,
	[CLKID_CSI_PHY0_DIV]		= &csi_phy0_div.hw,
	[CLKID_CSI_PHY0]		= &csi_phy0.hw,
	[CLKID_DEWARPA_SEL]		= &dewarpa_sel.hw,
	[CLKID_DEWARPA_DIV]		= &dewarpa_div.hw,
	[CLKID_DEWARPA]			= &dewarpa.hw,
	[CLKID_ISP0_SEL]		= &isp0_sel.hw,
	[CLKID_ISP0_DIV]		= &isp0_div.hw,
	[CLKID_ISP0]			= &isp0.hw,
	[CLKID_NNA_CORE_SEL]		= &nna_core_sel.hw,
	[CLKID_NNA_CORE_DIV]		= &nna_core_div.hw,
	[CLKID_NNA_CORE]		= &nna_core.hw,
	[CLKID_GE2D_SEL]		= &ge2d_sel.hw,
	[CLKID_GE2D_DIV]		= &ge2d_div.hw,
	[CLKID_GE2D]			= &ge2d.hw,
	[CLKID_VAPB_SEL]		= &vapb_sel.hw,
	[CLKID_VAPB_DIV]		= &vapb_div.hw,
	[CLKID_VAPB]			= &vapb.hw,
};

/* Convenience table to populate regmap in .probe */
static struct clk_regmap *const c3_periphs_clk_regmaps[] = {
	&pll_in,
	&mclk_pll_in,
	&rtc_xtal_clkin,
	&rtc_32k_div,
	&rtc_32k_mux,
	&rtc_32k,
	&rtc_clk,
	&sys_a_sel,
	&sys_a_div,
	&sys_a,
	&sys_b_sel,
	&sys_b_div,
	&sys_b,
	&sys_clk,
	&axi_a_sel,
	&axi_a_div,
	&axi_a,
	&axi_b_sel,
	&axi_b_div,
	&axi_b,
	&axi_clk,
	&sys_reset_ctrl,
	&sys_pwr_ctrl,
	&sys_pad_ctrl,
	&sys_ctrl,
	&sys_ts_pll,
	&sys_dev_arb,
	&sys_mmc_pclk,
	&sys_capu,
	&sys_cpu_ctrl,
	&sys_jtag_ctrl,
	&sys_ir_ctrl,
	&sys_irq_ctrl,
	&sys_msr_clk,
	&sys_rom,
	&sys_uart_f,
	&sys_cpu_apb,
	&sys_rsa,
	&sys_sar_adc,
	&sys_startup,
	&sys_secure,
	&sys_spifc,
	&sys_nna,
	&sys_eth_mac,
	&sys_gic,
	&sys_rama,
	&sys_big_nic,
	&sys_ramb,
	&sys_audio_pclk,
	&sys_pwm_kl,
	&sys_pwm_ij,
	&sys_usb,
	&sys_sd_emmc_a,
	&sys_sd_emmc_c,
	&sys_pwm_ab,
	&sys_pwm_cd,
	&sys_pwm_ef,
	&sys_pwm_gh,
	&sys_spicc_1,
	&sys_spicc_0,
	&sys_uart_a,
	&sys_uart_b,
	&sys_uart_c,
	&sys_uart_d,
	&sys_uart_e,
	&sys_i2c_m_a,
	&sys_i2c_m_b,
	&sys_i2c_m_c,
	&sys_i2c_m_d,
	&sys_i2c_s_a,
	&sys_rtc,
	&sys_ge2d,
	&sys_isp,
	&sys_gpv_isp_nic,
	&sys_gpv_cve_nic,
	&sys_mipi_dsi_host,
	&sys_mipi_dsi_phy,
	&sys_eth_phy,
	&sys_acodec,
	&sys_dwap,
	&sys_dos,
	&sys_cve,
	&sys_vout,
	&sys_vc9000e,
	&sys_pwm_mn,
	&sys_sd_emmc_b,
	&axi_sys_nic,
	&axi_isp_nic,
	&axi_cve_nic,
	&axi_ramb,
	&axi_rama,
	&axi_cpu_dmc,
	&axi_nic,
	&axi_dma,
	&axi_mux_nic,
	&axi_capu,
	&axi_cve,
	&axi_dev1_dmc,
	&axi_dev0_dmc,
	&axi_dsp_dmc,
	&clk_12_24m_in,
	&clk_12_24m,
	&fclk_25m_div,
	&fclk_25m,
	&gen_sel,
	&gen_div,
	&gen,
	&saradc_sel,
	&saradc_div,
	&saradc,
	&pwm_a_sel,
	&pwm_a_div,
	&pwm_a,
	&pwm_b_sel,
	&pwm_b_div,
	&pwm_b,
	&pwm_c_sel,
	&pwm_c_div,
	&pwm_c,
	&pwm_d_sel,
	&pwm_d_div,
	&pwm_d,
	&pwm_e_sel,
	&pwm_e_div,
	&pwm_e,
	&pwm_f_sel,
	&pwm_f_div,
	&pwm_f,
	&pwm_g_sel,
	&pwm_g_div,
	&pwm_g,
	&pwm_h_sel,
	&pwm_h_div,
	&pwm_h,
	&pwm_i_sel,
	&pwm_i_div,
	&pwm_i,
	&pwm_j_sel,
	&pwm_j_div,
	&pwm_j,
	&pwm_k_sel,
	&pwm_k_div,
	&pwm_k,
	&pwm_l_sel,
	&pwm_l_div,
	&pwm_l,
	&pwm_m_sel,
	&pwm_m_div,
	&pwm_m,
	&pwm_n_sel,
	&pwm_n_div,
	&pwm_n,
	&spicc_a_sel,
	&spicc_a_div,
	&spicc_a,
	&spicc_b_sel,
	&spicc_b_div,
	&spicc_b,
	&spifc_sel,
	&spifc_div,
	&spifc,
	&sd_emmc_a_sel,
	&sd_emmc_a_div,
	&sd_emmc_a,
	&sd_emmc_b_sel,
	&sd_emmc_b_div,
	&sd_emmc_b,
	&sd_emmc_c_sel,
	&sd_emmc_c_div,
	&sd_emmc_c,
	&ts_div,
	&ts,
	&eth_125m,
	&eth_rmii_div,
	&eth_rmii,
	&mipi_dsi_meas_sel,
	&mipi_dsi_meas_div,
	&mipi_dsi_meas,
	&dsi_phy_sel,
	&dsi_phy_div,
	&dsi_phy,
	&vout_mclk_sel,
	&vout_mclk_div,
	&vout_mclk,
	&vout_enc_sel,
	&vout_enc_div,
	&vout_enc,
	&hcodec_0_sel,
	&hcodec_0_div,
	&hcodec_0,
	&hcodec_1_sel,
	&hcodec_1_div,
	&hcodec_1,
	&hcodec,
	&vc9000e_aclk_sel,
	&vc9000e_aclk_div,
	&vc9000e_aclk,
	&vc9000e_core_sel,
	&vc9000e_core_div,
	&vc9000e_core,
	&csi_phy0_sel,
	&csi_phy0_div,
	&csi_phy0,
	&dewarpa_sel,
	&dewarpa_div,
	&dewarpa,
	&isp0_sel,
	&isp0_div,
	&isp0,
	&nna_core_sel,
	&nna_core_div,
	&nna_core,
	&ge2d_sel,
	&ge2d_div,
	&ge2d,
	&vapb_sel,
	&vapb_div,
	&vapb,
};

static struct regmap_config clkc_regmap_config = {
	.reg_bits       = 32,
	.val_bits       = 32,
	.reg_stride     = 4,
};

static struct meson_clk_hw_data c3_periphs_clks = {
	.hws = c3_periphs_hw_clks,
	.num = ARRAY_SIZE(c3_periphs_hw_clks),
};

static int aml_c3_peripherals_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct regmap *regmap;
	void __iomem *base;
	int clkid, ret, i;

	base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(base))
		return PTR_ERR(base);

	regmap = devm_regmap_init_mmio(dev, base, &clkc_regmap_config);
	if (IS_ERR(regmap))
		return PTR_ERR(regmap);

	/* Populate regmap for the regmap backed clocks */
	for (i = 0; i < ARRAY_SIZE(c3_periphs_clk_regmaps); i++)
		c3_periphs_clk_regmaps[i]->map = regmap;

	for (clkid = 0; clkid < c3_periphs_clks.num; clkid++) {
		/* array might be sparse */
		if (!c3_periphs_clks.hws[clkid])
			continue;

		ret = devm_clk_hw_register(dev, c3_periphs_clks.hws[clkid]);
		if (ret) {
			dev_err(dev, "Clock registration failed\n");
			return ret;
		}
	}

	return devm_of_clk_add_hw_provider(dev, meson_clk_hw_get,
					   &c3_periphs_clks);
}

static const struct of_device_id c3_peripherals_clkc_match_table[] = {
	{
		.compatible = "amlogic,c3-peripherals-clkc",
	},
	{}
};

MODULE_DEVICE_TABLE(of, c3_peripherals_clkc_match_table);

static struct platform_driver c3_peripherals_driver = {
	.probe		= aml_c3_peripherals_probe,
	.driver		= {
		.name	= "c3-peripherals-clkc",
		.of_match_table = c3_peripherals_clkc_match_table,
	},
};

module_platform_driver(c3_peripherals_driver);
MODULE_AUTHOR("Chuan Liu <chuan.liu@amlogic.com>");
MODULE_LICENSE("GPL");
