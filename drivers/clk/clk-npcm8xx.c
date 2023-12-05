// SPDX-License-Identifier: GPL-2.0
/*
 * Nuvoton NPCM8xx Clock Generator
 * All the clocks are initialized by the bootloader, so this driver allows only
 * reading of current settings directly from the hardware.
 *
 * Copyright (C) 2020 Nuvoton Technologies
 * Author: Tomer Maimon <tomer.maimon@nuvoton.com>
 */

#define pr_fmt(fmt) "npcm8xx_clk: " fmt

#include <linux/bitfield.h>
#include <linux/clk-provider.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/regmap.h>
#include <linux/mfd/syscon.h>

#include <dt-bindings/clock/nuvoton,npcm845-clk.h>

/* npcm8xx clock registers*/
#define NPCM8XX_CLKSEL		0x04
#define NPCM8XX_CLKDIV1		0x08
#define NPCM8XX_CLKDIV2		0x2C
#define NPCM8XX_CLKDIV3		0x58
#define NPCM8XX_CLKDIV4		0x7C
#define NPCM8XX_PLLCON0		0x0C
#define NPCM8XX_PLLCON1		0x10
#define NPCM8XX_PLLCON2		0x54
#define NPCM8XX_PLLCONG		0x60
#define NPCM8XX_THRTL_CNT	0xC0

#define PLLCON_LOKI	BIT(31)
#define PLLCON_LOKS	BIT(30)
#define PLLCON_FBDV	GENMASK(27, 16)
#define PLLCON_OTDV2	GENMASK(15, 13)
#define PLLCON_PWDEN	BIT(12)
#define PLLCON_OTDV1	GENMASK(10, 8)
#define PLLCON_INDV	GENMASK(5, 0)

struct npcm8xx_clk {
	struct regmap	*clk_regmap;
	unsigned int	offset;
	const char	*name;
	const u32	*table;
	u32		mask;
	u8		shift;
	unsigned long	width;
	unsigned long	flags;
	struct clk_hw	hw;
};

#define to_npcm8xx_clk(_hw) container_of(_hw, struct npcm8xx_clk, hw)

struct npcm8xx_clk_pll_data {
	const char *name;
	struct clk_parent_data parent;
	unsigned int reg;
	unsigned long flags;
	struct clk_hw hw;
};

struct npcm8xx_clk_div_data {
	u32 reg;
	u8 shift;
	u8 width;
	const char *name;
	const struct clk_hw *parent_hw;
	unsigned long clk_divider_flags;
	unsigned long flags;
	int onecell_idx;
	struct clk_hw hw;
};

struct npcm8xx_clk_mux_data {
	u8 shift;
	u32 mask;
	const u32 *table;
	const char *name;
	const struct clk_parent_data *parent_data;
	u8 num_parents;
	unsigned long flags;
	struct clk_hw hw;
};

/* external clock definition */
#define NPCM8XX_CLK_S_REFCLK	"refclk"

/* pll definition */
#define NPCM8XX_CLK_S_PLL0	"pll0"
#define NPCM8XX_CLK_S_PLL1	"pll1"
#define NPCM8XX_CLK_S_PLL2	"pll2"
#define NPCM8XX_CLK_S_PLL_GFX	"pll_gfx"

/* early divider definition */
#define NPCM8XX_CLK_S_PLL2_DIV2		"pll2_div2"
#define NPCM8XX_CLK_S_PLL_GFX_DIV2	"pll_gfx_div2"
#define NPCM8XX_CLK_S_PLL1_DIV2		"pll1_div2"

