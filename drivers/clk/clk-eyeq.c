// SPDX-License-Identifier: GPL-2.0-only
/*
 * PLL clock driver for the Mobileye EyeQ5, EyeQ6L and EyeQ6H platforms.
 *
 * This controller handles:
 *  - Read-only PLLs, all derived from the same main crystal clock.
 *  - It also exposes divider clocks, those are children to PLLs.
 *  - Fixed factor clocks, children to PLLs.
 *
 * Parent clock is expected to be constant. This driver's registers live in a
 * shared region called OLB. Some PLLs and fixed-factors are initialised early
 * by of_clk_init(); if so, two clk providers are registered.
 *
 * We use eqc_ as prefix, as-in "EyeQ Clock", but way shorter.
 *
 * Copyright (C) 2024 Mobileye Vision Technologies Ltd.
 */

/*
 * Set pr_fmt() for printing from eqc_early_init().
 * It is called at of_clk_init() stage (read: really early).
 */
#define pr_fmt(fmt) "clk-eyeq: " fmt

#include <linux/array_size.h>
#include <linux/auxiliary_bus.h>
#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/clk-provider.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/io-64-nonatomic-hi-lo.h>
#include <linux/io.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/overflow.h>
#include <linux/platform_device.h>
#include <linux/printk.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/types.h>

#include <dt-bindings/clock/mobileye,eyeq-clk.h>

/* In frac mode, it enables fractional noise canceling DAC. Else, no function. */
#define FRACG_PCSR0_DAC_EN			BIT(0)
/* Fractional or integer mode */
#define FRACG_PCSR0_DSM_EN			BIT(1)
#define FRACG_PCSR0_PLL_EN			BIT(2)
/* All clocks output held at 0 */
#define FRACG_PCSR0_FOUTPOSTDIV_EN		BIT(3)
#define FRACG_PCSR0_POST_DIV1			GENMASK(6, 4)
#define FRACG_PCSR0_POST_DIV2			GENMASK(9, 7)
#define FRACG_PCSR0_REF_DIV			GENMASK(15, 10)
#define FRACG_PCSR0_INTIN			GENMASK(27, 16)
#define FRACG_PCSR0_BYPASS			BIT(28)
/* Bits 30..29 are reserved */
#define FRACG_PCSR0_PLL_LOCKED			BIT(31)

#define FRACG_PCSR1_RESET			BIT(0)
#define FRACG_PCSR1_SSGC_DIV			GENMASK(4, 1)
/* Spread amplitude (% = 0.1 * SPREAD[4:0]) */
#define FRACG_PCSR1_SPREAD			GENMASK(9, 5)
#define FRACG_PCSR1_DIS_SSCG			BIT(10)
/* Down-spread or center-spread */
#define FRACG_PCSR1_DOWN_SPREAD			BIT(11)
#define FRACG_PCSR1_FRAC_IN			GENMASK(31, 12)

#define JFRACR_PCSR0_BYPASS			BIT(0)
#define JFRACR_PCSR0_PLL_EN			BIT(1)
#define JFRACR_PCSR0_FOUTVCO_EN			BIT(2)
#define JFRACR_PCSR0_FOUTPOSTDIV_EN		BIT(3)
#define JFRACR_PCSR0_POST_DIV1			GENMASK(6, 4)
#define JFRACR_PCSR0_POST_DIV2			GENMASK(9, 7)
#define JFRACR_PCSR0_REF_DIV			GENMASK(15, 10)
#define JFRACR_PCSR0_FB_DIV			GENMASK(27, 16)
#define JFRACR_PCSR0_VCO_SEL			GENMASK(29, 28)
#define JFRACR_PCSR0_PLL_LOCKED			GENMASK(31, 30)

#define JFRACR_PCSR1_FRAC_IN			GENMASK(23, 0)
#define JFRACR_PCSR1_FOUT4PHASE_EN		BIT(24)
#define JFRACR_PCSR1_DAC_EN			BIT(25)
#define JFRACR_PCSR1_DSM_EN			BIT(26)
/* Bits 31..27 are reserved */
#define JFRACR_PCSR2_RESET			BIT(0)
#define JFRACR_PCSR2_DIS_SSCG			BIT(1)
#define JFRACR_PCSR2_DOWN_SPREAD		BIT(2)
#define JFRACR_PCSR2_SSGC_DIV			GENMASK(7, 4)
#define JFRACR_PCSR2_SPREAD			GENMASK(12, 8)
/* Bits 31..13 are reserved */

#define AINTP_PCSR_BYPASS			BIT(0)
#define AINTP_PCSR_PLL_EN			BIT(1)
#define AINTP_PCSR_FOUTVCO_EN			BIT(2)
#define AINTP_PCSR_FOUTPOSTDIV_EN		BIT(3)
#define AINTP_PCSR_POST_DIV1			GENMASK(6, 4)
#define AINTP_PCSR_POST_DIV2			GENMASK(9, 7)
#define AINTP_PCSR_REF_DIV			GENMASK(15, 10)
#define AINTP_PCSR_FB_DIV			GENMASK(27, 16)
#define AINTP_PCSR_VCO_SEL			GENMASK(29, 28)
#define AINTP_PCSR_PLL_LOCKED			GENMASK(31, 30)

enum eqc_pll_type {
	EQC_PLL_FRACG,
	EQC_PLL_JFRACR,
	EQC_PLL_AINTP,
};

