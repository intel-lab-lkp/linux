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
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_clk.h>
#include <linux/slab.h>
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
	u64 numerator;
	unsigned long parent_rate;
	u32 divr1, frac = 0, m, n, out_div, source;

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

static int __init h7rs_register_fixed(struct clk_hw_onecell_data *data,
				      unsigned int id, const char *name,
				       unsigned long rate)
{
	struct clk_hw *hw;

	hw = clk_hw_register_fixed_rate(NULL, name, NULL, CLK_IS_CRITICAL, rate);
	if (IS_ERR(hw))
		return PTR_ERR(hw);

	data->hws[id] = hw;
	return 0;
}

static int __init h7rs_register_gate(struct clk_hw_onecell_data *data,
				     unsigned int id, const char *name,
				      const char *parent_name,
				      void __iomem *base, u16 offset,
				      u8 bit_idx)
{
	struct clk_hw *hw;

	hw = clk_hw_register_gate(NULL, name, parent_name, CLK_IGNORE_UNUSED,
				  base + offset, bit_idx, 0,
				  &stm32h7rs_rcc_lock);
	if (IS_ERR(hw))
		return PTR_ERR(hw);

	data->hws[id] = hw;
	return 0;
}

struct stm32h7rs_gate {
	u8 id;
	u8 bit_idx;
	u16 offset;
	const char *name;
	const char *parent_name;
};

static const struct stm32h7rs_gate stm32h7rs_gates[] __initconst = {
	{ STM32H7RS_TIM5_CK, 3, RCC_APB1ENR1, "tim5", "tim5_ker" },
	{ STM32H7RS_UART4_CK, 19, RCC_APB1ENR1, "uart4", "uart4_ker" },
	{ STM32H7RS_GPIOA_CK, 0, RCC_AHB4ENR, "gpioa", "h7rs_hclk" },
	{ STM32H7RS_GPIOB_CK, 1, RCC_AHB4ENR, "gpiob", "h7rs_hclk" },
	{ STM32H7RS_GPIOC_CK, 2, RCC_AHB4ENR, "gpioc", "h7rs_hclk" },
	{ STM32H7RS_GPIOD_CK, 3, RCC_AHB4ENR, "gpiod", "h7rs_hclk" },
	{ STM32H7RS_GPIOE_CK, 4, RCC_AHB4ENR, "gpioe", "h7rs_hclk" },
	{ STM32H7RS_GPIOF_CK, 5, RCC_AHB4ENR, "gpiof", "h7rs_hclk" },
	{ STM32H7RS_GPIOG_CK, 6, RCC_AHB4ENR, "gpiog", "h7rs_hclk" },
	{ STM32H7RS_GPIOH_CK, 7, RCC_AHB4ENR, "gpioh", "h7rs_hclk" },
	{ STM32H7RS_GPIOM_CK, 12, RCC_AHB4ENR, "gpiom", "h7rs_hclk" },
	{ STM32H7RS_GPION_CK, 13, RCC_AHB4ENR, "gpion", "h7rs_hclk" },
	{ STM32H7RS_GPIOO_CK, 14, RCC_AHB4ENR, "gpioo", "h7rs_hclk" },
	{ STM32H7RS_GPIOP_CK, 15, RCC_AHB4ENR, "gpiop", "h7rs_hclk" },
	{ STM32H7RS_SDMMC1_CK, 8, RCC_AHB5ENR, "sdmmc1", "sdmmc1_ker" },
};

