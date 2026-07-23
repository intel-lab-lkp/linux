// SPDX-License-Identifier: GPL-2.0-only
/*
 * Clock driver for the STM32H7RS Reset and Clock Controller.
 *
 * The first-stage firmware configures the oscillators and PLLs before Linux
 * starts executing from external NOR.  The driver reconstructs that running
 * clock tree from RCC registers and gives Linux ownership of peripheral clock
 * gates without disturbing the XIP-critical PLL configuration.
 */

#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/reset-controller.h>
#include <linux/reset/reset-simple.h>
#include <linux/spinlock.h>

#include <dt-bindings/clock/stm32h7rs-clks.h>

#define RCC_CR			0x000
#define RCC_CFGR		0x010
#define RCC_CDCFGR		0x018
#define RCC_BMCFGR		0x01c
#define RCC_APBCFGR		0x020
#define RCC_PLLCKSELR		0x028
#define RCC_PLLCFGR		0x02c
#define RCC_PLL1DIVR1		0x030
#define RCC_PLL1FRACR		0x034
#define RCC_PLL2DIVR1		0x038
#define RCC_PLL2FRACR		0x03c
#define RCC_PLL3DIVR1		0x040
#define RCC_PLL3FRACR		0x044
#define RCC_CCIPR1		0x04c
#define RCC_CCIPR2		0x050
#define RCC_PLL2DIVR2		0x0c4
#define RCC_AHB5ENR		0x134
#define RCC_AHB4ENR		0x140
#define RCC_APB1ENR1		0x148

#define RCC_CR_HSIDIV_SHIFT	3
#define RCC_CFGR_SWS_SHIFT	3
#define RCC_CFGR_TIMPRE		BIT(15)
#define RCC_PLLCFGR_FRACEN(_pll)	BIT(((_pll) - 1) * 11)
#define RCC_PLL_ON(_pll)	BIT(24 + (((_pll) - 1) * 2))

#define RCC_CDCFGR_CPRE_SHIFT	0
#define RCC_BMCFGR_BMPRE_SHIFT	0
#define RCC_APBCFGR_PPRE1_SHIFT	0
#define RCC_APBCFGR_PPRE2_SHIFT	4
#define RCC_APBCFGR_PPRE4_SHIFT	8
#define RCC_APBCFGR_PPRE5_SHIFT	12

#define RCC_CCIPR1_SDMMC12SEL	BIT(2)
#define RCC_CCIPR2_UARTSEL_MASK	GENMASK(2, 0)

#define HSI_RATE		64000000UL
#define CSI_RATE		4000000UL

static DEFINE_SPINLOCK(stm32h7rs_rcc_lock);

static const u16 pll_divr_offset[] = {
	RCC_PLL1DIVR1, RCC_PLL2DIVR1, RCC_PLL3DIVR1,
};

static const u16 pll_fracr_offset[] = {
	RCC_PLL1FRACR, RCC_PLL2FRACR, RCC_PLL3FRACR,
};

static const u8 pll_m_shift[] = { 4, 12, 20 };

enum stm32h7rs_pll_output {
	PLL_P,
	PLL_Q,
	PLL_R,
	PLL_S,
	PLL_T,
};

static unsigned long stm32h7rs_hsi_rate(void __iomem *base)
{
	u32 div = (readl_relaxed(base + RCC_CR) >> RCC_CR_HSIDIV_SHIFT) & 0x3;

	return HSI_RATE >> div;
}