struct eqc_pll {
	unsigned int		index;
	const char		*name;
	unsigned int		reg;
	enum eqc_pll_type	type;
	const char		*parent_name;
};

/*
 * Divider clock. Divider is 2*(v+1), with v the register value.
 * Min divider is 2, max is 2*(2^width).
 */
struct eqc_div {
	unsigned int	index;
	const char	*name;
	unsigned int	parent_idx;
	const char	*parent_name;
	unsigned int	reg;
	u8		shift;
	u8		width;
};

struct eqc_fixed_factor {
	unsigned int	index;
	const char	*name;
	unsigned int	mult;
	unsigned int	div;
	unsigned int	parent_idx;
	const char	*parent_name;
};

struct eqc_match_data {
	unsigned int		pll_count;
	const struct eqc_pll	*plls;

	unsigned int		div_count;
	const struct eqc_div	*divs;

	unsigned int			fixed_factor_count;
	const struct eqc_fixed_factor	*fixed_factors;

	const char		*reset_auxdev_name;
	const char		*pinctrl_auxdev_name;

	unsigned int		early_clk_count;
};

struct eqc_early_match_data {
	unsigned int		early_pll_count;
	const struct eqc_pll	*early_plls;

	unsigned int			early_fixed_factor_count;
	const struct eqc_fixed_factor	*early_fixed_factors;

	/*
	 * We want our of_xlate callback to EPROBE_DEFER instead of dev_err()
	 * and EINVAL. For that, we must know the total clock count.
	 */
	unsigned int		late_clk_count;
};

/*
 * Both factors (mult and div) must fit in 32 bits. When an operation overflows,
 * this function throws away low bits so that factors still fit in 32 bits.
 *
 * Precision loss depends on amplitude of mult and div. Worst theoretical
 * loss is: (UINT_MAX+1) / UINT_MAX - 1 = 2.3e-10.
 * This is 1Hz every 4.3GHz.
 */
static void eqc_pll_downshift_factors(unsigned long *mult, unsigned long *div)
{
	unsigned long biggest;
	unsigned int shift;

	/* This function can be removed if mult/div switch to unsigned long. */
	static_assert(sizeof_field(struct clk_fixed_factor, mult) == sizeof(unsigned int));
	static_assert(sizeof_field(struct clk_fixed_factor, div) == sizeof(unsigned int));

	/* No overflow, nothing to be done. */
	if (*mult <= UINT_MAX && *div <= UINT_MAX)
		return;

	/*
	 * Compute the shift required to bring the biggest factor into unsigned
	 * int range. That is, shift its highest set bit to the unsigned int
	 * most significant bit.
	 */
	biggest = max(*mult, *div);
	shift = __fls(biggest) - (BITS_PER_BYTE * sizeof(unsigned int)) + 1;

	*mult >>= shift;
	*div >>= shift;
}

static int eqc_pll_parse_fracg(void __iomem *base, unsigned long *mult,
			       unsigned long *div, unsigned long *acc)
{
	u64 val;
	u32 r0, r1;
	u32 spread;

	val = readq(base);
	r0 = val;
	r1 = val >> 32;

	if (r0 & FRACG_PCSR0_BYPASS) {
		*mult = 1;
		*div = 1;
		*acc = 0;
		return 0;
	}

	if (!(r0 & FRACG_PCSR0_PLL_LOCKED))
		return -EINVAL;

	*mult = FIELD_GET(FRACG_PCSR0_INTIN, r0);
	*div = FIELD_GET(FRACG_PCSR0_REF_DIV, r0);

	/* Fractional mode, in 2^20 (0x100000) parts. */
	if (r0 & FRACG_PCSR0_DSM_EN) {
		*div *= (1ULL << 20);
		*mult = *mult * (1ULL << 20) + FIELD_GET(FRACG_PCSR1_FRAC_IN, r1);
	}

	if (!*mult || !*div)
		return -EINVAL;

	if (r1 & (FRACG_PCSR1_RESET | FRACG_PCSR1_DIS_SSCG)) {
		*acc = 0;
		return 0;
	}

	/*
	 * Spread spectrum.
	 *
	 * Spread is 1/1000 parts of frequency, accuracy is half of
	 * that. To get accuracy, convert to ppb (parts per billion).
	 *
	 * acc = spread * 1e6 / 2
	 *   with acc in parts per billion and,
	 *        spread in parts per thousand.
	 */
	spread = FIELD_GET(FRACG_PCSR1_SPREAD, r1);
	*acc = spread * 500000;

	if (r1 & FRACG_PCSR1_DOWN_SPREAD) {
		/*
		 * Downspreading: the central frequency is half a
		 * spread lower.
		 */
		*mult *= 2000 - spread;
		*div *= 2000;

		/*
		 * Previous operation might overflow 32 bits. If it
		 * does, throw away the least amount of low bits.
		 */
		eqc_pll_downshift_factors(mult, div);
	}

	return 0;
}