/* mux definition */
#define NPCM8XX_CLK_S_CPU_MUX     "cpu_mux"
#define NPCM8XX_CLK_S_PIX_MUX     "gfx_pixel_mux"
#define NPCM8XX_CLK_S_SD_MUX      "sd_mux"
#define NPCM8XX_CLK_S_UART_MUX    "uart_mux"
#define NPCM8XX_CLK_S_SU_MUX      "serial_usb_mux"
#define NPCM8XX_CLK_S_MC_MUX      "mc_mux"
#define NPCM8XX_CLK_S_ADC_MUX     "adc_mux"
#define NPCM8XX_CLK_S_GFX_MUX     "gfx_mux"
#define NPCM8XX_CLK_S_CLKOUT_MUX  "clkout_mux"
#define NPCM8XX_CLK_S_GFXM_MUX    "gfxm_mux"
#define NPCM8XX_CLK_S_DVC_MUX     "dvc_mux"
#define NPCM8XX_CLK_S_RG_MUX	  "rg_mux"
#define NPCM8XX_CLK_S_RCP_MUX	  "rcp_mux"

/* div definition */
#define NPCM8XX_CLK_S_MC          "mc"
#define NPCM8XX_CLK_S_AXI         "axi"
#define NPCM8XX_CLK_S_AHB         "ahb"
#define NPCM8XX_CLK_S_SPI0        "spi0"
#define NPCM8XX_CLK_S_SPI1        "spi1"
#define NPCM8XX_CLK_S_SPI3        "spi3"
#define NPCM8XX_CLK_S_SPIX        "spix"
#define NPCM8XX_CLK_S_APB1        "apb1"
#define NPCM8XX_CLK_S_APB2        "apb2"
#define NPCM8XX_CLK_S_APB3        "apb3"
#define NPCM8XX_CLK_S_APB4        "apb4"
#define NPCM8XX_CLK_S_APB5        "apb5"
#define NPCM8XX_CLK_S_APB19       "apb19"
#define NPCM8XX_CLK_S_TOCK        "tock"
#define NPCM8XX_CLK_S_CLKOUT      "clkout"
#define NPCM8XX_CLK_S_PRE_ADC     "pre adc"
#define NPCM8XX_CLK_S_UART        "uart"
#define NPCM8XX_CLK_S_UART2       "uart2"
#define NPCM8XX_CLK_S_TIMER       "timer"
#define NPCM8XX_CLK_S_MMC         "mmc"
#define NPCM8XX_CLK_S_SDHC        "sdhc"
#define NPCM8XX_CLK_S_ADC         "adc"
#define NPCM8XX_CLK_S_GFX         "gfx0_gfx1_mem"
#define NPCM8XX_CLK_S_USBIF       "serial_usbif"
#define NPCM8XX_CLK_S_USB_HOST    "usb_host"
#define NPCM8XX_CLK_S_USB_BRIDGE  "usb_bridge"
#define NPCM8XX_CLK_S_PCI         "pci"
#define NPCM8XX_CLK_S_TH          "th"
#define NPCM8XX_CLK_S_ATB         "atb"
#define NPCM8XX_CLK_S_PRE_CLK     "pre_clk"
#define NPCM8XX_CLK_S_RG	  "rg"
#define NPCM8XX_CLK_S_RCP	  "rcp"

static struct clk_hw hw_pll1_div2, hw_pll2_div2, hw_gfx_div2, hw_pre_clk;
static struct npcm8xx_clk_pll_data npcm8xx_pll_clks[] = {
	{ NPCM8XX_CLK_S_PLL0, { .name = NPCM8XX_CLK_S_REFCLK }, NPCM8XX_PLLCON0, 0 },
	{ NPCM8XX_CLK_S_PLL1, { .name = NPCM8XX_CLK_S_REFCLK }, NPCM8XX_PLLCON1, 0 },
	{ NPCM8XX_CLK_S_PLL2, { .name = NPCM8XX_CLK_S_REFCLK }, NPCM8XX_PLLCON2, 0 },
	{ NPCM8XX_CLK_S_PLL_GFX, { .name = NPCM8XX_CLK_S_REFCLK }, NPCM8XX_PLLCONG, 0 },
};

