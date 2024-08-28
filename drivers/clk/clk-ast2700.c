// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2024 ASPEED Technology Inc.
 * Author: Ryan Chen <ryan_chen@aspeedtech.com>
 */

#include <linux/bits.h>
#include <linux/clk-provider.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/reset-controller.h>
#include <linux/slab.h>

#include <dt-bindings/clock/aspeed,ast2700-clk.h>
#include <dt-bindings/reset/aspeed,ast2700-reset.h>

#define SCU_CLK_24MHZ 24000000
#define SCU_CLK_25MHZ 25000000
#define SCU_CLK_192MHZ 192000000
/* SOC0 USB2 PHY CLK*/
#define SCU_CLK_12MHZ 12000000
/* SOC0 */
#define SCU0_HWSTRAP1 0x010
#define SCU0_RESET_CTRL1 0x200
#define SCU0_RESET_CTRL2 0x220
#define SCU0_CLK_STOP 0x240
#define SCU0_CLK_SEL1 0x280
#define SCU0_CLK_SEL2 0x284
#define GET_USB_REFCLK_DIV(x) ((GENMASK(23, 20) & (x)) >> 20)
#define UART_DIV13_EN BIT(30)
#define SCU0_HPLL_PARAM 0x300
#define SCU0_DPLL_PARAM 0x308
#define SCU0_MPLL_PARAM 0x310
#define SCU0_D0CLK_PARAM 0x320
#define SCU0_D1CLK_PARAM 0x330
#define SCU0_CRT0CLK_PARAM 0x340
#define SCU0_CRT1CLK_PARAM 0x350
#define SCU0_MPHYCLK_PARAM 0x360

/* SOC1 */
#define SCU1_RESET_CTRL1 0x200
#define SCU1_RESET_CTRL2 0x220
#define SCU1_CLK_STOP 0x240
#define SCU1_CLK_STOP2 0x260
#define SCU1_CLK_SEL1 0x280
#define SCU1_CLK_SEL2 0x284
#define UXCLK_MASK GENMASK(1, 0)
#define HUXCLK_MASK GENMASK(4, 3)
#define SCU1_HPLL_PARAM 0x300
#define SCU1_APLL_PARAM 0x310
#define SCU1_DPLL_PARAM 0x320
#define SCU1_UXCLK_CTRL 0x330
#define SCU1_HUXCLK_CTRL 0x334
#define SCU1_MAC12_CLK_DLY 0x390
#define SCU1_MAC12_CLK_DLY_100M 0x394
#define SCU1_MAC12_CLK_DLY_10M 0x398
#define SCU1_PCIE3_CTRL 0x908

#define SCU0_NUM_CLKS	62
#define SCU1_NUM_CLKS	84

struct ast2700_reset_signal {
	int dedicated_clr; /* dedicated reset clr offset */
	u32 offset, bit;
};

static const struct ast2700_reset_signal ast2700_reset0_signals[] = {
	[SCU0_RESET_SDRAM] = { 1, SCU0_RESET_CTRL1, BIT(0) },
	[SCU0_RESET_DDRPHY] = { 1, SCU0_RESET_CTRL1, BIT(1) },
	[SCU0_RESET_RSA]     = { 1, SCU0_RESET_CTRL1, BIT(2) },
	[SCU0_RESET_SHA3]	= { 1, SCU0_RESET_CTRL1, BIT(3) },
	[SCU0_RESET_HACE]	= { 1, SCU0_RESET_CTRL1, BIT(4) },
	[SCU0_RESET_SOC]	= { 1, SCU0_RESET_CTRL1, BIT(5) },
	[SCU0_RESET_VIDEO]	= { 1, SCU0_RESET_CTRL1, BIT(6) },
	[SCU0_RESET_2D]	= { 1, SCU0_RESET_CTRL1, BIT(7) },
	[SCU0_RESET_PCIS]	= { 1, SCU0_RESET_CTRL1, BIT(8) },
	[SCU0_RESET_RVAS0]		= { 1, SCU0_RESET_CTRL1, BIT(9) },
	[SCU0_RESET_RVAS1]		= { 1, SCU0_RESET_CTRL1, BIT(10) },
	[SCU0_RESET_SM3]		= { 1, SCU0_RESET_CTRL1, BIT(11) },
	[SCU0_RESET_SM4]		= { 1, SCU0_RESET_CTRL1, BIT(12) },
	[SCU0_RESET_CRT0]	= { 1, SCU0_RESET_CTRL1, BIT(13) },
	[SCU0_RESET_ECC]	= { 1, SCU0_RESET_CTRL1, BIT(14) },
	[SCU0_RESET_DP_PCI]	= { 1, SCU0_RESET_CTRL1, BIT(15) },
	[SCU0_RESET_UFS]	= { 1, SCU0_RESET_CTRL1, BIT(16) },
	[SCU0_RESET_EMMC]	= { 1, SCU0_RESET_CTRL1, BIT(17) },
	[SCU0_RESET_PCIE1RST]	= { 1, SCU0_RESET_CTRL1, BIT(18) },
	[SCU0_RESET_PCIE1RSTOE]	= { 1, SCU0_RESET_CTRL1, BIT(19) },
	[SCU0_RESET_PCIE0RST]		= { 1, SCU0_RESET_CTRL1, BIT(20) },
	[SCU0_RESET_PCIE0RSTOE]	= { 1, SCU0_RESET_CTRL1, BIT(21) },
	[SCU0_RESET_JTAG]	= { 1, SCU0_RESET_CTRL1, BIT(22) },
	[SCU0_RESET_MCTP0] = { 1, SCU0_RESET_CTRL1, BIT(23) },
	[SCU0_RESET_MCTP1]		= { 1, SCU0_RESET_CTRL1, BIT(24) },
	[SCU0_RESET_XDMA0]	= { 1, SCU0_RESET_CTRL1, BIT(25) },
	[SCU0_RESET_XDMA1]	= { 1, SCU0_RESET_CTRL1, BIT(26) },
	[SCU0_RESET_H2X1]	= { 1, SCU0_RESET_CTRL1, BIT(27) },
	[SCU0_RESET_DP]	= { 1, SCU0_RESET_CTRL1, BIT(28) },
	[SCU0_RESET_DP_MCU]	= { 1, SCU0_RESET_CTRL1, BIT(29) },
	[SCU0_RESET_SSP]	= { 1, SCU0_RESET_CTRL1, BIT(30) },
	[SCU0_RESET_H2X0]	= { 1, SCU0_RESET_CTRL1, BIT(31) },
	[SCU0_RESET_PORTA_USB]	= { 1, SCU0_RESET_CTRL2, BIT(0) },
	[SCU0_RESET_PORTA_PHY3]	= { 1, SCU0_RESET_CTRL2, BIT(1) },
	[SCU0_RESET_PORTA_XHCI]	= { 1, SCU0_RESET_CTRL2, BIT(2) },
	[SCU0_RESET_PORTB_VHUB1]	= { 1, SCU0_RESET_CTRL2, BIT(3) },
	[SCU0_RESET_PORTB_PHY3]	= { 1, SCU0_RESET_CTRL2, BIT(4) },
	[SCU0_RESET_PORTB_XHCI]	= { 1, SCU0_RESET_CTRL2, BIT(5) },
	/*PORTA EHCI/VHUB share reset */
	[SCU0_RESET_PORTA_USB]	= { 1, SCU0_RESET_CTRL2, BIT(6) },
	/*PORTB EHCI/VHUB share reset */
	[SCU0_RESET_PORTB_USB]	= { 1, SCU0_RESET_CTRL2, BIT(7) },
	[SCU0_RESET_UHCI]	= { 1, SCU0_RESET_CTRL2, BIT(8) },
	[SCU0_RESET_TSP]	= { 1, SCU0_RESET_CTRL2, BIT(9) },
	[SCU0_RESET_E2M0]	= { 1, SCU0_RESET_CTRL2, BIT(10) },
	[SCU0_RESET_E2M1]	= { 1, SCU0_RESET_CTRL2, BIT(11) },
	[SCU0_RESET_VLINK]	= { 1, SCU0_RESET_CTRL2, BIT(12) },
};