static int eqc_pll_parse_jfracr(void __iomem *base, unsigned long *mult,
				unsigned long *div, unsigned long *acc)
{
	u64 val;
	u32 r0, r1, r2;
	u32 spread;

	val = readq(base);
	r0 = val;
	r1 = val >> 32;
	r2 = readl(base + 8);

	if (r0 & JFRACR_PCSR0_BYPASS) {
		*mult = 1;
		*div = 1;
		*acc = 0;
		return 0;
	}

	if (!(r0 & JFRACR_PCSR0_PLL_LOCKED))
		return -EINVAL;

	*mult = FIELD_GET(JFRACR_PCSR0_FB_DIV, r0);
	*div = FIELD_GET(JFRACR_PCSR0_REF_DIV, r0);

	if (r1 & JFRACR_PCSR1_DSM_EN) {
		*div *= (1ULL << 20);
		*mult = *mult * (1ULL << 20) + FIELD_GET(JFRACR_PCSR1_FRAC_IN, r1);
	}

	if (!*mult || !*div)
		return -EINVAL;

	if (r2 & (JFRACR_PCSR2_RESET | JFRACR_PCSR2_DIS_SSCG)) {
		*acc = 0;
		return 0;
	}

	spread = FIELD_GET(JFRACR_PCSR2_SPREAD, r2);
	*acc = spread * 500000;

	if (r2 & JFRACR_PCSR2_DOWN_SPREAD) {
		*mult *= 2000 - spread;
		*div *= 2000;
		eqc_pll_downshift_factors(mult, div);
	}

	return 0;
}

static int eqc_pll_parse_aintp(void __iomem *base, unsigned long *mult,
			       unsigned long *div, unsigned long *acc)
{
	u32 r0;

	/* no spread spectrum */
	*acc = 0;

	r0 = readl(base);
	if (r0 & AINTP_PCSR_BYPASS) {
		*mult = 1;
		*div = 1;
		return 0;
	}

	if (!(r0 & AINTP_PCSR_PLL_LOCKED))
		return -EINVAL;

	*mult = FIELD_GET(AINTP_PCSR_FB_DIV, r0);
	*div = FIELD_GET(AINTP_PCSR_REF_DIV, r0);

	if (!*mult || !*div)
		return -EINVAL;

	return 0;
}

static int eqc_parse_one_pll(void __iomem *base, enum eqc_pll_type type, unsigned long *mult,
			     unsigned long *div, unsigned long *acc)
{
	switch (type) {
	case EQC_PLL_FRACG:
		return eqc_pll_parse_fracg(base, mult, div, acc);
	case EQC_PLL_JFRACR:
		return eqc_pll_parse_jfracr(base, mult, div, acc);
	case EQC_PLL_AINTP:
		return eqc_pll_parse_aintp(base, mult, div, acc);
	}
	return -EINVAL;
}

static void eqc_probe_init_plls(struct device *dev, const struct eqc_match_data *data,
				void __iomem *base, struct clk_hw_onecell_data *cells)
{
	struct device_node *np = dev->of_node;
	unsigned long mult, div, acc;
	const struct eqc_pll *pll;
	const char *fw_name;
	struct clk_hw *hw;
	unsigned int i;
	int ret;

	for (i = 0; i < data->pll_count; i++) {
		pll = &data->plls[i];

		ret = eqc_parse_one_pll(base + pll->reg, pll->type, &mult, &div, &acc);
		if (ret) {
			dev_warn(dev, "failed parsing state of %s\n", pll->name);
			cells->hws[pll->index] = ERR_PTR(ret);
			continue;
		}

		if (!pll->parent_name)
			fw_name = "ref";
		else if (of_property_match_string(np, "clock-names", pll->parent_name) >= 0)
			fw_name = pll->parent_name;
		else
			fw_name = NULL;

		if (fw_name)
			hw = clk_hw_register_fixed_factor_with_accuracy_fwname(dev,
					np, pll->name, fw_name, 0, mult, div, acc);
		else
			hw = clk_hw_register_fixed_factor_with_accuracy(dev,
					pll->name, pll->parent_name, 0, mult, div, acc);

		cells->hws[pll->index] = hw;
		if (IS_ERR(hw))
			dev_warn(dev, "failed registering %s: %pe\n", pll->name, hw);
	}
}

static void eqc_probe_init_divs(struct device *dev, const struct eqc_match_data *data,
				void __iomem *base, struct clk_hw_onecell_data *cells)
{
	struct clk_parent_data parent_data = { };
	const struct eqc_div *div;
	struct clk_hw *parent;
	void __iomem *reg;
	struct clk_hw *hw;
	unsigned int i;

	for (i = 0; i < data->div_count; i++) {
		div = &data->divs[i];
		reg = base + div->reg;
		parent = cells->hws[div->parent_idx];

		if (IS_ERR(parent)) {
			/* Parent is in early clk provider. */
			parent_data.index = -1;
			parent_data.name = div->parent_name;
			parent_data.hw = NULL;
		} else {
			/* Avoid clock lookup when we already have the hw reference. */
			parent_data.index = 0;
			parent_data.hw = parent;
		}

		hw = clk_hw_register_divider_table_parent_data(dev, div->name,
				&parent_data, 0, reg, div->shift, div->width,
				CLK_DIVIDER_EVEN_INTEGERS, NULL, NULL);
		cells->hws[div->index] = hw;
		if (IS_ERR(hw))
			dev_warn(dev, "failed registering %s: %pe\n",
				 div->name, hw);
	}
}

static void eqc_probe_init_fixed_factors(struct device *dev,
					 const struct eqc_match_data *data,
					 struct clk_hw_onecell_data *cells)
{
	const struct eqc_fixed_factor *ff;
	struct clk_hw *hw, *parent_hw;
	unsigned int i;