static unsigned long stm32h7rs_pll_rate(void __iomem *base, unsigned int pll,
					unsigned int output,
					unsigned long hse_rate)
{
	static const u8 output_shift[] = { 9, 16, 24 };
	u32 divr1, frac = 0, m, n, out_div, source;
	unsigned long parent_rate;
	u64 numerator;

	if (pll < 1 || pll > ARRAY_SIZE(pll_divr_offset) || output > PLL_T)
		return 0;

	if (output > PLL_R && pll != 2)
		return 0;

	if (!(readl_relaxed(base + RCC_CR) & RCC_PLL_ON(pll)))
		return 0;

	source = readl_relaxed(base + RCC_PLLCKSELR) & GENMASK(1, 0);
	switch (source) {
	case 0:
		parent_rate = stm32h7rs_hsi_rate(base);
		break;
	case 1:
		parent_rate = CSI_RATE;
		break;
	case 2:
		parent_rate = hse_rate;
		break;
	default:
		return 0;
	}

	m = (readl_relaxed(base + RCC_PLLCKSELR) >> pll_m_shift[pll - 1]) & 0x3f;
	if (!m || !parent_rate)
		return 0;

	divr1 = readl_relaxed(base + pll_divr_offset[pll - 1]);
	n = (divr1 & GENMASK(8, 0)) + 1;
	if (readl_relaxed(base + RCC_PLLCFGR) & RCC_PLLCFGR_FRACEN(pll))
		frac = (readl_relaxed(base + pll_fracr_offset[pll - 1]) >> 3) &
			GENMASK(12, 0);

	if (output <= PLL_R) {
		out_div = ((divr1 >> output_shift[output]) &
			   GENMASK(6, 0)) + 1;
	} else {
		u32 divr2 = readl_relaxed(base + RCC_PLL2DIVR2);
		u8 shift = output == PLL_S ? 0 : 8;

		out_div = ((divr2 >> shift) & GENMASK(2, 0)) + 1;
	}

	numerator = (u64)parent_rate * (n * 8192ULL + frac);

	return div64_u64(numerator, (u64)m * 8192ULL * out_div);
}

static unsigned long stm32h7rs_ahb_rate(unsigned long parent_rate, u32 val)
{
	static const u16 div_table[] = { 2, 4, 8, 16, 64, 128, 256, 512 };

	if (val < 8)
		return parent_rate;

	return parent_rate / div_table[val - 8];
}

static unsigned long stm32h7rs_sys_rate(void __iomem *base,
					unsigned long hse_rate)
{
	unsigned long rate;
	u32 cpre, source;

	source = (readl_relaxed(base + RCC_CFGR) >> RCC_CFGR_SWS_SHIFT) & 0x7;
	switch (source) {
	case 0:
		rate = stm32h7rs_hsi_rate(base);
		break;
	case 1:
		rate = CSI_RATE;
		break;
	case 2:
		rate = hse_rate;
		break;
	case 3:
		rate = stm32h7rs_pll_rate(base, 1, PLL_P, hse_rate);
		break;
	default:
		return 0;
	}

	cpre = (readl_relaxed(base + RCC_CDCFGR) >> RCC_CDCFGR_CPRE_SHIFT) & 0xf;
	return stm32h7rs_ahb_rate(rate, cpre);
}

static unsigned long stm32h7rs_apb_rate(void __iomem *base,
					unsigned long hclk_rate, u8 shift)
{
	u32 prescaler = (readl_relaxed(base + RCC_APBCFGR) >> shift) & 0x7;

	if (prescaler < 4)
		return hclk_rate;

	return hclk_rate >> (prescaler - 3);
}

static unsigned long stm32h7rs_timer_rate(void __iomem *base,
					  unsigned long hclk_rate,
					  unsigned long pclk_rate)
{
	u32 prescaler = (readl_relaxed(base + RCC_APBCFGR) >>
			 RCC_APBCFGR_PPRE1_SHIFT) & 0x7;

	if (prescaler < 4)
		return pclk_rate;

	if (!(readl_relaxed(base + RCC_CFGR) & RCC_CFGR_TIMPRE))
		return pclk_rate * 2;

	return prescaler < 6 ? hclk_rate : pclk_rate * 4;
}

static unsigned long stm32h7rs_uart4_rate(void __iomem *base,
					  unsigned long hse_rate,
					  unsigned long lse_rate,
					  unsigned long pclk1_rate)
{
	u32 source = readl_relaxed(base + RCC_CCIPR2) & RCC_CCIPR2_UARTSEL_MASK;

	switch (source) {
	case 0:
		return pclk1_rate;
	case 1:
		return stm32h7rs_pll_rate(base, 2, PLL_Q, hse_rate);
	case 2:
		return stm32h7rs_pll_rate(base, 3, PLL_Q, hse_rate);
	case 3:
		return stm32h7rs_hsi_rate(base);
	case 4:
		return CSI_RATE;
	case 5:
		return lse_rate;
	default:
		return 0;
	}
}