static const u32 cpuck_mux_table[] = { 0, 1, 2, 7 };
static const struct clk_parent_data cpuck_mux_parents[] = {
	{ .hw = &npcm8xx_pll_clks[0].hw },
	{ .hw = &npcm8xx_pll_clks[1].hw },
	{ .index = 0 },
	{ .hw = &npcm8xx_pll_clks[2].hw }
};

static const u32 pixcksel_mux_table[] = { 0, 2 };
static const struct clk_parent_data pixcksel_mux_parents[] = {
	{ .hw = &npcm8xx_pll_clks[3].hw },
	{ .index = 0 }
};

static const u32 default_mux_table[] = { 0, 1, 2, 3 };
static const struct clk_parent_data default_mux_parents[] = {
	{ .hw = &npcm8xx_pll_clks[0].hw },
	{ .hw = &npcm8xx_pll_clks[1].hw },
	{ .index = 0 },
	{ .hw = &hw_pll2_div2 }
};

static const u32 sucksel_mux_table[] = { 2, 3 };
static const struct clk_parent_data sucksel_mux_parents[] = {
	{ .index = 0 },
	{ .hw = &hw_pll2_div2 }
};

static const u32 mccksel_mux_table[] = { 0, 2 };
static const struct clk_parent_data mccksel_mux_parents[] = {
	{ .hw = &hw_pll1_div2 },
	{ .index = 0 }
};

static const u32 clkoutsel_mux_table[] = { 0, 1, 2, 3, 4 };
static const struct clk_parent_data clkoutsel_mux_parents[] = {
	{ .hw = &npcm8xx_pll_clks[0].hw },
	{ .hw = &npcm8xx_pll_clks[1].hw },
	{ .index = 0 },
	{ .hw = &hw_gfx_div2 },
	{ .hw = &hw_pll2_div2 }
};

static const u32 gfxmsel_mux_table[] = { 2, 3 };
static const struct clk_parent_data gfxmsel_mux_parents[] = {
	{ .index = 0 },
	{ .hw = &npcm8xx_pll_clks[2].hw }
};

static const u32 dvcssel_mux_table[] = { 2, 3 };
static const struct clk_parent_data dvcssel_mux_parents[] = {
	{ .index = 0 },
	{ .hw = &npcm8xx_pll_clks[2].hw }
};

static const u32 default3_mux_table[] = { 0, 1, 2 };
static const struct clk_parent_data default3_mux_parents[] = {
	{ .hw = &npcm8xx_pll_clks[0].hw },
	{ .hw = &npcm8xx_pll_clks[1].hw },
	{ .index = 0 }
};