	for (i = 0; i < data->fixed_factor_count; i++) {
		ff = &data->fixed_factors[i];
		parent_hw = cells->hws[ff->parent_idx];

		if (IS_ERR(parent_hw)) {
			/* Parent is in early clk provider. */
			hw = clk_hw_register_fixed_factor(dev, ff->name,
					ff->parent_name, 0, ff->mult, ff->div);
		} else {
			/* Avoid clock lookup when we already have the hw reference. */
			hw = clk_hw_register_fixed_factor_parent_hw(dev, ff->name,
					parent_hw, 0, ff->mult, ff->div);
		}

		cells->hws[ff->index] = hw;
		if (IS_ERR(hw))
			dev_warn(dev, "failed registering %s: %pe\n",
				 ff->name, hw);
	}
}

static void eqc_auxdev_release(struct device *dev)
{
	struct auxiliary_device *adev = to_auxiliary_dev(dev);

	kfree(adev);
}

static int eqc_auxdev_create(struct device *dev, void __iomem *base,
			     const char *name, u32 id)
{
	struct auxiliary_device *adev;
	int ret;

	adev = kzalloc(sizeof(*adev), GFP_KERNEL);
	if (!adev)
		return -ENOMEM;

	adev->name = name;
	adev->dev.parent = dev;
	adev->dev.platform_data = (void __force *)base;
	adev->dev.release = eqc_auxdev_release;
	adev->id = id;

	ret = auxiliary_device_init(adev);
	if (ret)
		return ret;

	ret = auxiliary_device_add(adev);
	if (ret)
		auxiliary_device_uninit(adev);

	return ret;
}

static int eqc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	const struct eqc_match_data *data;
	struct clk_hw_onecell_data *cells;
	unsigned int i, clk_count;
	struct resource *res;
	void __iomem *base;
	int ret;

	data = device_get_match_data(dev);
	if (!data)
		return 0; /* No clocks nor auxdevs, we are done. */

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
		return -ENODEV;

	base = ioremap(res->start, resource_size(res));
	if (!base)
		return -ENOMEM;

	/* Init optional reset auxiliary device. */
	if (data->reset_auxdev_name) {
		ret = eqc_auxdev_create(dev, base, data->reset_auxdev_name, 0);
		if (ret)
			dev_warn(dev, "failed creating auxiliary device %s.%s: %d\n",
				 KBUILD_MODNAME, data->reset_auxdev_name, ret);
	}

	/* Init optional pinctrl auxiliary device. */
	if (data->pinctrl_auxdev_name) {
		ret = eqc_auxdev_create(dev, base, data->pinctrl_auxdev_name, 0);
		if (ret)
			dev_warn(dev, "failed creating auxiliary device %s.%s: %d\n",
				 KBUILD_MODNAME, data->pinctrl_auxdev_name, ret);
	}

	if (data->pll_count + data->div_count + data->fixed_factor_count == 0)
		return 0; /* Zero clocks, we are done. */

	clk_count = data->pll_count + data->div_count +
		    data->fixed_factor_count + data->early_clk_count;
	cells = kzalloc(struct_size(cells, hws, clk_count), GFP_KERNEL);
	if (!cells)
		return -ENOMEM;

	cells->num = clk_count;

	/* Early PLLs are marked as errors: the early provider will get queried. */
	for (i = 0; i < clk_count; i++)
		cells->hws[i] = ERR_PTR(-EINVAL);

	eqc_probe_init_plls(dev, data, base, cells);

	eqc_probe_init_divs(dev, data, base, cells);

	eqc_probe_init_fixed_factors(dev, data, cells);

	return of_clk_add_hw_provider(np, of_clk_hw_onecell_get, cells);
}

/* Required early for GIC timer (pll-cpu) and UARTs (pll-per). */
static const struct eqc_pll eqc_eyeq5_early_plls[] = {
	{ .index = EQ5C_PLL_CPU, .name = "pll-cpu",  .reg = 0x02C, .type = EQC_PLL_FRACG },
	{ .index = EQ5C_PLL_PER, .name = "pll-per",  .reg = 0x05C, .type = EQC_PLL_FRACG },
};

static const struct eqc_pll eqc_eyeq5_plls[] = {
	{ .index = EQ5C_PLL_VMP,  .name = "pll-vmp",  .reg = 0x034, .type = EQC_PLL_FRACG },
	{ .index = EQ5C_PLL_PMA,  .name = "pll-pma",  .reg = 0x03C, .type = EQC_PLL_FRACG },
	{ .index = EQ5C_PLL_VDI,  .name = "pll-vdi",  .reg = 0x044, .type = EQC_PLL_FRACG },
	{ .index = EQ5C_PLL_DDR0, .name = "pll-ddr0", .reg = 0x04C, .type = EQC_PLL_FRACG },
	{ .index = EQ5C_PLL_PCI,  .name = "pll-pci",  .reg = 0x054, .type = EQC_PLL_FRACG },
	{ .index = EQ5C_PLL_PMAC, .name = "pll-pmac", .reg = 0x064, .type = EQC_PLL_FRACG },
	{ .index = EQ5C_PLL_MPC,  .name = "pll-mpc",  .reg = 0x06C, .type = EQC_PLL_FRACG },
	{ .index = EQ5C_PLL_DDR1, .name = "pll-ddr1", .reg = 0x074, .type = EQC_PLL_FRACG },
};