static const struct ast2700_reset_signal ast2700_reset1_signals[] = {
	[SCU1_RESET_LPC0] = { 1, SCU1_RESET_CTRL1, BIT(0) },
	[SCU1_RESET_LPC1] = { 1, SCU1_RESET_CTRL1, BIT(1) },
	[SCU1_RESET_MII]     = { 1, SCU1_RESET_CTRL1, BIT(2) },
	[SCU1_RESET_PECI]	= { 1, SCU1_RESET_CTRL1, BIT(3) },
	[SCU1_RESET_PWM]	= { 1, SCU1_RESET_CTRL1, BIT(4) },
	[SCU1_RESET_MAC0]	= { 1, SCU1_RESET_CTRL1, BIT(5) },
	[SCU1_RESET_MAC1]	= { 1, SCU1_RESET_CTRL1, BIT(6) },
	[SCU1_RESET_MAC2]	= { 1, SCU1_RESET_CTRL1, BIT(7) },
	[SCU1_RESET_ADC]	= { 1, SCU1_RESET_CTRL1, BIT(8) },
	[SCU1_RESET_SD]		= { 1, SCU1_RESET_CTRL1, BIT(9) },
	[SCU1_RESET_ESPI0]		= { 1, SCU1_RESET_CTRL1, BIT(10) },
	[SCU1_RESET_ESPI1]		= { 1, SCU1_RESET_CTRL1, BIT(11) },
	[SCU1_RESET_JTAG1]		= { 1, SCU1_RESET_CTRL1, BIT(12) },
	[SCU1_RESET_SPI0]	= { 1, SCU1_RESET_CTRL1, BIT(13) },
	[SCU1_RESET_SPI1]	= { 1, SCU1_RESET_CTRL1, BIT(14) },
	[SCU1_RESET_SPI2]	= { 1, SCU1_RESET_CTRL1, BIT(15) },
	[SCU1_RESET_I3C0]	= { 1, SCU1_RESET_CTRL1, BIT(16) },
	[SCU1_RESET_I3C1]	= { 1, SCU1_RESET_CTRL1, BIT(17) },
	[SCU1_RESET_I3C2]	= { 1, SCU1_RESET_CTRL1, BIT(18) },
	[SCU1_RESET_I3C3]	= { 1, SCU1_RESET_CTRL1, BIT(19) },
	[SCU1_RESET_I3C4]		= { 1, SCU1_RESET_CTRL1, BIT(20) },
	[SCU1_RESET_I3C5]	= { 1, SCU1_RESET_CTRL1, BIT(21) },
	[SCU1_RESET_I3C6]	= { 1, SCU1_RESET_CTRL1, BIT(22) },
	[SCU1_RESET_I3C7] = { 1, SCU1_RESET_CTRL1, BIT(23) },
	[SCU1_RESET_I3C8]		= { 1, SCU1_RESET_CTRL1, BIT(24) },
	[SCU1_RESET_I3C9]	= { 1, SCU1_RESET_CTRL1, BIT(25) },
	[SCU1_RESET_I3C10]	= { 1, SCU1_RESET_CTRL1, BIT(26) },
	[SCU1_RESET_I3C11]	= { 1, SCU1_RESET_CTRL1, BIT(27) },
	[SCU1_RESET_I3C12]	= { 1, SCU1_RESET_CTRL1, BIT(28) },
	[SCU1_RESET_I3C13]	= { 1, SCU1_RESET_CTRL1, BIT(29) },
	[SCU1_RESET_I3C14]	= { 1, SCU1_RESET_CTRL1, BIT(30) },
	[SCU1_RESET_I3C15]	= { 1, SCU1_RESET_CTRL1, BIT(31) },
	[SCU1_RESET_MCU0]	= { 1, SCU1_RESET_CTRL2, BIT(0) },
	[SCU1_RESET_MCU1]	= { 1, SCU1_RESET_CTRL2, BIT(1) },
	[SCU1_RESET_H2A_SPI1]	= { 1, SCU1_RESET_CTRL2, BIT(2) },
	[SCU1_RESET_H2A_SPI2]	= { 1, SCU1_RESET_CTRL2, BIT(3) },
	[SCU1_RESET_UART0]	= { 1, SCU1_RESET_CTRL2, BIT(4) },
	[SCU1_RESET_UART1]	= { 1, SCU1_RESET_CTRL2, BIT(5) },
	[SCU1_RESET_UART2]	= { 1, SCU1_RESET_CTRL2, BIT(6) },
	[SCU1_RESET_UART3]	= { 1, SCU1_RESET_CTRL2, BIT(7) },
	[SCU1_RESET_I2C_FILTER]	= { 1, SCU1_RESET_CTRL2, BIT(8) },
	[SCU1_RESET_CALIPTRA]	= { 1, SCU1_RESET_CTRL2, BIT(9) },
	[SCU1_RESET_XDMA]	= { 1, SCU1_RESET_CTRL2, BIT(10) },
	[SCU1_RESET_FSI]	= { 1, SCU1_RESET_CTRL2, BIT(12) },
	[SCU1_RESET_CAN]	= { 1, SCU1_RESET_CTRL2, BIT(13) },
	[SCU1_RESET_MCTP]	= { 1, SCU1_RESET_CTRL2, BIT(14) },
	[SCU1_RESET_I2C]	= { 1, SCU1_RESET_CTRL2, BIT(15) },
	[SCU1_RESET_UART6]	= { 1, SCU1_RESET_CTRL2, BIT(16) },
	[SCU1_RESET_UART7]	= { 1, SCU1_RESET_CTRL2, BIT(17) },
	[SCU1_RESET_UART8]	= { 1, SCU1_RESET_CTRL2, BIT(18) },
	[SCU1_RESET_UART9]	= { 1, SCU1_RESET_CTRL2, BIT(19) },
	[SCU1_RESET_LTPI0]	= { 1, SCU1_RESET_CTRL2, BIT(20) },
	[SCU1_RESET_VGAL]	= { 1, SCU1_RESET_CTRL2, BIT(21) },
	[SCU1_RESET_LTPI1]	= { 1, SCU1_RESET_CTRL2, BIT(22) },
	[SCU1_RESET_ACE]	= { 1, SCU1_RESET_CTRL2, BIT(23) },
	[SCU1_RESET_E2M]	= { 1, SCU1_RESET_CTRL2, BIT(24) },
	[SCU1_RESET_UHCI]	= { 1, SCU1_RESET_CTRL2, BIT(25) },
	[SCU1_RESET_PORTC_USB2UART]	= { 1, SCU1_RESET_CTRL2, BIT(26) },
	/*PORTC EHCI/VHUB share reset */
	[SCU1_RESET_PORTC_USB]	= { 1, SCU1_RESET_CTRL2, BIT(27) },
	[SCU1_RESET_PORTD_USB2UART]	= { 1, SCU1_RESET_CTRL2, BIT(28) },
	/*PORTC EHCI/VHUB share reset */
	[SCU1_RESET_PORTD_USB]	= { 1, SCU1_RESET_CTRL2, BIT(29) },
	[SCU1_RESET_H2X]	= { 1, SCU1_RESET_CTRL2, BIT(30) },
	[SCU1_RESET_I3CDMA]	= { 1, SCU1_RESET_CTRL2, BIT(31) },
	[SCU1_RESET_PCIE2RST]	= { 0, SCU1_PCIE3_CTRL, BIT(0) },
};

/* Globally visible clocks */
static DEFINE_SPINLOCK(ast2700_clk_lock);