static struct npcm8xx_clk_mux_data npcm8xx_muxes[] = {
	{ 0, 7, cpuck_mux_table, NPCM8XX_CLK_S_CPU_MUX, cpuck_mux_parents,
		ARRAY_SIZE(cpuck_mux_parents), CLK_IS_CRITICAL },
	{ 4, 3, pixcksel_mux_table, NPCM8XX_CLK_S_PIX_MUX, pixcksel_mux_parents,
		ARRAY_SIZE(pixcksel_mux_parents), 0 },
	{ 6, 3, default_mux_table, NPCM8XX_CLK_S_SD_MUX, default_mux_parents,
		ARRAY_SIZE(default_mux_parents), 0 },
	{ 8, 3, default_mux_table, NPCM8XX_CLK_S_UART_MUX, default_mux_parents,
		ARRAY_SIZE(default_mux_parents), 0 },
	{ 10, 3, sucksel_mux_table, NPCM8XX_CLK_S_SU_MUX, sucksel_mux_parents,
		ARRAY_SIZE(sucksel_mux_parents), 0 },
	{ 12, 3, mccksel_mux_table, NPCM8XX_CLK_S_MC_MUX, mccksel_mux_parents,
		ARRAY_SIZE(mccksel_mux_parents), 0 },
	{ 14, 3, default_mux_table, NPCM8XX_CLK_S_ADC_MUX, default_mux_parents,
		ARRAY_SIZE(default_mux_parents), 0 },
	{ 16, 3, default_mux_table, NPCM8XX_CLK_S_GFX_MUX, default_mux_parents,
		ARRAY_SIZE(default_mux_parents), 0 },
	{ 18, 7, clkoutsel_mux_table, NPCM8XX_CLK_S_CLKOUT_MUX, clkoutsel_mux_parents,
		ARRAY_SIZE(clkoutsel_mux_parents), 0 },
	{ 21, 3, gfxmsel_mux_table, NPCM8XX_CLK_S_GFXM_MUX, gfxmsel_mux_parents,
		ARRAY_SIZE(gfxmsel_mux_parents), 0 },
	{ 23, 3, dvcssel_mux_table, NPCM8XX_CLK_S_DVC_MUX, dvcssel_mux_parents,
		ARRAY_SIZE(dvcssel_mux_parents), 0 },
	{ 25, 3, default3_mux_table, NPCM8XX_CLK_S_RG_MUX, default3_mux_parents,
		ARRAY_SIZE(default3_mux_parents), 0 },
	{ 27, 3, default3_mux_table, NPCM8XX_CLK_S_RCP_MUX, default3_mux_parents,
		ARRAY_SIZE(default3_mux_parents), 0 },
};

static struct npcm8xx_clk_div_data npcm8xx_pre_divs[] = {
	{ NPCM8XX_CLKDIV1, 21, 5, NPCM8XX_CLK_S_PRE_ADC, &npcm8xx_muxes[6].hw, CLK_DIVIDER_READ_ONLY, 0, -1 },
	{ NPCM8XX_CLKDIV1, 26, 2, NPCM8XX_CLK_S_AHB, &hw_pre_clk, CLK_DIVIDER_READ_ONLY, CLK_IS_CRITICAL, NPCM8XX_CLK_AHB },
};

/* configurable dividers: */
static struct npcm8xx_clk_div_data npcm8xx_divs[] = {
	{ NPCM8XX_CLKDIV1, 28, 3, NPCM8XX_CLK_S_ADC, &npcm8xx_pre_divs[0].hw, CLK_DIVIDER_READ_ONLY | CLK_DIVIDER_POWER_OF_TWO, 0, NPCM8XX_CLK_ADC },
	{ NPCM8XX_CLKDIV1, 16, 5, NPCM8XX_CLK_S_UART, &npcm8xx_muxes[3].hw, 0, 0, NPCM8XX_CLK_UART },
	{ NPCM8XX_CLKDIV1, 11, 5, NPCM8XX_CLK_S_MMC, &npcm8xx_muxes[2].hw, CLK_DIVIDER_READ_ONLY, 0, NPCM8XX_CLK_MMC },
	{ NPCM8XX_CLKDIV1, 6, 5, NPCM8XX_CLK_S_SPI3, &npcm8xx_pre_divs[1].hw, 0, 0, NPCM8XX_CLK_SPI3 },
	{ NPCM8XX_CLKDIV1, 2, 4, NPCM8XX_CLK_S_PCI, &npcm8xx_muxes[7].hw, CLK_DIVIDER_READ_ONLY, 0, NPCM8XX_CLK_PCI },