enum {
	/*
	 * EQ5C_PLL_CPU children.
	 * EQ5C_PER_OCC_PCI is the last clock exposed in dt-bindings.
	 */
	EQ5C_CPU_OCC = EQ5C_PER_OCC_PCI + 1,
	EQ5C_CPU_SI_CSS0,
	EQ5C_CPU_CPC,
	EQ5C_CPU_CM,
	EQ5C_CPU_MEM,
	EQ5C_CPU_OCC_ISRAM,
	EQ5C_CPU_ISRAM,
	EQ5C_CPU_OCC_DBU,
	EQ5C_CPU_SI_DBU_TP,

	/*
	 * EQ5C_PLL_VDI children.
	 */
	EQ5C_VDI_OCC_VDI,
	EQ5C_VDI_VDI,
	EQ5C_VDI_OCC_CAN_SER,
	EQ5C_VDI_CAN_SER,
	EQ5C_VDI_I2C_SER,

	/*
	 * EQ5C_PLL_PER children.
	 */
	EQ5C_PER_PERIPH,
	EQ5C_PER_CAN,
	EQ5C_PER_TIMER,
	EQ5C_PER_CCF,
	EQ5C_PER_OCC_MJPEG,
	EQ5C_PER_HSM,
	EQ5C_PER_MJPEG,
	EQ5C_PER_FCMU_A,
};

static const struct eqc_fixed_factor eqc_eyeq5_early_fixed_factors[] = {
	/* EQ5C_PLL_CPU children */
	{ EQ5C_CPU_OCC,		"occ-cpu",	1, 1,	EQ5C_PLL_CPU },
	{ EQ5C_CPU_SI_CSS0,	"si-css0",	1, 1,	EQ5C_CPU_OCC },
	{ EQ5C_CPU_CORE0,	"core0",	1, 1,	EQ5C_CPU_SI_CSS0 },
	{ EQ5C_CPU_CORE1,	"core1",	1, 1,	EQ5C_CPU_SI_CSS0 },
	{ EQ5C_CPU_CORE2,	"core2",	1, 1,	EQ5C_CPU_SI_CSS0 },
	{ EQ5C_CPU_CORE3,	"core3",	1, 1,	EQ5C_CPU_SI_CSS0 },

	/* EQ5C_PLL_PER children */
	{ EQ5C_PER_OCC,		"occ-periph",	1, 16,	EQ5C_PLL_PER },
	{ EQ5C_PER_UART,	"uart",		1, 1,	EQ5C_PER_OCC },
};

static const struct eqc_fixed_factor eqc_eyeq5_fixed_factors[] = {
	/* EQ5C_PLL_CPU children */
	{ EQ5C_CPU_CPC,		"cpc",		1, 1,	EQ5C_CPU_SI_CSS0,	"si-css0" },
	{ EQ5C_CPU_CM,		"cm",		1, 1,	EQ5C_CPU_SI_CSS0,	"si-css0" },
	{ EQ5C_CPU_MEM,		"mem",		1, 1,	EQ5C_CPU_SI_CSS0,	"si-css0" },
	{ EQ5C_CPU_OCC_ISRAM,	"occ-isram",	1, 2,	EQ5C_PLL_CPU,		"pll-cpu" },
	{ EQ5C_CPU_ISRAM,	"isram",	1, 1,	EQ5C_CPU_OCC_ISRAM },
	{ EQ5C_CPU_OCC_DBU,	"occ-dbu",	1, 10,	EQ5C_PLL_CPU,		"pll-cpu" },
	{ EQ5C_CPU_SI_DBU_TP,	"si-dbu-tp",	1, 1,	EQ5C_CPU_OCC_DBU },

	/* EQ5C_PLL_VDI children */
	{ EQ5C_VDI_OCC_VDI,	"occ-vdi",	1, 2,	EQ5C_PLL_VDI,		"pll-vdi" },
	{ EQ5C_VDI_VDI,		"vdi",		1, 1,	EQ5C_VDI_OCC_VDI },
	{ EQ5C_VDI_OCC_CAN_SER,	"occ-can-ser",	1, 16,	EQ5C_PLL_VDI,		"pll-vdi" },
	{ EQ5C_VDI_CAN_SER,	"can-ser",	1, 1,	EQ5C_VDI_OCC_CAN_SER },
	{ EQ5C_VDI_I2C_SER,	"i2c-ser",	1, 20,	EQ5C_PLL_VDI,		"pll-vdi" },

	/* EQ5C_PLL_PER children */
	{ EQ5C_PER_PERIPH,	"periph",	1, 1,	EQ5C_PER_OCC,		 "occ-periph" },
	{ EQ5C_PER_CAN,		"can",		1, 1,	EQ5C_PER_OCC,		 "occ-periph" },
	{ EQ5C_PER_SPI,		"spi",		1, 1,	EQ5C_PER_OCC,		 "occ-periph" },
	{ EQ5C_PER_I2C,		"i2c",		1, 1,	EQ5C_PER_OCC,		 "occ-periph" },
	{ EQ5C_PER_TIMER,	"timer",	1, 1,	EQ5C_PER_OCC,		 "occ-periph" },
	{ EQ5C_PER_GPIO,	"gpio",		1, 1,	EQ5C_PER_OCC,		 "occ-periph" },
	{ EQ5C_PER_EMMC,	"emmc-sys",	1, 10,	EQ5C_PLL_PER,		 "pll-per" },
	{ EQ5C_PER_CCF,		"ccf-ctrl",	1, 4,	EQ5C_PLL_PER,		 "pll-per" },
	{ EQ5C_PER_OCC_MJPEG,	"occ-mjpeg",	1, 2,	EQ5C_PLL_PER,		 "pll-per" },
	{ EQ5C_PER_HSM,		"hsm",		1, 1,	EQ5C_PER_OCC_MJPEG },
	{ EQ5C_PER_MJPEG,	"mjpeg",	1, 1,	EQ5C_PER_OCC_MJPEG },
	{ EQ5C_PER_FCMU_A,	"fcmu-a",	1, 20,	EQ5C_PLL_PER,		 "pll-per" },
	{ EQ5C_PER_OCC_PCI,	"occ-pci-sys",	1, 8,	EQ5C_PLL_PER,		 "pll-per" },
};