static void __init stm32h7rs_rcc_init(struct device_node *np)
{
	struct clk_hw_onecell_data *clk_data;
	unsigned long hse_rate, lse_rate;
	unsigned long sys_rate, hclk_rate;
	unsigned long pclk1_rate, pclk2_rate, pclk4_rate, pclk5_rate;
	unsigned long timer_rate, uart4_rate, sdmmc1_rate;
	struct clk_hw *timer_hw = NULL, *uart4_hw = NULL, *sdmmc1_hw = NULL;
	struct clk *input;
	void __iomem *base;
	unsigned int fixed_count = 0, gate_count = 0;
	unsigned int i;
	u32 bmpre;
	int ret;

	base = of_iomap(np, 0);
	if (!base) {
		pr_err("%pOF: unable to map RCC registers\n", np);
		return;
	}

	input = of_clk_get(np, 0);
	if (IS_ERR(input)) {
		pr_err("%pOF: unable to get HSE input clock\n", np);
		goto err_unmap;
	}
	hse_rate = clk_get_rate(input);
	clk_put(input);

	input = of_clk_get(np, 1);
	if (IS_ERR(input)) {
		pr_err("%pOF: unable to get LSE input clock\n", np);
		goto err_unmap;
	}
	lse_rate = clk_get_rate(input);
	clk_put(input);

	clk_data = kzalloc(struct_size(clk_data, hws, STM32H7RS_MAX_CLKS),
			   GFP_KERNEL);
	if (!clk_data)
		goto err_unmap;
	clk_data->num = STM32H7RS_MAX_CLKS;

	sys_rate = stm32h7rs_sys_rate(base, hse_rate);
	bmpre = (readl_relaxed(base + RCC_BMCFGR) >>
		  RCC_BMCFGR_BMPRE_SHIFT) & 0xf;
	hclk_rate = stm32h7rs_ahb_rate(sys_rate, bmpre);
	pclk1_rate = stm32h7rs_apb_rate(base, hclk_rate, RCC_APBCFGR_PPRE1_SHIFT);
	pclk2_rate = stm32h7rs_apb_rate(base, hclk_rate, RCC_APBCFGR_PPRE2_SHIFT);
	pclk4_rate = stm32h7rs_apb_rate(base, hclk_rate, RCC_APBCFGR_PPRE4_SHIFT);
	pclk5_rate = stm32h7rs_apb_rate(base, hclk_rate, RCC_APBCFGR_PPRE5_SHIFT);
	timer_rate = stm32h7rs_timer_rate(base, hclk_rate, pclk1_rate);
	uart4_rate = stm32h7rs_uart4_rate(base, hse_rate, lse_rate, pclk1_rate);
	sdmmc1_rate = stm32h7rs_sdmmc1_rate(base, hse_rate);

	if (!sys_rate || !hclk_rate || !pclk1_rate || !pclk2_rate ||
	    !pclk4_rate || !pclk5_rate || !timer_rate || !uart4_rate ||
	    !sdmmc1_rate) {
		pr_err("%pOF: invalid boot clock configuration\n", np);
		goto err_free;
	}

	ret = h7rs_register_fixed(clk_data, STM32H7RS_SYS_CK,
				  "h7rs_sys_ck", sys_rate);
	if (ret)
		goto err_register;
	fixed_count++;
	ret = h7rs_register_fixed(clk_data, STM32H7RS_HCLK,
				  "h7rs_hclk", hclk_rate);
	if (ret)
		goto err_register;
	fixed_count++;
	ret = h7rs_register_fixed(clk_data, STM32H7RS_PCLK1,
				  "h7rs_pclk1", pclk1_rate);
	if (ret)
		goto err_register;
	fixed_count++;
	ret = h7rs_register_fixed(clk_data, STM32H7RS_PCLK2,
				  "h7rs_pclk2", pclk2_rate);
	if (ret)
		goto err_register;
	fixed_count++;
	ret = h7rs_register_fixed(clk_data, STM32H7RS_PCLK4,
				  "h7rs_pclk4", pclk4_rate);
	if (ret)
		goto err_register;
	fixed_count++;
	ret = h7rs_register_fixed(clk_data, STM32H7RS_PCLK5,
				  "h7rs_pclk5", pclk5_rate);
	if (ret)
		goto err_register;
	fixed_count++;

	timer_hw = clk_hw_register_fixed_rate(NULL, "tim5_ker", NULL,
					      CLK_IS_CRITICAL, timer_rate);
	if (IS_ERR(timer_hw)) {
		ret = PTR_ERR(timer_hw);
		timer_hw = NULL;
		goto err_register;
	}
	uart4_hw = clk_hw_register_fixed_rate(NULL, "uart4_ker", NULL,
					      CLK_IS_CRITICAL, uart4_rate);
	if (IS_ERR(uart4_hw)) {
		ret = PTR_ERR(uart4_hw);
		uart4_hw = NULL;
		goto err_register;
	}
	sdmmc1_hw = clk_hw_register_fixed_rate(NULL, "sdmmc1_ker", NULL,
					       CLK_IS_CRITICAL, sdmmc1_rate);
	if (IS_ERR(sdmmc1_hw)) {
		ret = PTR_ERR(sdmmc1_hw);
		sdmmc1_hw = NULL;
		goto err_register;
	}

	for (i = 0; i < ARRAY_SIZE(stm32h7rs_gates); i++) {
		const struct stm32h7rs_gate *gate = &stm32h7rs_gates[i];

		ret = h7rs_register_gate(clk_data, gate->id, gate->name,
					 gate->parent_name, base,
					   gate->offset, gate->bit_idx);
		if (ret) {
			pr_err("%pOF: failed to register %s clock: %d\n",
			       np, gate->name, ret);
			goto err_register;
		}
		gate_count++;
	}

	ret = of_clk_add_hw_provider(np, of_clk_hw_onecell_get, clk_data);
	if (ret) {
		pr_err("%pOF: failed to add clock provider: %d\n", np, ret);
		goto err_register;
	}

	pr_info("STM32H7RS RCC: sys=%lu hclk=%lu pclk1=%lu tim5=%lu uart4=%lu sdmmc1=%lu\n",
		sys_rate, hclk_rate, pclk1_rate, timer_rate, uart4_rate,
		sdmmc1_rate);
	return;

err_register:
	while (gate_count) {
		const struct stm32h7rs_gate *gate;

		gate = &stm32h7rs_gates[--gate_count];
		clk_hw_unregister_gate(clk_data->hws[gate->id]);
	}
	if (sdmmc1_hw)
		clk_hw_unregister_fixed_rate(sdmmc1_hw);
	if (uart4_hw)
		clk_hw_unregister_fixed_rate(uart4_hw);
	if (timer_hw)
		clk_hw_unregister_fixed_rate(timer_hw);
	while (fixed_count)
		clk_hw_unregister_fixed_rate(clk_data->hws[--fixed_count]);
	pr_err("%pOF: failed to register clocks: %d\n", np, ret);
err_free:
	kfree(clk_data);
err_unmap:
	iounmap(base);
}

CLK_OF_DECLARE_DRIVER(stm32h7rs_rcc, "st,stm32h7rs-rcc", stm32h7rs_rcc_init);