static unsigned long stm32h7rs_sdmmc1_rate(void __iomem *base,
					   unsigned long hse_rate)
{
	if (readl_relaxed(base + RCC_CCIPR1) & RCC_CCIPR1_SDMMC12SEL)
		return stm32h7rs_pll_rate(base, 2, PLL_T, hse_rate);

	return stm32h7rs_pll_rate(base, 2, PLL_S, hse_rate);
}

static int h7rs_register_fixed(struct device *dev,
			       struct clk_hw_onecell_data *data,
			       unsigned int id, const char *name,
			       unsigned long rate)
{
	struct clk_hw *hw;

	hw = devm_clk_hw_register_fixed_rate(dev, name, NULL, CLK_IS_CRITICAL,
					     rate);
	if (IS_ERR(hw))
		return PTR_ERR(hw);

	data->hws[id] = hw;

	return 0;
}

enum stm32h7rs_parent {
	STM32H7RS_PARENT_HCLK,
	STM32H7RS_PARENT_TIM5,
	STM32H7RS_PARENT_UART4,
	STM32H7RS_PARENT_SDMMC1,
	STM32H7RS_PARENT_COUNT,
};

struct stm32h7rs_gate {
	const char *name;
	u16 offset;
	u8 id;
	u8 parent;
	u8 bit_idx;
};

static const struct stm32h7rs_gate stm32h7rs_gates[] = {
	{ "tim5", RCC_APB1ENR1, STM32H7RS_TIM5_CK,
	  STM32H7RS_PARENT_TIM5, 3 },
	{ "uart4", RCC_APB1ENR1, STM32H7RS_UART4_CK,
	  STM32H7RS_PARENT_UART4, 19 },
	{ "gpioa", RCC_AHB4ENR, STM32H7RS_GPIOA_CK,
	  STM32H7RS_PARENT_HCLK, 0 },
	{ "gpiob", RCC_AHB4ENR, STM32H7RS_GPIOB_CK,
	  STM32H7RS_PARENT_HCLK, 1 },
	{ "gpioc", RCC_AHB4ENR, STM32H7RS_GPIOC_CK,
	  STM32H7RS_PARENT_HCLK, 2 },
	{ "gpiod", RCC_AHB4ENR, STM32H7RS_GPIOD_CK,
	  STM32H7RS_PARENT_HCLK, 3 },
	{ "gpioe", RCC_AHB4ENR, STM32H7RS_GPIOE_CK,
	  STM32H7RS_PARENT_HCLK, 4 },
	{ "gpiof", RCC_AHB4ENR, STM32H7RS_GPIOF_CK,
	  STM32H7RS_PARENT_HCLK, 5 },
	{ "gpiog", RCC_AHB4ENR, STM32H7RS_GPIOG_CK,
	  STM32H7RS_PARENT_HCLK, 6 },
	{ "gpioh", RCC_AHB4ENR, STM32H7RS_GPIOH_CK,
	  STM32H7RS_PARENT_HCLK, 7 },
	{ "gpiom", RCC_AHB4ENR, STM32H7RS_GPIOM_CK,
	  STM32H7RS_PARENT_HCLK, 12 },
	{ "gpion", RCC_AHB4ENR, STM32H7RS_GPION_CK,
	  STM32H7RS_PARENT_HCLK, 13 },
	{ "gpioo", RCC_AHB4ENR, STM32H7RS_GPIOO_CK,
	  STM32H7RS_PARENT_HCLK, 14 },
	{ "gpiop", RCC_AHB4ENR, STM32H7RS_GPIOP_CK,
	  STM32H7RS_PARENT_HCLK, 15 },
	{ "sdmmc1", RCC_AHB5ENR, STM32H7RS_SDMMC1_CK,
	  STM32H7RS_PARENT_SDMMC1, 8 },
};