static const struct eqc_div eqc_eyeq5_divs[] = {
	{
		.index = EQ5C_DIV_OSPI,
		.name = "div-ospi",
		.parent_idx = EQ5C_PLL_PER,
		.parent_name = "pll-per",
		.reg = 0x11C,
		.shift = 0,
		.width = 4,
	},
};

static const struct eqc_early_match_data eqc_eyeq5_early_match_data __initconst = {
	.early_pll_count	= ARRAY_SIZE(eqc_eyeq5_early_plls),
	.early_plls		= eqc_eyeq5_early_plls,

	.early_fixed_factor_count	= ARRAY_SIZE(eqc_eyeq5_early_fixed_factors),
	.early_fixed_factors		= eqc_eyeq5_early_fixed_factors,

	.late_clk_count		= ARRAY_SIZE(eqc_eyeq5_plls) + ARRAY_SIZE(eqc_eyeq5_divs) +
				  ARRAY_SIZE(eqc_eyeq5_fixed_factors),
};

static const struct eqc_match_data eqc_eyeq5_match_data = {
	.pll_count	= ARRAY_SIZE(eqc_eyeq5_plls),
	.plls		= eqc_eyeq5_plls,

	.div_count	= ARRAY_SIZE(eqc_eyeq5_divs),
	.divs		= eqc_eyeq5_divs,

	.fixed_factor_count	= ARRAY_SIZE(eqc_eyeq5_fixed_factors),
	.fixed_factors		= eqc_eyeq5_fixed_factors,

	.reset_auxdev_name = "reset",
	.pinctrl_auxdev_name = "pinctrl",

	.early_clk_count = ARRAY_SIZE(eqc_eyeq5_early_plls) +
			   ARRAY_SIZE(eqc_eyeq5_early_fixed_factors),
};

static const struct eqc_pll eqc_eyeq6l_plls[] = {
	{ .index = EQ6LC_PLL_DDR, .name = "pll-ddr", .reg = 0x02C, .type = EQC_PLL_FRACG },
	/* pll-cpu also for acc */
	{ .index = EQ6LC_PLL_CPU, .name = "pll-cpu", .reg = 0x034, .type = EQC_PLL_FRACG },
	{ .index = EQ6LC_PLL_PER, .name = "pll-per", .reg = 0x03C, .type = EQC_PLL_FRACG },
	{ .index = EQ6LC_PLL_VDI, .name = "pll-vdi", .reg = 0x044, .type = EQC_PLL_FRACG },
};

static const struct eqc_match_data eqc_eyeq6l_match_data = {
	.pll_count	= ARRAY_SIZE(eqc_eyeq6l_plls),
	.plls		= eqc_eyeq6l_plls,

	.reset_auxdev_name = "reset",
};

static const struct eqc_match_data eqc_eyeq6h_west_match_data = {
	.reset_auxdev_name = "reset_west",
};

static const struct eqc_pll eqc_eyeq6h_east_plls[] = {
	{ .index = 0, .name = "pll-east", .reg = 0x074, .type = EQC_PLL_FRACG },
};

static const struct eqc_match_data eqc_eyeq6h_east_match_data = {
	.pll_count	= ARRAY_SIZE(eqc_eyeq6h_east_plls),
	.plls		= eqc_eyeq6h_east_plls,

	.reset_auxdev_name = "reset_east",
};

static const struct eqc_pll eqc_eyeq6h_south_plls[] = {
	{ .index = EQ6HC_SOUTH_PLL_VDI,  .name = "pll-vdi",  .reg = 0x000, .type = EQC_PLL_FRACG },
	{ .index = EQ6HC_SOUTH_PLL_PCIE, .name = "pll-pcie", .reg = 0x008, .type = EQC_PLL_FRACG },
	{ .index = EQ6HC_SOUTH_PLL_PER,  .name = "pll-per",  .reg = 0x010, .type = EQC_PLL_FRACG },
	{ .index = EQ6HC_SOUTH_PLL_ISP,  .name = "pll-isp",  .reg = 0x018, .type = EQC_PLL_FRACG },
};