	{ NPCM8XX_CLKDIV2, 30, 2, NPCM8XX_CLK_S_APB4, &npcm8xx_pre_divs[1].hw, CLK_DIVIDER_READ_ONLY | CLK_DIVIDER_POWER_OF_TWO, 0, NPCM8XX_CLK_APB4 },
	{ NPCM8XX_CLKDIV2, 28, 2, NPCM8XX_CLK_S_APB3, &npcm8xx_pre_divs[1].hw, CLK_DIVIDER_READ_ONLY | CLK_DIVIDER_POWER_OF_TWO, 0, NPCM8XX_CLK_APB3 },
	{ NPCM8XX_CLKDIV2, 26, 2, NPCM8XX_CLK_S_APB2, &npcm8xx_pre_divs[1].hw, CLK_DIVIDER_READ_ONLY | CLK_DIVIDER_POWER_OF_TWO, 0, NPCM8XX_CLK_APB2 },
	{ NPCM8XX_CLKDIV2, 24, 2, NPCM8XX_CLK_S_APB1, &npcm8xx_pre_divs[1].hw, CLK_DIVIDER_READ_ONLY | CLK_DIVIDER_POWER_OF_TWO, 0, NPCM8XX_CLK_APB1 },
	{ NPCM8XX_CLKDIV2, 22, 2, NPCM8XX_CLK_S_APB5, &npcm8xx_pre_divs[1].hw, CLK_DIVIDER_READ_ONLY | CLK_DIVIDER_POWER_OF_TWO, 0, NPCM8XX_CLK_APB5 },
	{ NPCM8XX_CLKDIV2, 16, 5, NPCM8XX_CLK_S_CLKOUT, &npcm8xx_muxes[8].hw, CLK_DIVIDER_READ_ONLY, 0, NPCM8XX_CLK_CLKOUT },
	{ NPCM8XX_CLKDIV2, 13, 3, NPCM8XX_CLK_S_GFX, &npcm8xx_muxes[7].hw, CLK_DIVIDER_READ_ONLY, 0, NPCM8XX_CLK_GFX },
	{ NPCM8XX_CLKDIV2, 8, 5, NPCM8XX_CLK_S_USB_BRIDGE, &npcm8xx_muxes[4].hw, CLK_DIVIDER_READ_ONLY, 0, NPCM8XX_CLK_SU },
	{ NPCM8XX_CLKDIV2, 4, 4, NPCM8XX_CLK_S_USB_HOST, &npcm8xx_muxes[4].hw, CLK_DIVIDER_READ_ONLY, 0, NPCM8XX_CLK_SU48 },
	{ NPCM8XX_CLKDIV2, 0, 4, NPCM8XX_CLK_S_SDHC, &npcm8xx_muxes[2].hw, CLK_DIVIDER_READ_ONLY, 0, NPCM8XX_CLK_SDHC },

	{ NPCM8XX_CLKDIV3, 16, 8, NPCM8XX_CLK_S_SPI1, &npcm8xx_pre_divs[1].hw, CLK_DIVIDER_READ_ONLY, 0, NPCM8XX_CLK_SPI1 },
	{ NPCM8XX_CLKDIV3, 11, 5, NPCM8XX_CLK_S_UART2, &npcm8xx_muxes[3].hw, CLK_DIVIDER_READ_ONLY, 0, NPCM8XX_CLK_UART2 },
	{ NPCM8XX_CLKDIV3, 6, 5, NPCM8XX_CLK_S_SPI0, &npcm8xx_pre_divs[1].hw, CLK_DIVIDER_READ_ONLY, 0, NPCM8XX_CLK_SPI0 },
	{ NPCM8XX_CLKDIV3, 1, 5, NPCM8XX_CLK_S_SPIX, &npcm8xx_pre_divs[1].hw, CLK_DIVIDER_READ_ONLY, 0, NPCM8XX_CLK_SPIX },

	{ NPCM8XX_CLKDIV4, 28, 4, NPCM8XX_CLK_S_RG, &npcm8xx_muxes[11].hw, CLK_DIVIDER_READ_ONLY, 0, NPCM8XX_CLK_RG },
	{ NPCM8XX_CLKDIV4, 12, 4, NPCM8XX_CLK_S_RCP, &npcm8xx_muxes[12].hw, CLK_DIVIDER_READ_ONLY, 0, NPCM8XX_CLK_RCP },