static int h7rs_register_gate(struct device *dev,
			      struct clk_hw_onecell_data *data,
			      const struct stm32h7rs_gate *gate,
			      const struct clk_hw *parent_hw,
			      void __iomem *base)
{
	const struct clk_parent_data parent_data = { .hw = parent_hw };
	struct clk_hw *hw;

	hw = devm_clk_hw_register_gate_parent_data(dev, gate->name,
						   &parent_data,
						   CLK_IGNORE_UNUSED,
						   base + gate->offset,
						   gate->bit_idx, 0,
						   &stm32h7rs_rcc_lock);
	if (IS_ERR(hw))
		return PTR_ERR(hw);

	data->hws[gate->id] = hw;

	return 0;
}

static int stm32h7rs_rcc_probe(struct platform_device *pdev)
{
	struct clk_hw *parent_hws[STM32H7RS_PARENT_COUNT];
	struct clk_hw_onecell_data *clk_data;
	struct reset_simple_data *reset;
	struct device *dev = &pdev->dev;
	unsigned long sdmmc1_rate;
	unsigned long uart4_rate;
	unsigned long timer_rate;
	unsigned long pclk5_rate;
	unsigned long pclk4_rate;
	unsigned long pclk2_rate;
	unsigned long pclk1_rate;
	unsigned long hclk_rate;
	unsigned long sys_rate;
	unsigned long lse_rate;
	unsigned long hse_rate;
	resource_size_t size;
	struct clk *input;
	void __iomem *base;
	unsigned int i;
	u32 bmpre;
	int ret;

	base = devm_of_iomap(dev, dev->of_node, 0, &size);
	if (IS_ERR(base))
		return dev_err_probe(dev, PTR_ERR(base),
				     "unable to map RCC registers\n");

	input = devm_clk_get(dev, "hse");
	if (IS_ERR(input))
		return dev_err_probe(dev, PTR_ERR(input),
				     "unable to get HSE input clock\n");
	hse_rate = clk_get_rate(input);

	input = devm_clk_get(dev, "lse");
	if (IS_ERR(input))
		return dev_err_probe(dev, PTR_ERR(input),
				     "unable to get LSE input clock\n");
	lse_rate = clk_get_rate(input);

	reset = devm_kzalloc(dev, sizeof(*reset), GFP_KERNEL);
	if (!reset)
		return -ENOMEM;

	spin_lock_init(&reset->lock);
	reset->membase = base;
	reset->rcdev.owner = THIS_MODULE;
	reset->rcdev.nr_resets = size * BITS_PER_BYTE;
	reset->rcdev.ops = &reset_simple_ops;
	reset->rcdev.of_node = dev->of_node;

	ret = devm_reset_controller_register(dev, &reset->rcdev);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to register reset controller\n");

	clk_data = devm_kzalloc(dev,
				struct_size(clk_data, hws, STM32H7RS_MAX_CLKS),
				GFP_KERNEL);
	if (!clk_data)
		return -ENOMEM;

	clk_data->num = STM32H7RS_MAX_CLKS;
	for (i = 0; i < clk_data->num; i++)
		clk_data->hws[i] = ERR_PTR(-ENOENT);

	sys_rate = stm32h7rs_sys_rate(base, hse_rate);
	bmpre = (readl_relaxed(base + RCC_BMCFGR) >>
		  RCC_BMCFGR_BMPRE_SHIFT) & 0xf;
	hclk_rate = stm32h7rs_ahb_rate(sys_rate, bmpre);
	pclk1_rate = stm32h7rs_apb_rate(base, hclk_rate,
					RCC_APBCFGR_PPRE1_SHIFT);
	pclk2_rate = stm32h7rs_apb_rate(base, hclk_rate,
					RCC_APBCFGR_PPRE2_SHIFT);
	pclk4_rate = stm32h7rs_apb_rate(base, hclk_rate,
					RCC_APBCFGR_PPRE4_SHIFT);
	pclk5_rate = stm32h7rs_apb_rate(base, hclk_rate,
					RCC_APBCFGR_PPRE5_SHIFT);
	timer_rate = stm32h7rs_timer_rate(base, hclk_rate, pclk1_rate);
	uart4_rate = stm32h7rs_uart4_rate(base, hse_rate, lse_rate, pclk1_rate);
	sdmmc1_rate = stm32h7rs_sdmmc1_rate(base, hse_rate);

	if (!sys_rate || !hclk_rate || !pclk1_rate || !pclk2_rate ||
	    !pclk4_rate || !pclk5_rate || !timer_rate || !uart4_rate ||
	    !sdmmc1_rate)
		return dev_err_probe(dev, -EINVAL,
				     "invalid boot clock configuration\n");

	ret = h7rs_register_fixed(dev, clk_data, STM32H7RS_SYS_CK,
				  "h7rs_sys_ck", sys_rate);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to register system clock\n");

	ret = h7rs_register_fixed(dev, clk_data, STM32H7RS_HCLK,
				  "h7rs_hclk", hclk_rate);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to register HCLK\n");

	ret = h7rs_register_fixed(dev, clk_data, STM32H7RS_PCLK1,
				  "h7rs_pclk1", pclk1_rate);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to register PCLK1\n");

	ret = h7rs_register_fixed(dev, clk_data, STM32H7RS_PCLK2,
				  "h7rs_pclk2", pclk2_rate);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to register PCLK2\n");

	ret = h7rs_register_fixed(dev, clk_data, STM32H7RS_PCLK4,
				  "h7rs_pclk4", pclk4_rate);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to register PCLK4\n");

	ret = h7rs_register_fixed(dev, clk_data, STM32H7RS_PCLK5,
				  "h7rs_pclk5", pclk5_rate);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to register PCLK5\n");

	parent_hws[STM32H7RS_PARENT_HCLK] = clk_data->hws[STM32H7RS_HCLK];
	parent_hws[STM32H7RS_PARENT_TIM5] =
		devm_clk_hw_register_fixed_rate(dev, "tim5_ker", NULL,
						CLK_IS_CRITICAL, timer_rate);
	parent_hws[STM32H7RS_PARENT_UART4] =
		devm_clk_hw_register_fixed_rate(dev, "uart4_ker", NULL,
						CLK_IS_CRITICAL, uart4_rate);
	parent_hws[STM32H7RS_PARENT_SDMMC1] =
		devm_clk_hw_register_fixed_rate(dev, "sdmmc1_ker", NULL,
						CLK_IS_CRITICAL, sdmmc1_rate);

	for (i = STM32H7RS_PARENT_TIM5; i < STM32H7RS_PARENT_COUNT; i++) {
		if (IS_ERR(parent_hws[i]))
			return dev_err_probe(dev, PTR_ERR(parent_hws[i]),
					     "failed to register parent clock %u\n",
					     i);
	}

	for (i = 0; i < ARRAY_SIZE(stm32h7rs_gates); i++) {
		const struct stm32h7rs_gate *gate = &stm32h7rs_gates[i];

		ret = h7rs_register_gate(dev, clk_data, gate,
					 parent_hws[gate->parent], base);
		if (ret)
			return dev_err_probe(dev, ret,
					     "failed to register %s clock\n",
					     gate->name);
	}

	ret = devm_of_clk_add_hw_provider(dev, of_clk_hw_onecell_get, clk_data);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to add clock provider\n");

	dev_info(dev,
		 "sys=%lu hclk=%lu pclk1=%lu tim5=%lu uart4=%lu sdmmc1=%lu\n",
		 sys_rate, hclk_rate, pclk1_rate, timer_rate, uart4_rate,
		 sdmmc1_rate);

	return 0;
}

static const struct of_device_id stm32h7rs_rcc_match[] = {
	{ .compatible = "st,stm32h7rs-rcc" },
	{ }
};
MODULE_DEVICE_TABLE(of, stm32h7rs_rcc_match);

static struct platform_driver stm32h7rs_rcc_driver = {
	.probe = stm32h7rs_rcc_probe,
	.driver = {
		.name = "stm32h7rs-rcc",
		.of_match_table = stm32h7rs_rcc_match,
	},
};
builtin_platform_driver(stm32h7rs_rcc_driver);

MODULE_DESCRIPTION("STM32H7RS Reset and Clock Controller driver");
MODULE_LICENSE("GPL");