static const struct eqc_div eqc_eyeq6h_south_divs[] = {
	{
		.index = EQ6HC_SOUTH_DIV_EMMC,
		.name = "div-emmc",
		.parent_idx = EQ6HC_SOUTH_PLL_PER,
		.reg = 0x070,
		.shift = 4,
		.width = 4,
	},
	{
		.index = EQ6HC_SOUTH_DIV_OSPI_REF,
		.name = "div-ospi-ref",
		.parent_idx = EQ6HC_SOUTH_PLL_PER,
		.reg = 0x090,
		.shift = 4,
		.width = 4,
	},
	{
		.index = EQ6HC_SOUTH_DIV_OSPI_SYS,
		.name = "div-ospi-sys",
		.parent_idx = EQ6HC_SOUTH_PLL_PER,
		.reg = 0x090,
		.shift = 8,
		.width = 1,
	},
	{
		.index = EQ6HC_SOUTH_DIV_TSU,
		.name = "div-tsu",
		.parent_idx = EQ6HC_SOUTH_PLL_PCIE,
		.reg = 0x098,
		.shift = 4,
		.width = 8,
	},
};

static const struct eqc_match_data eqc_eyeq6h_south_match_data = {
	.pll_count	= ARRAY_SIZE(eqc_eyeq6h_south_plls),
	.plls		= eqc_eyeq6h_south_plls,

	.div_count	= ARRAY_SIZE(eqc_eyeq6h_south_divs),
	.divs		= eqc_eyeq6h_south_divs,
};

static const struct eqc_pll eqc_eyeq6h_ddr0_plls[] = {
	{ .index = 0, .name = "pll-ddr0", .reg = 0x074, .type = EQC_PLL_FRACG },
};

static const struct eqc_match_data eqc_eyeq6h_ddr0_match_data = {
	.pll_count	= ARRAY_SIZE(eqc_eyeq6h_ddr0_plls),
	.plls		= eqc_eyeq6h_ddr0_plls,
};

static const struct eqc_pll eqc_eyeq6h_ddr1_plls[] = {
	{ .index = 0, .name = "pll-ddr1", .reg = 0x074, .type = EQC_PLL_FRACG },
};

static const struct eqc_match_data eqc_eyeq6h_ddr1_match_data = {
	.pll_count	= ARRAY_SIZE(eqc_eyeq6h_ddr1_plls),
	.plls		= eqc_eyeq6h_ddr1_plls,
};

static const struct eqc_pll eqc_eyeq6h_acc_plls[] = {
	{ .index = EQ6HC_ACC_PLL_XNN, .name = "pll-xnn", .reg = 0x040, .type = EQC_PLL_FRACG },
	{ .index = EQ6HC_ACC_PLL_VMP, .name = "pll-vmp", .reg = 0x050, .type = EQC_PLL_FRACG },
	{ .index = EQ6HC_ACC_PLL_PMA, .name = "pll-pma", .reg = 0x05C, .type = EQC_PLL_FRACG },
	{ .index = EQ6HC_ACC_PLL_MPC, .name = "pll-mpc", .reg = 0x068, .type = EQC_PLL_FRACG },
	{ .index = EQ6HC_ACC_PLL_NOC, .name = "pll-noc", .reg = 0x070, .type = EQC_PLL_FRACG },
};

static const struct eqc_match_data eqc_eyeq6h_acc_match_data = {
	.pll_count	= ARRAY_SIZE(eqc_eyeq6h_acc_plls),
	.plls		= eqc_eyeq6h_acc_plls,

	.reset_auxdev_name = "reset_acc",
};

static const struct of_device_id eqc_match_table[] = {
	{ .compatible = "mobileye,eyeq5-olb", .data = &eqc_eyeq5_match_data },
	{ .compatible = "mobileye,eyeq6l-olb", .data = &eqc_eyeq6l_match_data },
	{ .compatible = "mobileye,eyeq6h-west-olb", .data = &eqc_eyeq6h_west_match_data },
	{ .compatible = "mobileye,eyeq6h-east-olb", .data = &eqc_eyeq6h_east_match_data },
	{ .compatible = "mobileye,eyeq6h-south-olb", .data = &eqc_eyeq6h_south_match_data },
	{ .compatible = "mobileye,eyeq6h-ddr0-olb", .data = &eqc_eyeq6h_ddr0_match_data },
	{ .compatible = "mobileye,eyeq6h-ddr1-olb", .data = &eqc_eyeq6h_ddr1_match_data },
	{ .compatible = "mobileye,eyeq6h-acc-olb", .data = &eqc_eyeq6h_acc_match_data },
	{}
};

static struct platform_driver eqc_driver = {
	.probe = eqc_probe,
	.driver = {
		.name = "clk-eyeq",
		.of_match_table = eqc_match_table,
		.suppress_bind_attrs = true,
	},
};
builtin_platform_driver(eqc_driver);

/* Required early for GIC timer. */
static const struct eqc_pll eqc_eyeq6h_central_early_plls[] = {
	{ .index = EQ6HC_CENTRAL_PLL_CPU, .name = "pll-cpu", .reg = 0x02C, .type = EQC_PLL_FRACG },
};

static const struct eqc_fixed_factor eqc_eyeq6h_central_early_fixed_factors[] = {
	{ EQ6HC_CENTRAL_CPU_OCC, "occ-cpu", 1, 1, EQ6HC_CENTRAL_PLL_CPU },
};

static const struct eqc_early_match_data eqc_eyeq6h_central_early_match_data __initconst = {
	.early_pll_count	= ARRAY_SIZE(eqc_eyeq6h_central_early_plls),
	.early_plls		= eqc_eyeq6h_central_early_plls,

	.early_fixed_factor_count = ARRAY_SIZE(eqc_eyeq6h_central_early_fixed_factors),
	.early_fixed_factors = eqc_eyeq6h_central_early_fixed_factors,
};