/* Division of RGMII Clock */
static const struct clk_div_table ast2700_rgmii_div_table[] = {
	{ 0x0, 4 },
	{ 0x1, 4 },
	{ 0x2, 6 },
	{ 0x3, 8 },
	{ 0x4, 10 },
	{ 0x5, 12 },
	{ 0x6, 14 },
	{ 0x7, 16 },
	{ 0 }
};

/* Division of RMII Clock */
static const struct clk_div_table ast2700_rmii_div_table[] = {
	{ 0x0, 8 },
	{ 0x1, 8 },
	{ 0x2, 12 },
	{ 0x3, 16 },
	{ 0x4, 20 },
	{ 0x5, 24 },
	{ 0x6, 28 },
	{ 0x7, 32 },
	{ 0 }
};

/* Division of HCLK/SDIO/MAC/apll_divn CLK */
static const struct clk_div_table ast2700_clk_div_table[] = {
	{ 0x0, 2 },
	{ 0x1, 2 },
	{ 0x2, 3 },
	{ 0x3, 4 },
	{ 0x4, 5 },
	{ 0x5, 6 },
	{ 0x6, 7 },
	{ 0x7, 8 },
	{ 0 }
};

/* Division of PCLK/EMMC CLK */
static const struct clk_div_table ast2700_clk_div_table2[] = {
	{ 0x0, 2 },
	{ 0x1, 4 },
	{ 0x2, 6 },
	{ 0x3, 8 },
	{ 0x4, 10 },
	{ 0x5, 12 },
	{ 0x6, 14 },
	{ 0x7, 16 },
	{ 0 }
};

/* HPLL/DPLL: 2000Mhz(default) */
static struct clk_hw *ast2700_soc0_hw_pll(const char *name, const char *parent_name, u32 val)
{
	unsigned int mult, div;

	if (val & BIT(24)) {
		/* Pass through mode */
		mult = 1;
		div = 1;
	} else {
		/* F = CLKIN(25MHz) * [(M+1) / 2(N+1)] / (P+1) */
		u32 m = val & 0x1fff;
		u32 n = (val >> 13) & 0x3f;
		u32 p = (val >> 19) & 0xf;

		mult = (m + 1) / (2 * (n + 1));
		div = (p + 1);
	}

	return clk_hw_register_fixed_factor(NULL, name, parent_name, 0, mult, div);
};

/* MPLL 1600Mhz(default) */
static struct clk_hw *ast2700_calc_mpll(const char *name, const char *parent_name, u32 val)
{
	unsigned int mult, div;

	if (val & BIT(24)) {
		/* Pass through mode */
		mult = 1;
		div = 1;
	} else {
		/* F = CLKIN(25MHz) * [CLKF/(CLKR+1)] /(CLKOD+1) */
		u32 m = val & 0x1fff;
		u32 n = (val >> 13) & 0x3f;
		u32 p = (val >> 19) & 0xf;

		mult = m / (n + 1);
		div = (p + 1);
	}
	return clk_hw_register_fixed_factor(NULL, name, parent_name, 0, mult, div);
};

static struct clk_hw *ast2700_calc_uclk(const char *name, u32 val)
{
	unsigned int mult, div;

	/* UARTCLK = UXCLK * R / (N * 2) */
	u32 r = val & 0xff;
	u32 n = (val >> 8) & 0x3ff;

	mult = r;
	div = n * 2;

	return clk_hw_register_fixed_factor(NULL, name, "uxclk", 0, mult, div);
};

static struct clk_hw *ast2700_calc_huclk(const char *name, u32 val)
{
	unsigned int mult, div;

	/* UARTCLK = UXCLK * R / (N * 2) */
	u32 r = val & 0xff;
	u32 n = (val >> 8) & 0x3ff;

	mult = r;
	div = n * 2;

	return clk_hw_register_fixed_factor(NULL, name, "huxclk", 0, mult, div);
};

static struct clk_hw *ast2700_calc_soc1_pll(const char *name, const char *parent_name, u32 val)
{
	unsigned int mult, div;

	if (val & BIT(24)) {
		/* Pass through mode */
		mult = 1;
		div = 1;
	} else {
		/* F = 25Mhz * [(M + 1) / (n + 1)] / (p + 1) */
		u32 m = val & 0x1fff;
		u32 n = (val >> 13) & 0x3f;
		u32 p = (val >> 19) & 0xf;

		mult = (m + 1) / (n + 1);
		div = (p + 1);
	}
	return clk_hw_register_fixed_factor(NULL, name, parent_name, 0, mult, div);
};

static int ast2700_clk_is_enabled(struct clk_hw *hw)
{
	struct clk_gate *gate = to_clk_gate(hw);
	u32 clk = BIT(gate->bit_idx);
	u32 reg;

	reg = readl(gate->reg);

	return !(reg & clk);
}

static int ast2700_clk_enable(struct clk_hw *hw)
{
	struct clk_gate *gate = to_clk_gate(hw);
	u32 clk = BIT(gate->bit_idx);

	if (readl(gate->reg) & clk)
		writel(clk, gate->reg + 0x04);

	return 0;
}

static void ast2700_clk_disable(struct clk_hw *hw)
{
	struct clk_gate *gate = to_clk_gate(hw);
	u32 clk = BIT(gate->bit_idx);

	/* Clock is set to enable, so use write to set register */
	writel(clk, gate->reg);
}

static const struct clk_ops ast2700_clk_gate_ops = {
	.enable = ast2700_clk_enable,
	.disable = ast2700_clk_disable,
	.is_enabled = ast2700_clk_is_enabled,
};

static struct clk_hw *ast2700_clk_hw_register_gate(struct device *dev, const char *name,
						   const char *parent_name, unsigned long flags,
						   void __iomem *reg, u8 clock_idx,
						   u8 clk_gate_flags, spinlock_t *lock)
{
	struct clk_gate *gate;
	struct clk_hw *hw;
	struct clk_init_data init;
	int ret = -EINVAL;

	gate = kzalloc(sizeof(*gate), GFP_KERNEL);
	if (!gate)
		return ERR_PTR(-ENOMEM);

	init.name = name;
	init.ops = &ast2700_clk_gate_ops;
	init.flags = flags;
	init.parent_names = parent_name ? &parent_name : NULL;
	init.num_parents = parent_name ? 1 : 0;

	gate->reg = reg;
	gate->bit_idx = clock_idx;
	gate->flags = clk_gate_flags;
	gate->lock = lock;
	gate->hw.init = &init;

	hw = &gate->hw;
	ret = clk_hw_register(dev, hw);
	if (ret) {
		kfree(gate);
		hw = ERR_PTR(ret);
	}

	return hw;
}

struct ast2700_reset {
	void __iomem *base;
	struct ast2700_reset_signal const *signal;
	struct reset_controller_dev rcdev;
};

#define to_rc_data(p) container_of(p, struct ast2700_reset, rcdev)

static int ast2700_reset_assert(struct reset_controller_dev *rcdev, unsigned long id)
{
	struct ast2700_reset *rc = to_rc_data(rcdev);

	if (rc->signal[id].dedicated_clr)
		writel(rc->signal[id].bit, rc->base);
	else
		writel(readl(rc->base) & ~rc->signal[id].bit, rc->base);

	return 0;
}

static int ast2700_reset_deassert(struct reset_controller_dev *rcdev, unsigned long id)
{
	struct ast2700_reset *rc = to_rc_data(rcdev);

	if (rc->signal[id].dedicated_clr)
		writel(rc->signal[id].bit, rc->base + 0x04);
	else
		writel(readl(rc->base) | rc->signal[id].bit, rc->base);

	return 0;
}

static int ast2700_reset_status(struct reset_controller_dev *rcdev, unsigned long id)
{
	struct ast2700_reset *rc = to_rc_data(rcdev);

	if (readl(rc->base) & rc->signal[id].bit)
		return 1;
	else
		return 0;
}