	{ NPCM8XX_THRTL_CNT, 0, 2, NPCM8XX_CLK_S_TH, &npcm8xx_muxes[0].hw, CLK_DIVIDER_READ_ONLY | CLK_DIVIDER_POWER_OF_TWO, 0, NPCM8XX_CLK_TH },
};

static struct clk_hw *
npcm8xx_clk_register(struct device *dev, const char *name,
		     struct regmap *clk_regmap, unsigned int offset,
		     unsigned long flags, const struct clk_ops *npcm8xx_clk_ops,
		     const struct clk_parent_data *parent_data,
		     const struct clk_hw *parent_hw, u8 num_parents,
		     u8 shift, u32 mask, unsigned long width,
		     const u32 *table, unsigned long clk_flags)
{
	struct npcm8xx_clk *clk;
	struct clk_init_data init = {};
	int ret;

	clk = devm_kzalloc(dev, sizeof(*clk), GFP_KERNEL);
	if (!clk)
		return ERR_PTR(-ENOMEM);

	init.name = name;
	init.ops = npcm8xx_clk_ops;
	init.parent_data = parent_data;
	init.parent_hws = parent_hw ? &parent_hw : NULL;
	init.num_parents = num_parents;
	init.flags = flags;

	clk->clk_regmap = clk_regmap;
	clk->hw.init = &init;
	clk->offset = offset;
	clk->shift = shift;
	clk->mask = mask;
	clk->width = width;
	clk->table = table;
	clk->flags = clk_flags;

	ret = devm_clk_hw_register(dev, &clk->hw);
	if (ret)
		return ERR_PTR(ret);

	return &clk->hw;
}

static unsigned long npcm8xx_clk_pll_recalc_rate(struct clk_hw *hw,
						 unsigned long parent_rate)
{
	struct npcm8xx_clk *pll = to_npcm8xx_clk(hw);
	unsigned long fbdv, indv, otdv1, otdv2;
	unsigned int val;
	u64 ret;

	if (parent_rate == 0) {
		pr_debug("%s: parent rate is zero\n", __func__);
		return 0;
	}

	regmap_read(pll->clk_regmap, pll->offset, &val);

	indv = FIELD_GET(PLLCON_INDV, val);
	fbdv = FIELD_GET(PLLCON_FBDV, val);
	otdv1 = FIELD_GET(PLLCON_OTDV1, val);
	otdv2 = FIELD_GET(PLLCON_OTDV2, val);

	ret = (u64)parent_rate * fbdv;
	do_div(ret, indv * otdv1 * otdv2);

	return ret;
}

static const struct clk_ops npcm8xx_clk_pll_ops = {
	.recalc_rate = npcm8xx_clk_pll_recalc_rate,
};

static u8 npcm8xx_clk_mux_get_parent(struct clk_hw *hw)
{
	struct npcm8xx_clk *mux = to_npcm8xx_clk(hw);
	u32 val;

	regmap_read(mux->clk_regmap, mux->offset, &val);
	val = val >> mux->shift;
	val &= mux->mask;

	return clk_mux_val_to_index(hw, mux->table, mux->flags, val);
}

static const struct clk_ops npcm8xx_clk_mux_ops = {
	.get_parent = npcm8xx_clk_mux_get_parent,
};

static unsigned long npcm8xx_clk_div_get_parent(struct clk_hw *hw,
						unsigned long parent_rate)
{
	struct npcm8xx_clk *div = to_npcm8xx_clk(hw);
	unsigned int val;

	regmap_read(div->clk_regmap, div->offset, &val);
	val = val >> div->shift;
	val &= clk_div_mask(div->width);

	return divider_recalc_rate(hw, parent_rate, val, NULL, div->flags,
				   div->width);
}

static const struct clk_ops npcm8xx_clk_div_ops = {
	.recalc_rate = npcm8xx_clk_div_get_parent,
};