/* Required early for UART. */
static const struct eqc_pll eqc_eyeq6h_west_early_plls[] = {
	{ .index = EQ6HC_WEST_PLL_PER, .name = "pll-west", .reg = 0x074, .type = EQC_PLL_FRACG },
};

static const struct eqc_fixed_factor eqc_eyeq6h_west_early_fixed_factors[] = {
	{ EQ6HC_WEST_PER_OCC,  "west-per-occ",  1, 10, EQ6HC_WEST_PLL_PER },
	{ EQ6HC_WEST_PER_UART, "west-per-uart", 1, 1,  EQ6HC_WEST_PER_OCC },
};

static const struct eqc_early_match_data eqc_eyeq6h_west_early_match_data __initconst = {
	.early_pll_count	= ARRAY_SIZE(eqc_eyeq6h_west_early_plls),
	.early_plls		= eqc_eyeq6h_west_early_plls,

	.early_fixed_factor_count = ARRAY_SIZE(eqc_eyeq6h_west_early_fixed_factors),
	.early_fixed_factors = eqc_eyeq6h_west_early_fixed_factors,
};

static void __init eqc_early_init(struct device_node *np,
				  const struct eqc_early_match_data *early_data)
{
	struct clk_hw_onecell_data *cells;
	unsigned int i, clk_count;
	void __iomem *base;
	int ret;

	clk_count = early_data->early_pll_count + early_data->early_fixed_factor_count +
		    early_data->late_clk_count;
	cells = kzalloc(struct_size(cells, hws, clk_count), GFP_KERNEL);
	if (!cells) {
		ret = -ENOMEM;
		goto err;
	}

	cells->num = clk_count;

	/*
	 * Mark all clocks as deferred; some are registered here, the rest at
	 * platform device probe.
	 *
	 * Once the platform device is probed, its provider will take priority
	 * when looking up clocks.
	 */
	for (i = 0; i < clk_count; i++)
		cells->hws[i] = ERR_PTR(-EPROBE_DEFER);

	/* Offsets (reg) of early PLLs are relative to OLB block. */
	base = of_iomap(np, 0);
	if (!base) {
		ret = -ENODEV;
		goto err;
	}

	for (i = 0; i < early_data->early_pll_count; i++) {
		const struct eqc_pll *pll = &early_data->early_plls[i];
		unsigned long mult, div, acc;
		struct clk_hw *hw;

		ret = eqc_parse_one_pll(base + pll->reg, pll->type, &mult, &div, &acc);
		if (ret) {
			pr_err("failed parsing state of %s\n", pll->name);
			goto err;
		}

		hw = clk_hw_register_fixed_factor_with_accuracy_fwname(NULL,
				np, pll->name, "ref", 0, mult, div, acc);
		cells->hws[pll->index] = hw;
		if (IS_ERR(hw)) {
			pr_err("failed registering %s: %pe\n", pll->name, hw);
			ret = PTR_ERR(hw);
			goto err;
		}
	}

	for (i = 0; i < early_data->early_fixed_factor_count; i++) {
		const struct eqc_fixed_factor *ff = &early_data->early_fixed_factors[i];
		struct clk_hw *parent_hw = cells->hws[ff->parent_idx];
		struct clk_hw *hw;

		hw = clk_hw_register_fixed_factor_parent_hw(NULL, ff->name,
				parent_hw, 0, ff->mult, ff->div);
		cells->hws[ff->index] = hw;
		if (IS_ERR(hw)) {
			pr_err("failed registering %s: %pe\n", ff->name, hw);
			ret = PTR_ERR(hw);
			goto err;
		}
	}

	ret = of_clk_add_hw_provider(np, of_clk_hw_onecell_get, cells);
	if (ret) {
		pr_err("failed registering clk provider: %d\n", ret);
		goto err;
	}

	return;

err:
	/*
	 * We are doomed. The system will not be able to boot.
	 *
	 * Let's still try to be good citizens by freeing resources and print
	 * a last error message that might help debugging.
	 */

	pr_err("failed clk init: %d\n", ret);

	if (cells) {
		of_clk_del_provider(np);

		for (i = 0; i < early_data->early_pll_count; i++) {
			const struct eqc_pll *pll = &early_data->early_plls[i];
			struct clk_hw *hw = cells->hws[pll->index];

			if (!IS_ERR_OR_NULL(hw))
				clk_hw_unregister_fixed_factor(hw);
		}

		kfree(cells);
	}
}

static void __init eqc_eyeq5_early_init(struct device_node *np)
{
	eqc_early_init(np, &eqc_eyeq5_early_match_data);
}
CLK_OF_DECLARE_DRIVER(eqc_eyeq5, "mobileye,eyeq5-olb", eqc_eyeq5_early_init);

static void __init eqc_eyeq6h_central_early_init(struct device_node *np)
{
	eqc_early_init(np, &eqc_eyeq6h_central_early_match_data);
}
CLK_OF_DECLARE_DRIVER(eqc_eyeq6h_central, "mobileye,eyeq6h-central-olb",
		      eqc_eyeq6h_central_early_init);

static void __init eqc_eyeq6h_west_early_init(struct device_node *np)
{
	eqc_early_init(np, &eqc_eyeq6h_west_early_match_data);
}
CLK_OF_DECLARE_DRIVER(eqc_eyeq6h_west, "mobileye,eyeq6h-west-olb",
		      eqc_eyeq6h_west_early_init);