static const struct reset_control_ops ast2700_reset_ops = {
	.assert = ast2700_reset_assert,
	.deassert = ast2700_reset_deassert,
	.status = ast2700_reset_status,
};

static const char *const sdclk_sel[] = {
	"soc1-hpll",
	"soc1-apll",
};

static const char *const uartclk_sel[] = {
	"uartxclk",
	"huartxclk",
};

static const char *const uxclk_sel[] = {
	"soc1-apll_div4",
	"soc1-apll_div2",
	"soc1-apll",
	"soc1-hpll",
};

static int ast2700_soc1_clk_init(struct device_node *soc1_node)
{
	struct clk_hw_onecell_data *clk_data;
	struct ast2700_reset *reset;
	u32 uart_clk_source = 0;
	void __iomem *clk_base;
	struct clk_hw **clks;
	u32 val, id;
	int ret;

	clk_base = of_iomap(soc1_node, 0);
	if (IS_ERR(clk_base))
		return PTR_ERR(clk_base);

	clk_data = kzalloc(struct_size(clk_data, hws, SCU1_NUM_CLKS), GFP_KERNEL);
	if (!clk_data)
		return -ENOMEM;

	clk_data->num = SCU1_NUM_CLKS;
	clks = clk_data->hws;

	reset = kzalloc(sizeof(*reset), GFP_KERNEL);
	if (!reset)
		return -ENOMEM;

	reset->base = clk_base;
	reset->signal = ast2700_reset1_signals;

	reset->rcdev.owner = THIS_MODULE;
	reset->rcdev.nr_resets = ARRAY_SIZE(ast2700_reset1_signals);
	reset->rcdev.ops = &ast2700_reset_ops;
	reset->rcdev.of_node = soc1_node;

	ret = reset_controller_register(&reset->rcdev);
	if (ret) {
		pr_err("soc1 failed to register reset controller\n");
		return ret;
	}
	/*
	 * Ast2700 A0 workaround:
	 * I3C reset should assert all of the I3C controllers simultaneously.
	 * Otherwise, it may lead to failure in accessing I3C registers.
	 */
	for (id = SCU1_RESET_I3C0; id <= SCU1_RESET_I3C15; id++)
		ast2700_reset_assert(&reset->rcdev, id);

	clks[SCU1_CLKIN] =
		clk_hw_register_fixed_rate(NULL, "soc1-clkin", NULL, 0, SCU_CLK_25MHZ);

	val = readl(clk_base + SCU1_HPLL_PARAM);
	clks[SCU1_CLK_HPLL] = ast2700_calc_soc1_pll("soc1-hpll", "soc1-clkin", val);

	val = readl(clk_base + SCU1_APLL_PARAM);
	clks[SCU1_CLK_APLL] = ast2700_calc_soc1_pll("soc1-apll", "soc1-clkin", val);

	clks[SCU1_CLK_APLL_DIV2] =
		clk_hw_register_fixed_factor(NULL, "soc1-apll_div2", "soc1-apll", 0, 1, 2);

	clks[SCU1_CLK_APLL_DIV4] =
		clk_hw_register_fixed_factor(NULL, "soc1-apll_div4", "soc1-apll", 0, 1, 4);

	val = readl(clk_base + SCU1_DPLL_PARAM);
	clks[SCU1_CLK_DPLL] = ast2700_calc_soc1_pll("dpll", "soc1-clkin", val);

	clks[SCU1_CLK_UXCLK] =
		clk_hw_register_mux(NULL, "uxclk", uxclk_sel, ARRAY_SIZE(uxclk_sel),
				    0, clk_base + SCU1_CLK_SEL2,
				    0, 2, 0, &ast2700_clk_lock);

	val = readl(clk_base + SCU1_UXCLK_CTRL);
	clks[SCU1_CLK_UARTX] = ast2700_calc_uclk("uartxclk", val);

	clks[SCU1_CLK_HUXCLK] =
		clk_hw_register_mux(NULL, "huxclk", uxclk_sel, ARRAY_SIZE(uxclk_sel),
				    0, clk_base + SCU1_CLK_SEL2,
				    3, 2, 0, &ast2700_clk_lock);

	val = readl(clk_base + SCU1_HUXCLK_CTRL);
	clks[SCU1_CLK_HUARTX] = ast2700_calc_huclk("huartxclk", val);

	clks[SCU1_CLK_AHB] =
		clk_hw_register_divider_table(NULL, "soc1-ahb", "soc1-hpll",
					      0, clk_base + SCU1_CLK_SEL2,
					      20, 3, 0, ast2700_clk_div_table, &ast2700_clk_lock);

	clks[SCU1_CLK_APB] =
		clk_hw_register_divider_table(NULL, "soc1-apb", "soc1-hpll",
					      0, clk_base + SCU1_CLK_SEL1,
					      18, 3, 0, ast2700_clk_div_table2, &ast2700_clk_lock);

	clks[SCU1_CLK_RMII] =
		clk_hw_register_divider_table(NULL, "rmii", "soc1-hpll",
					      0, clk_base + SCU1_CLK_SEL1,
					      21, 3, 0, ast2700_rmii_div_table, &ast2700_clk_lock);

	clks[SCU1_CLK_MAC0RCLK] =
		clk_hw_register_gate(NULL, "mac0rclk", "rmii", 0,
				     clk_base + SCU1_MAC12_CLK_DLY, 29,
				     0, &ast2700_clk_lock);

	clks[SCU1_CLK_MAC1RCLK] =
		clk_hw_register_gate(NULL, "mac1rclk", "rmii", 0,
				     clk_base + SCU1_MAC12_CLK_DLY, 30,
				     0, &ast2700_clk_lock);

	clks[SCU1_CLK_RGMII] =
		clk_hw_register_divider_table(NULL, "rgmii", "soc1-hpll",
					      0, clk_base + SCU1_CLK_SEL1,
					      25, 3, 0, ast2700_rgmii_div_table, &ast2700_clk_lock);

	clks[SCU1_CLK_MACHCLK] =
		clk_hw_register_divider_table(NULL, "machclk", "soc1-hpll",
					      0, clk_base + SCU1_CLK_SEL1,
					      29, 3, 0, ast2700_clk_div_table, &ast2700_clk_lock);

	clks[SCU1_CLK_GATE_LCLK0] =
		ast2700_clk_hw_register_gate(NULL, "lclk0-gate", NULL,
					     CLK_IS_CRITICAL, clk_base + SCU1_CLK_STOP,
					     0, 0, &ast2700_clk_lock);

	clks[SCU1_CLK_GATE_LCLK0] =
		ast2700_clk_hw_register_gate(NULL, "lclk1-gate", NULL,
					     CLK_IS_CRITICAL, clk_base + SCU1_CLK_STOP,
					     1, 0, &ast2700_clk_lock);

	clks[SCU1_CLK_GATE_ESPI0CLK] =
		ast2700_clk_hw_register_gate(NULL, "espi0clk-gate", NULL,
					     CLK_IS_CRITICAL, clk_base + SCU1_CLK_STOP,
					     2, 0, &ast2700_clk_lock);

	clks[SCU1_CLK_GATE_ESPI1CLK] =
		ast2700_clk_hw_register_gate(NULL, "espi1clk-gate", NULL,
					     CLK_IS_CRITICAL, clk_base + SCU1_CLK_STOP,
					     3, 0, &ast2700_clk_lock);

	clks[SCU1_CLK_APLL_DIVN] =
		clk_hw_register_divider_table(NULL, "soc1-apll_divn", "soc1-apll",
					      0, clk_base + SCU1_CLK_SEL2,
					      8, 3, 0, ast2700_clk_div_table, &ast2700_clk_lock);

	clks[SCU1_CLK_SDMUX] =
		clk_hw_register_mux(NULL, "sdclk-mux", sdclk_sel, ARRAY_SIZE(sdclk_sel),
				    0, clk_base + SCU1_CLK_SEL1,
				    13, 1, 0, &ast2700_clk_lock);

	clks[SCU1_CLK_SDCLK] =
		clk_hw_register_divider_table(NULL, "sdclk", "sdclk-mux",
					      0, clk_base + SCU1_CLK_SEL1,
					      14, 3, 0, ast2700_clk_div_table, &ast2700_clk_lock);

	clks[SCU1_CLK_GATE_SDCLK] =
		ast2700_clk_hw_register_gate(NULL, "sdclk-gate", "sdclk",
					     0, clk_base + SCU1_CLK_STOP,
					     4, 0, &ast2700_clk_lock);

	clks[SCU1_CLK_GATE_IPEREFCLK] =
		ast2700_clk_hw_register_gate(NULL, "soc1-refclk-gate", NULL,
					     CLK_IS_CRITICAL, clk_base + SCU1_CLK_STOP,
					     6, 0, &ast2700_clk_lock);

	clks[SCU1_CLK_GATE_LPCHCLK] =
		ast2700_clk_hw_register_gate(NULL, "lpchclk-gate", NULL,
					     CLK_IS_CRITICAL, clk_base + SCU1_CLK_STOP,
					     7, 0, &ast2700_clk_lock);

	clks[SCU1_CLK_GATE_MAC0CLK] =
		ast2700_clk_hw_register_gate(NULL, "mac0clk-gate", NULL,
					     0, clk_base + SCU1_CLK_STOP,
					     8, 0, &ast2700_clk_lock);

	clks[SCU1_CLK_GATE_MAC1CLK] =
		ast2700_clk_hw_register_gate(NULL, "mac1clk-gate", NULL,
					     0, clk_base + SCU1_CLK_STOP,
					     9, 0, &ast2700_clk_lock);

	clks[SCU1_CLK_GATE_MAC2CLK] =
		ast2700_clk_hw_register_gate(NULL, "mac2clk-gate", NULL,
					     0, clk_base + SCU1_CLK_STOP,
					     10, 0, &ast2700_clk_lock);

	of_property_read_u32(soc1_node, "uart-clk-source", &uart_clk_source);
	if (uart_clk_source) {
		val = readl(clk_base + SCU1_CLK_SEL1) & ~GENMASK(12, 0);
		uart_clk_source &= GENMASK(12, 0);
		writel(val | uart_clk_source, clk_base + SCU1_CLK_SEL1);
	}

	clks[SCU1_CLK_UART0] =
		clk_hw_register_mux(NULL, "uart0clk", uartclk_sel, ARRAY_SIZE(uartclk_sel),
				    0, clk_base + SCU1_CLK_SEL1,
				    0, 1, 0, &ast2700_clk_lock);

	clks[SCU1_CLK_GATE_UART0CLK] =
		ast2700_clk_hw_register_gate(NULL, "uart0clk-gate", "uart0clk",
					     CLK_IS_CRITICAL, clk_base + SCU1_CLK_STOP,
					     11, 0, &ast2700_clk_lock);

	clks[SCU1_CLK_UART1] =
		clk_hw_register_mux(NULL, "uart1clk", uartclk_sel, ARRAY_SIZE(uartclk_sel),
				    0, clk_base + SCU1_CLK_SEL1,
				    1, 1, 0, &ast2700_clk_lock);

	clks[SCU1_CLK_GATE_UART1CLK] =
		ast2700_clk_hw_register_gate(NULL, "uart1clk-gate", "uart1clk",
					     CLK_IS_CRITICAL, clk_base + SCU1_CLK_STOP,
					     12, 0, &ast2700_clk_lock);

	clks[SCU1_CLK_UART2] =
		clk_hw_register_mux(NULL, "uart2clk", uartclk_sel, ARRAY_SIZE(uartclk_sel),
				    0, clk_base + SCU1_CLK_SEL1,
				    2, 1, 0, &ast2700_clk_lock);

	clks[SCU1_CLK_GATE_UART2CLK] =
		ast2700_clk_hw_register_gate(NULL, "uart2clk-gate", "uart2clk",
					     CLK_IS_CRITICAL, clk_base + SCU1_CLK_STOP,
					     13, 0, &ast2700_clk_lock);

	clks[SCU1_CLK_UART3] =
		clk_hw_register_mux(NULL, "uart3clk", uartclk_sel, ARRAY_SIZE(uartclk_sel),
				    0, clk_base + SCU1_CLK_SEL1,
				    3, 1, 0, &ast2700_clk_lock);

	clks[SCU1_CLK_GATE_UART3CLK] =
		ast2700_clk_hw_register_gate(NULL, "uart3clk-gate", "uart3clk",
					     CLK_IS_CRITICAL, clk_base + SCU1_CLK_STOP,
					     14, 0, &ast2700_clk_lock);

	clks[SCU1_CLK_GATE_I3C0CLK] =
		ast2700_clk_hw_register_gate(NULL, "i3c0clk-gate", "soc1-ahb",
					     0, clk_base + SCU1_CLK_STOP,
					     16, 0, &ast2700_clk_lock);

	clks[SCU1_CLK_GATE_I3C1CLK] =
		ast2700_clk_hw_register_gate(NULL, "i3c1clk-gate", "soc1-ahb",
					     0, clk_base + SCU1_CLK_STOP,
					     17, 0, &ast2700_clk_lock);

	clks[SCU1_CLK_GATE_I3C2CLK] =
		ast2700_clk_hw_register_gate(NULL, "i3c2clk-gate", "soc1-ahb",
					     0, clk_base + SCU1_CLK_STOP,
					     18, 0, &ast2700_clk_lock);

	clks[SCU1_CLK_GATE_I3C3CLK] =
		ast2700_clk_hw_register_gate(NULL, "i3c3clk-gate", "soc1-ahb",
					     0, clk_base + SCU1_CLK_STOP,
					     19, 0, &ast2700_clk_lock);

	clks[SCU1_CLK_GATE_I3C4CLK] =
		ast2700_clk_hw_register_gate(NULL, "i3c4clk-gate", "soc1-ahb",
					     0, clk_base + SCU1_CLK_STOP,
					     20, 0, &ast2700_clk_lock);

	clks[SCU1_CLK_GATE_I3C5CLK] =
		ast2700_clk_hw_register_gate(NULL, "i3c5clk-gate", "soc1-ahb",
					     0, clk_base + SCU1_CLK_STOP,
					     21, 0, &ast2700_clk_lock);

	clks[SCU1_CLK_GATE_I3C6CLK] =
		ast2700_clk_hw_register_gate(NULL, "i3c6clk-gate", "soc1-ahb",
					     0, clk_base + SCU1_CLK_STOP,
					     22, 0, &ast2700_clk_lock);

	clks[SCU1_CLK_GATE_I3C7CLK] =
		ast2700_clk_hw_register_gate(NULL, "i3c7clk-gate", "soc1-ahb",
					     0, clk_base + SCU1_CLK_STOP,
					     23, 0, &ast2700_clk_lock);

	clks[SCU1_CLK_GATE_I3C8CLK] =
		ast2700_clk_hw_register_gate(NULL, "i3c8clk-gate", "soc1-ahb",
					     0, clk_base + SCU1_CLK_STOP,
					     24, 0, &ast2700_clk_lock);

	clks[SCU1_CLK_GATE_I3C9CLK] =
		ast2700_clk_hw_register_gate(NULL, "i3c9clk-gate", "soc1-ahb",
					     0, clk_base + SCU1_CLK_STOP,
					     25, 0, &ast2700_clk_lock);

	clks[SCU1_CLK_GATE_I3C10CLK] =
		ast2700_clk_hw_register_gate(NULL, "i3c10clk-gate", "soc1-ahb",
					     0, clk_base + SCU1_CLK_STOP,
					     26, 0, &ast2700_clk_lock);

	clks[SCU1_CLK_GATE_I3C11CLK] =
		ast2700_clk_hw_register_gate(NULL, "i3c11clk-gate", "soc1-ahb",
					     0, clk_base + SCU1_CLK_STOP,
					     27, 0, &ast2700_clk_lock);

	clks[SCU1_CLK_GATE_I3C12CLK] =
		ast2700_clk_hw_register_gate(NULL, "i3c12clk-gate", "soc1-ahb",
					     0, clk_base + SCU1_CLK_STOP,
					     28, 0, &ast2700_clk_lock);

	clks[SCU1_CLK_GATE_I3C13CLK] =
		ast2700_clk_hw_register_gate(NULL, "i3c13clk-gate", "soc1-ahb",
					     0, clk_base + SCU1_CLK_STOP,
					     29, 0, &ast2700_clk_lock);

	clks[SCU1_CLK_GATE_I3C14CLK] =
		ast2700_clk_hw_register_gate(NULL, "i3c14clk-gate", "soc1-ahb",
					     0, clk_base + SCU1_CLK_STOP,
					     30, 0, &ast2700_clk_lock);

	clks[SCU1_CLK_GATE_I3C15CLK] =
		ast2700_clk_hw_register_gate(NULL, "i3c15clk-gate", "soc1-ahb",
					     0, clk_base + SCU1_CLK_STOP,
					     31, 0, &ast2700_clk_lock);

	clks[SCU1_CLK_UART5] =
		clk_hw_register_mux(NULL, "uart5clk", uartclk_sel, ARRAY_SIZE(uartclk_sel),
				    0, clk_base + SCU1_CLK_SEL1,
				    5, 1, 0, &ast2700_clk_lock);

	clks[SCU1_CLK_GATE_UART5CLK] =
		ast2700_clk_hw_register_gate(NULL, "uart5clk-gate", "uart5clk",
					     CLK_IS_CRITICAL, clk_base + SCU1_CLK_STOP2,
					     0, 0, &ast2700_clk_lock);

	clks[SCU1_CLK_UART6] =
		clk_hw_register_mux(NULL, "uart6clk", uartclk_sel, ARRAY_SIZE(uartclk_sel),
				    0, clk_base + SCU1_CLK_SEL1,
				    6, 1, 0, &ast2700_clk_lock);

	clks[SCU1_CLK_GATE_UART6CLK] =
		ast2700_clk_hw_register_gate(NULL, "uart6clk-gate", "uart6clk",
					     CLK_IS_CRITICAL, clk_base + SCU1_CLK_STOP2,
					     1, 0, &ast2700_clk_lock);

	clks[SCU1_CLK_UART7] =
		clk_hw_register_mux(NULL, "uart7clk", uartclk_sel, ARRAY_SIZE(uartclk_sel),
				    0, clk_base + SCU1_CLK_SEL1,
				    7, 1, 0, &ast2700_clk_lock);

	clks[SCU1_CLK_GATE_UART7CLK] =
		ast2700_clk_hw_register_gate(NULL, "uart7clk-gate", "uart7clk",
					     CLK_IS_CRITICAL, clk_base + SCU1_CLK_STOP2,
					     2, 0, &ast2700_clk_lock);

	clks[SCU1_CLK_UART8] =
		clk_hw_register_mux(NULL, "uart8clk", uartclk_sel, ARRAY_SIZE(uartclk_sel),
				    0, clk_base + SCU1_CLK_SEL1,
				    8, 1, 0, &ast2700_clk_lock);

	clks[SCU1_CLK_GATE_UART8CLK] =
		ast2700_clk_hw_register_gate(NULL, "uart8clk-gate", "uart8clk",
					     CLK_IS_CRITICAL, clk_base + SCU1_CLK_STOP2,
					     3, 0, &ast2700_clk_lock);

	clks[SCU1_CLK_UART9] =
		clk_hw_register_mux(NULL, "uart9clk", uartclk_sel, ARRAY_SIZE(uartclk_sel),
				    0, clk_base + SCU1_CLK_SEL1,
				    9, 1, 0, &ast2700_clk_lock);

	clks[SCU1_CLK_GATE_UART9CLK] =
		ast2700_clk_hw_register_gate(NULL, "uart9clk-gate", "uart9clk",
					     0, clk_base + SCU1_CLK_STOP2,
					     4, 0, &ast2700_clk_lock);

	clks[SCU1_CLK_UART10] =
		clk_hw_register_mux(NULL, "uart10clk", uartclk_sel, ARRAY_SIZE(uartclk_sel),
				    0, clk_base + SCU1_CLK_SEL1,
				    10, 1, 0, &ast2700_clk_lock);

	clks[SCU1_CLK_GATE_UART10CLK] =
		ast2700_clk_hw_register_gate(NULL, "uart10clk-gate", "uart10clk",
					     0, clk_base + SCU1_CLK_STOP2,
					     5, 0, &ast2700_clk_lock);

	clks[SCU1_CLK_UART11] =
		clk_hw_register_mux(NULL, "uart11clk", uartclk_sel, ARRAY_SIZE(uartclk_sel),
				    0, clk_base + SCU1_CLK_SEL1,
				    11, 1, 0, &ast2700_clk_lock);

	clks[SCU1_CLK_GATE_UART11CLK] =
		ast2700_clk_hw_register_gate(NULL, "uart11clk-gate", "uart11clk",
					     CLK_IS_CRITICAL, clk_base + SCU1_CLK_STOP2,
					     6, 0, &ast2700_clk_lock);

	clks[SCU1_CLK_UART12] =
		clk_hw_register_mux(NULL, "uart12clk", uartclk_sel, ARRAY_SIZE(uartclk_sel),
				    0, clk_base + SCU1_CLK_SEL1,
				    12, 1, 0, &ast2700_clk_lock);

	clks[SCU1_CLK_GATE_UART12CLK] =
		ast2700_clk_hw_register_gate(NULL, "uart12clk-gate", "uart12clk",
					     CLK_IS_CRITICAL, clk_base + SCU1_CLK_STOP2,
					     7, 0, &ast2700_clk_lock);

	clks[SCU1_CLK_GATE_FSICLK] =
		ast2700_clk_hw_register_gate(NULL, "fsiclk-gate", NULL,
					     0, clk_base + SCU1_CLK_STOP2,
					     8, 0, &ast2700_clk_lock);

	clks[SCU1_CLK_GATE_LTPIPHYCLK] =
		ast2700_clk_hw_register_gate(NULL, "ltpiphyclk-gate", NULL,
					     0, clk_base + SCU1_CLK_STOP2,
					     9, 0, &ast2700_clk_lock);

	clks[SCU1_CLK_GATE_LTPICLK] =
		ast2700_clk_hw_register_gate(NULL, "ltpiclk-gate", NULL,
					     0, clk_base + SCU1_CLK_STOP2,
					     10, 0, &ast2700_clk_lock);

	clks[SCU1_CLK_GATE_VGALCLK] =
		ast2700_clk_hw_register_gate(NULL, "vgalclk-gate", NULL,
					     CLK_IS_CRITICAL, clk_base + SCU1_CLK_STOP2,
					     11, 0, &ast2700_clk_lock);

	clks[SCU1_CLK_GATE_USBUARTCLK] =
		ast2700_clk_hw_register_gate(NULL, "usbuartclk-gate", NULL,
					     0, clk_base + SCU1_CLK_STOP2,
					     12, 0, &ast2700_clk_lock);

	clk_hw_register_fixed_factor(NULL, "canclk", "soc1-apll", 0, 1, 10);

	clks[SCU1_CLK_GATE_CANCLK] =
		ast2700_clk_hw_register_gate(NULL, "canclk-gate", "canclk",
					     0, clk_base + SCU1_CLK_STOP2,
					     13, 0, &ast2700_clk_lock);

	clks[SCU1_CLK_GATE_PCICLK] =
		ast2700_clk_hw_register_gate(NULL, "pciclk-gate", NULL,
					     0, clk_base + SCU1_CLK_STOP2,
					     14, 0, &ast2700_clk_lock);

	clks[SCU1_CLK_GATE_SLICLK] =
		ast2700_clk_hw_register_gate(NULL, "sliclk-gate", NULL,
					     CLK_IS_CRITICAL, clk_base + SCU1_CLK_STOP2,
					     15, 0, &ast2700_clk_lock);

	of_clk_add_hw_provider(soc1_node, of_clk_hw_onecell_get, clk_data);

	return 0;
};