static int npcm8xx_clk_probe(struct platform_device *pdev)
{
	struct clk_hw_onecell_data *npcm8xx_clk_data;
	struct device_node *np = pdev->dev.of_node;
	struct device *dev = &pdev->dev;
	struct regmap *clk_regmap;
	struct clk_hw *hw;
	unsigned int i;

	npcm8xx_clk_data = devm_kzalloc(dev, struct_size(npcm8xx_clk_data, hws,
							 NPCM8XX_NUM_CLOCKS),
					GFP_KERNEL);
	if (!npcm8xx_clk_data)
		return -ENOMEM;

	clk_regmap = syscon_regmap_lookup_by_phandle(np, "nuvoton,sysclk");
	if (IS_ERR(clk_regmap)) {
		dev_err(&pdev->dev, "Failed to find nuvoton,sysclk\n");
		return PTR_ERR(clk_regmap);
	}

	npcm8xx_clk_data->num = NPCM8XX_NUM_CLOCKS;

	for (i = 0; i < NPCM8XX_NUM_CLOCKS; i++)
		npcm8xx_clk_data->hws[i] = ERR_PTR(-EPROBE_DEFER);

	/* Register plls */
	for (i = 0; i < ARRAY_SIZE(npcm8xx_pll_clks); i++) {
		struct npcm8xx_clk_pll_data *pll_clk = &npcm8xx_pll_clks[i];

		hw = npcm8xx_clk_register(dev, pll_clk->name, clk_regmap,
					  pll_clk->reg, pll_clk->flags,
					  &npcm8xx_clk_pll_ops, &pll_clk->parent,
					  NULL, 1, 0, 0, 0, NULL, 0);
		if (IS_ERR(hw))
			return dev_err_probe(dev, PTR_ERR(hw), "Can't register pll\n");
		pll_clk->hw = *hw;
	}

	/* Register fixed dividers */
	hw = devm_clk_hw_register_fixed_factor(dev, NPCM8XX_CLK_S_PLL1_DIV2,
					       NPCM8XX_CLK_S_PLL1, 0, 1, 2);
	if (IS_ERR(hw))
		return dev_err_probe(dev, PTR_ERR(hw), "Can't register fixed div\n");
	hw_pll1_div2 = *hw;

	hw = devm_clk_hw_register_fixed_factor(dev, NPCM8XX_CLK_S_PLL2_DIV2,
					       NPCM8XX_CLK_S_PLL2, 0, 1, 2);
	if (IS_ERR(hw))
		return dev_err_probe(dev, PTR_ERR(hw), "Can't register pll2 div2\n");
	hw_pll2_div2 = *hw;

	hw = devm_clk_hw_register_fixed_factor(dev, NPCM8XX_CLK_S_PLL_GFX_DIV2,
					       NPCM8XX_CLK_S_PLL_GFX, 0, 1, 2);
	if (IS_ERR(hw))
		return dev_err_probe(dev, PTR_ERR(hw), "Can't register gfx div2\n");
	hw_gfx_div2 = *hw;

	/* Register muxes */
	for (i = 0; i < ARRAY_SIZE(npcm8xx_muxes); i++) {
		struct npcm8xx_clk_mux_data *mux_data = &npcm8xx_muxes[i];

		hw = npcm8xx_clk_register(dev, mux_data->name, clk_regmap,
					  NPCM8XX_CLKSEL, mux_data->flags,
					  &npcm8xx_clk_mux_ops,
					  mux_data->parent_data, NULL,
					  mux_data->num_parents,
					  mux_data->shift, mux_data->mask, 0,
					  mux_data->table, 0);
		if (IS_ERR(hw))
			return dev_err_probe(dev, PTR_ERR(hw), "Can't register mux\n");
		mux_data->hw = *hw;
	}

	hw = devm_clk_hw_register_fixed_factor(dev, NPCM8XX_CLK_S_PRE_CLK,
					       NPCM8XX_CLK_S_CPU_MUX, 0, 1, 2);
	if (IS_ERR(hw))
		return dev_err_probe(dev, PTR_ERR(hw), "Can't register pre clk div2\n");
	hw_pre_clk = *hw;

	hw = devm_clk_hw_register_fixed_factor(dev, NPCM8XX_CLK_S_AXI,
					       NPCM8XX_CLK_S_TH, 0, 1, 2);
	if (IS_ERR(hw))
		return dev_err_probe(dev, PTR_ERR(hw), "Can't register axi div2\n");
	npcm8xx_clk_data->hws[NPCM8XX_CLK_AXI] = hw;

	hw = devm_clk_hw_register_fixed_factor(dev, NPCM8XX_CLK_S_ATB,
					       NPCM8XX_CLK_S_AXI, 0, 1, 2);
	if (IS_ERR(hw))
		return dev_err_probe(dev, PTR_ERR(hw), "Can't register atb div2\n");
	npcm8xx_clk_data->hws[NPCM8XX_CLK_ATB] = hw;

	/* Register clock pre dividers specified in npcm8xx_divs */
	for (i = 0; i < ARRAY_SIZE(npcm8xx_pre_divs); i++) {
		struct npcm8xx_clk_div_data *div_data = &npcm8xx_pre_divs[i];

		hw = npcm8xx_clk_register(dev, div_data->name, clk_regmap,
					  div_data->reg, div_data->flags,
					  &npcm8xx_clk_div_ops, NULL,
					  div_data->parent_hw, 1,
					  div_data->shift, 0, div_data->width,
					  NULL, div_data->clk_divider_flags);
		if (IS_ERR(hw))
			return dev_err_probe(dev, PTR_ERR(hw), "Can't register pre div table\n");
		div_data->hw = *hw;

		if (div_data->onecell_idx >= 0)
			npcm8xx_clk_data->hws[div_data->onecell_idx] = hw;
	}

	/* Register clock dividers specified in npcm8xx_divs */
	for (i = 0; i < ARRAY_SIZE(npcm8xx_divs); i++) {
		struct npcm8xx_clk_div_data *div_data = &npcm8xx_divs[i];

		hw = npcm8xx_clk_register(dev, div_data->name, clk_regmap,
					  div_data->reg, div_data->flags,
					  &npcm8xx_clk_div_ops, NULL,
					  div_data->parent_hw, 1,
					  div_data->shift, 0, div_data->width,
					  NULL, div_data->clk_divider_flags);
		if (IS_ERR(hw))
			return dev_err_probe(dev, PTR_ERR(hw), "Can't register div table\n");

		if (div_data->onecell_idx >= 0)
			npcm8xx_clk_data->hws[div_data->onecell_idx] = hw;
	}

	return devm_of_clk_add_hw_provider(dev, of_clk_hw_onecell_get,
					   npcm8xx_clk_data);
}

static const struct of_device_id npcm8xx_clk_dt_ids[] = {
	{ .compatible = "nuvoton,npcm845-clk", },
	{ }
};
MODULE_DEVICE_TABLE(of, npcm8xx_clk_dt_ids);

static struct platform_driver npcm8xx_clk_driver = {
	.probe  = npcm8xx_clk_probe,
	.driver = {
		.name = "npcm8xx_clk",
		.of_match_table = npcm8xx_clk_dt_ids,
	},
};

static int __init npcm8xx_clk_driver_init(void)
{
	return platform_driver_register(&npcm8xx_clk_driver);
}
arch_initcall(npcm8xx_clk_driver_init);

static void __exit npcm8xx_clk_exit(void)
{
	platform_driver_unregister(&npcm8xx_clk_driver);
}
module_exit(npcm8xx_clk_exit);

MODULE_DESCRIPTION("Clock driver for Nuvoton NPCM8XX BMC SoC");
MODULE_AUTHOR("Tomer Maimon <tomer.maimon@nuvoton.com>");
MODULE_LICENSE("GPL v2");