static const char *const pspclk_sel[] = {
	"soc0-mpll",
	"soc0-hpll",
};

static const char *const soc0_uartclk_sel[] = {
	"soc0-clk24Mhz",
	"soc0-clk192Mhz",
};

static const char *const emmcclk_sel[] = {
	"soc0-mpll_div4",
	"soc0-hpll_div4",
};

static int ast2700_soc0_clk_init(struct device_node *soc0_node)
{
	struct clk_hw_onecell_data *clk_data;
	void __iomem *clk_base;
	struct ast2700_reset *reset;
	struct clk_hw **clks;
	int div;
	u32 val;
	int ret;

	clk_data = kzalloc(struct_size(clk_data, hws, SCU0_NUM_CLKS), GFP_KERNEL);
	if (!clk_data)
		return -ENOMEM;

	clk_data->num = SCU0_NUM_CLKS;
	clks = clk_data->hws;

	clk_base = of_iomap(soc0_node, 0);
	if (WARN_ON(IS_ERR(clk_base)))
		return PTR_ERR(clk_base);

	reset = kzalloc(sizeof(*reset), GFP_KERNEL);
	if (!reset)
		return -ENOMEM;

	reset->base = clk_base;

	reset->rcdev.owner = THIS_MODULE;
	reset->rcdev.nr_resets = ARRAY_SIZE(ast2700_reset0_signals);
	reset->rcdev.ops = &ast2700_reset_ops;
	reset->rcdev.of_node = soc0_node;

	ret = reset_controller_register(&reset->rcdev);
	if (ret) {
		pr_err("soc0 failed to register reset controller\n");
		return ret;
	}

	clks[SCU0_CLKIN] =
		clk_hw_register_fixed_rate(NULL, "soc0-clkin", NULL, 0, SCU_CLK_25MHZ);

	clks[SCU0_CLK_24M] =
		clk_hw_register_fixed_rate(NULL, "soc0-clk24Mhz", NULL, 0, SCU_CLK_24MHZ);

	clks[SCU0_CLK_192M] =
		clk_hw_register_fixed_rate(NULL, "soc0-clk192Mhz", NULL, 0, SCU_CLK_192MHZ);

	val = readl(clk_base + SCU0_HWSTRAP1);
	if ((val & GENMASK(3, 2)) != 0) {
		switch ((val & GENMASK(3, 2)) >> 2) {
		case 1:
			clks[SCU0_CLK_HPLL] =
				clk_hw_register_fixed_rate(NULL, "soc0-hpll", NULL, 0, 1900000000);
			break;
		case 2:
			clks[SCU0_CLK_HPLL] =
				clk_hw_register_fixed_rate(NULL, "soc0-hpll", NULL, 0, 1800000000);
			break;
		case 3:
			clks[SCU0_CLK_HPLL] =
				clk_hw_register_fixed_rate(NULL, "soc0-hpll", NULL, 0, 1700000000);
			break;
		}
	} else {
		val = readl(clk_base + SCU0_HPLL_PARAM);
		clks[SCU0_CLK_HPLL] = ast2700_soc0_hw_pll("soc0-hpll", "soc0-clkin", val);
	}
	clks[SCU0_CLK_HPLL_DIV2] =
			clk_hw_register_fixed_factor(NULL, "soc0-hpll_div2", "soc0-hpll", 0, 1, 2);
	clks[SCU0_CLK_HPLL_DIV4] =
			clk_hw_register_fixed_factor(NULL, "soc0-hpll_div4", "soc0-hpll", 0, 1, 4);

	val = readl(clk_base + SCU0_DPLL_PARAM);
	clks[SCU0_CLK_DPLL] = ast2700_soc0_hw_pll("dpll", "soc0-clkin", val);

	val = readl(clk_base + SCU0_MPLL_PARAM);
	clks[SCU0_CLK_MPLL] = ast2700_calc_mpll("soc0-mpll", "soc0-clkin", val);
	clks[SCU0_CLK_MPLL_DIV2] =
			clk_hw_register_fixed_factor(NULL, "soc0-mpll_div2", "soc0-mpll", 0, 1, 2);
	clks[SCU0_CLK_MPLL_DIV4] =
			clk_hw_register_fixed_factor(NULL, "soc0-mpll_div4", "soc0-mpll", 0, 1, 4);
	clks[SCU0_CLK_MPLL_DIV8] =
			clk_hw_register_fixed_factor(NULL, "soc0-mpll_div8", "soc0-mpll", 0, 1, 8);

	val = readl(clk_base + SCU0_D0CLK_PARAM);
	clks[SCU0_CLK_D0] = ast2700_soc0_hw_pll("d0clk", "soc0-clkin", val);

	val = readl(clk_base + SCU0_D1CLK_PARAM);
	clks[SCU0_CLK_D1] = ast2700_soc0_hw_pll("d1clk", "soc0-clkin", val);

	val = readl(clk_base + SCU0_CRT0CLK_PARAM);
	clks[SCU0_CLK_CRT0] = ast2700_soc0_hw_pll("crt0clk", "soc0-clkin", val);

	val = readl(clk_base + SCU0_CRT1CLK_PARAM);
	clks[SCU0_CLK_CRT1] = ast2700_soc0_hw_pll("crt1clk", "soc0-clkin", val);

	val = readl(clk_base + SCU0_MPHYCLK_PARAM);
	clks[SCU0_CLK_MPHY] =
		clk_hw_register_fixed_factor(NULL, "mphyclk", "soc0-hpll", 0, 1, val + 1);

	clks[SCU0_CLK_PSP] =
		clk_hw_register_mux(NULL, "pspclk", pspclk_sel, ARRAY_SIZE(pspclk_sel),
				    0, clk_base + SCU0_HWSTRAP1,
				    4, 1, 0, &ast2700_clk_lock);

	clks[SCU0_CLK_AXI0] =
		clk_hw_register_fixed_factor(NULL, "axi0clk", "pspclk", 0, 1, 2);

	val = readl(clk_base + SCU0_HWSTRAP1);
	if (val & BIT(7)) {
		clks[SCU0_CLK_AHB] =
			clk_hw_register_divider_table(NULL, "soc0-ahb", "soc0-hpll",
						      0, clk_base + SCU0_HWSTRAP1,
						      5, 2, 0, ast2700_clk_div_table,
						      &ast2700_clk_lock);
	} else {
		clks[SCU0_CLK_AHB] =
			clk_hw_register_divider_table(NULL, "soc0-ahb", "soc0-mpll",
						      0, clk_base + SCU0_HWSTRAP1,
						      5, 2, 0, ast2700_clk_div_table,
						      &ast2700_clk_lock);
	}

	clks[SCU0_CLK_AXI1] =
		clk_hw_register_fixed_factor(NULL, "axi1clk", "soc0-ahb", 0, 1, 2);

	clks[SCU0_CLK_APB] =
		clk_hw_register_divider_table(NULL, "soc0-apb", "axi0clk",
					      0, clk_base + SCU0_CLK_SEL1,
					      23, 3, 0, ast2700_clk_div_table2, &ast2700_clk_lock);

	clks[SCU0_CLK_GATE_MCLK] =
		ast2700_clk_hw_register_gate(NULL, "mclk", "soc0-mpll",
					     CLK_IS_CRITICAL, clk_base + SCU0_CLK_STOP,
					     0, 0, &ast2700_clk_lock);

	clks[SCU0_CLK_GATE_ECLK] =
		ast2700_clk_hw_register_gate(NULL, "eclk", NULL,
					     0, clk_base + SCU0_CLK_STOP,
					     1, 0, &ast2700_clk_lock);

	clks[SCU0_CLK_GATE_2DCLK] =
		ast2700_clk_hw_register_gate(NULL, "gclk", NULL,
					     CLK_IS_CRITICAL, clk_base + SCU0_CLK_STOP,
					     2, 0, &ast2700_clk_lock);

	clks[SCU0_CLK_GATE_VCLK] =
		ast2700_clk_hw_register_gate(NULL, "vclk", NULL,
					     0, clk_base + SCU0_CLK_STOP,
					     3, 0, &ast2700_clk_lock);

	clks[SCU0_CLK_GATE_BCLK] =
		ast2700_clk_hw_register_gate(NULL, "bclk", NULL,
					     CLK_IS_CRITICAL, clk_base + SCU0_CLK_STOP,
					     4, 0, &ast2700_clk_lock);

	clks[SCU0_CLK_GATE_VGA0CLK] =
		ast2700_clk_hw_register_gate(NULL, "d1clk-gate", NULL,
					     CLK_IS_CRITICAL, clk_base + SCU0_CLK_STOP,
					     5, 0, &ast2700_clk_lock);

	clks[SCU0_CLK_GATE_REFCLK] =
		ast2700_clk_hw_register_gate(NULL, "soc0-refclk-gate", "soc0-clkin",
					     CLK_IS_CRITICAL, clk_base + SCU0_CLK_STOP,
					     6, 0, &ast2700_clk_lock);

	div = (GET_USB_REFCLK_DIV(readl(clk_base + SCU0_CLK_SEL2)) + 1) * 2;
	clks[SCU0_CLK_U2PHY_REFCLK] =
		clk_hw_register_fixed_factor(NULL, "xhci_ref_clk", "soc0-mpll_div8", 0, 1, div);

	clks[SCU0_CLK_U2PHY_CLK12M] =
		clk_hw_register_fixed_rate(NULL, "xhci_suspend_clk", NULL, 0, SCU_CLK_12MHZ);

	clks[SCU0_CLK_GATE_PORTBUSB2CLK] =
		ast2700_clk_hw_register_gate(NULL, "portb-usb2clk", NULL,
					     0, clk_base + SCU0_CLK_STOP,
					     7, 0, &ast2700_clk_lock);

	clks[SCU0_CLK_GATE_UHCICLK] =
		ast2700_clk_hw_register_gate(NULL, "uhciclk", NULL,
					     0, clk_base + SCU0_CLK_STOP,
					     9, 0, &ast2700_clk_lock);

	clks[SCU0_CLK_GATE_VGA1CLK] =
		ast2700_clk_hw_register_gate(NULL, "d2clk-gate", NULL,
					     CLK_IS_CRITICAL, clk_base + SCU0_CLK_STOP,
					     10, 0, &ast2700_clk_lock);

	clks[SCU0_CLK_GATE_HACCLK] =
		ast2700_clk_hw_register_gate(NULL, "hac-clk", NULL,
					     0, clk_base + SCU0_CLK_STOP,
					     13, 0, &ast2700_clk_lock);

	clks[SCU0_CLK_GATE_PORTAUSB2CLK] =
		ast2700_clk_hw_register_gate(NULL, "porta-usb2clk", NULL,
					     0, clk_base + SCU0_CLK_STOP,
					     14, 0, &ast2700_clk_lock);

	clks[SCU0_CLK_UART] =
		clk_hw_register_mux(NULL, "soc0-uartclk", soc0_uartclk_sel,
				    ARRAY_SIZE(soc0_uartclk_sel),
				    0, clk_base + SCU0_CLK_SEL2,
				    14, 1, 0, &ast2700_clk_lock);

	if (readl(clk_base + SCU0_CLK_SEL2) & UART_DIV13_EN)
		div = 13;
	else
		div = 1;

	clks[SCU0_CLK_UART4] =
		clk_hw_register_fixed_factor(NULL, "uart4clk", "soc0-uartclk", 0, 1, div);

	clks[SCU0_CLK_GATE_UART4CLK] =
		ast2700_clk_hw_register_gate(NULL, "uart4clk-gate", "uart4clk",
					     CLK_IS_CRITICAL, clk_base + SCU0_CLK_STOP,
					     15, 0, &ast2700_clk_lock);

	clks[SCU0_CLK_GATE_DACCLK] =
		ast2700_clk_hw_register_gate(NULL, "dacclk", NULL,
					     CLK_IS_CRITICAL, clk_base + SCU0_CLK_STOP,
					     17, 0, &ast2700_clk_lock);

	clks[SCU0_CLK_GATE_CRT0CLK] =
		ast2700_clk_hw_register_gate(NULL, "crt0clk-gate", NULL,
					     0, clk_base + SCU0_CLK_STOP,
					     20, 0, &ast2700_clk_lock);

	clks[SCU0_CLK_GATE_CRT1CLK] =
		ast2700_clk_hw_register_gate(NULL, "crt1clk-gate", NULL,
					     0, clk_base + SCU0_CLK_STOP,
					     21, 0, &ast2700_clk_lock);

	clks[SCU0_CLK_GATE_ECDSACLK] =
		ast2700_clk_hw_register_gate(NULL, "eccclk", NULL,
					     0, clk_base + SCU0_CLK_STOP,
					     23, 0, &ast2700_clk_lock);

	clks[SCU0_CLK_GATE_RSACLK] =
		ast2700_clk_hw_register_gate(NULL, "rsaclk", NULL,
					     0, clk_base + SCU0_CLK_STOP,
					     24, 0, &ast2700_clk_lock);

	clks[SCU0_CLK_GATE_RVAS0CLK] =
		ast2700_clk_hw_register_gate(NULL, "rvasclk", NULL,
					     0, clk_base + SCU0_CLK_STOP,
					     25, 0, &ast2700_clk_lock);

	clks[SCU0_CLK_GATE_UFSCLK] =
		ast2700_clk_hw_register_gate(NULL, "ufsclk", NULL,
					     0, clk_base + SCU0_CLK_STOP,
					     26, 0, &ast2700_clk_lock);

	clks[SCU0_CLK_EMMCMUX] =
		clk_hw_register_mux(NULL, "emmcsrc-mux", emmcclk_sel, ARRAY_SIZE(emmcclk_sel),
				    0, clk_base + SCU0_CLK_SEL1,
				    11, 1, 0, &ast2700_clk_lock);

	clks[SCU0_CLK_EMMC] =
		clk_hw_register_divider_table(NULL, "emmcclk", "emmcsrc-mux",
					      0, clk_base + SCU0_CLK_SEL1,
					      12, 3, 0, ast2700_clk_div_table2,
					      &ast2700_clk_lock);

	clks[SCU0_CLK_GATE_EMMCCLK] =
		ast2700_clk_hw_register_gate(NULL, "emmcclk-gate", "emmcclk",
					     0, clk_base + SCU0_CLK_STOP,
					     27, 0, &ast2700_clk_lock);

	clks[SCU0_CLK_GATE_RVAS1CLK] =
		ast2700_clk_hw_register_gate(NULL, "rvas2clk", NULL,
					     0, clk_base + SCU0_CLK_STOP,
					     28, 0, &ast2700_clk_lock);

	of_clk_add_hw_provider(soc0_node, of_clk_hw_onecell_get, clk_data);

	return 0;
};

CLK_OF_DECLARE_DRIVER(ast2700_soc0, "aspeed,ast2700-scu0", ast2700_soc0_clk_init);
CLK_OF_DECLARE_DRIVER(ast2700_soc1, "aspeed,ast2700-scu1", ast2700_soc1_clk_init);
