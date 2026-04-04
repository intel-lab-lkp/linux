// SPDX-License-Identifier: GPL-2.0
/*
 * Freescale SAI BCLK as a generic clock driver
 *
 * Copyright 2020 Michael Walle <michael@walle.cc>
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/err.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/slab.h>

#define I2S_CSR		0x00
#define I2S_CR2		0x08
#define I2S_MCR		0x100
#define CSR_BCE_BIT	28
#define CSR_TE_BIT	31
#define CR2_BCD		BIT(24)
#define CR2_DIV_SHIFT	0
#define CR2_DIV_WIDTH	8
#define MCR_MOE		BIT(30)

struct fsl_sai_clk {
	struct clk_divider bclk_div;
	struct clk_divider mclk_div;
	struct clk_gate bclk_gate;
	struct clk_gate mclk_gate;
	struct clk_hw *bclk_hw;
	struct clk_hw *mclk_hw;
	spinlock_t lock;
};

struct fsl_sai_data {
	unsigned int	offset;	/* Register offset */
	bool		have_mclk; /* Have MCLK control */
};

static struct clk_hw *
fsl_sai_of_clk_get(struct of_phandle_args *clkspec, void *data)
{
	struct fsl_sai_clk *sai_clk = data;

	return clkspec->args[0] ? sai_clk->mclk_hw : sai_clk->bclk_hw;
}

static int fsl_sai_clk_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	const struct fsl_sai_data *data = device_get_match_data(dev);
	struct fsl_sai_clk *sai_clk;
	struct clk_parent_data pdata = { .index = 0 };
	struct clk *clk_bus;
	void __iomem *base;
	struct clk_hw *hw;

	sai_clk = devm_kzalloc(dev, sizeof(*sai_clk), GFP_KERNEL);
	if (!sai_clk)
		return -ENOMEM;

	base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(base))
		return PTR_ERR(base);

	clk_bus = devm_clk_get_optional_enabled(dev, "bus");
	if (IS_ERR(clk_bus))
		return PTR_ERR(clk_bus);

	spin_lock_init(&sai_clk->lock);

	sai_clk->bclk_gate.reg = base + data->offset + I2S_CSR;
	sai_clk->bclk_gate.bit_idx = CSR_BCE_BIT;
	sai_clk->bclk_gate.lock = &sai_clk->lock;

	sai_clk->bclk_div.reg = base + data->offset + I2S_CR2;
	sai_clk->bclk_div.shift = CR2_DIV_SHIFT;
	sai_clk->bclk_div.width = CR2_DIV_WIDTH;
	sai_clk->bclk_div.lock = &sai_clk->lock;

	/* set clock direction, we are the BCLK master */
	writel(CR2_BCD, base + data->offset + I2S_CR2);

	hw = devm_clk_hw_register_composite_pdata(dev, "BCLK",
						  &pdata, 1, NULL, NULL,
						  &sai_clk->bclk_div.hw,
						  &clk_divider_ops,
						  &sai_clk->bclk_gate.hw,
						  &clk_gate_ops,
						  CLK_SET_RATE_GATE);
	if (IS_ERR(hw))
		return PTR_ERR(hw);

	sai_clk->bclk_hw = hw;

	if (data->have_mclk) {
		sai_clk->mclk_gate.reg = base + data->offset + I2S_CSR;
		sai_clk->mclk_gate.bit_idx = CSR_TE_BIT;
		sai_clk->mclk_gate.lock = &sai_clk->lock;

		sai_clk->mclk_div.reg = base + I2S_MCR;
		sai_clk->mclk_div.shift = CR2_DIV_SHIFT;
		sai_clk->mclk_div.width = CR2_DIV_WIDTH;
		sai_clk->mclk_div.lock = &sai_clk->lock;

		pdata.index = 1; /* MCLK1 */
		hw = devm_clk_hw_register_composite_pdata(dev, "MCLK",
							  &pdata, 1, NULL, NULL,
							  &sai_clk->mclk_div.hw,
							  &clk_divider_ops,
							  &sai_clk->mclk_gate.hw,
							  &clk_gate_ops,
							  CLK_SET_RATE_GATE);
		if (IS_ERR(hw))
			return PTR_ERR(hw);

		sai_clk->mclk_hw = hw;

		/* set clock direction, we are the MCLK output */
		writel(MCR_MOE, base + I2S_MCR);
	}

	return devm_of_clk_add_hw_provider(dev, fsl_sai_of_clk_get, sai_clk);
}

static const struct fsl_sai_data fsl_sai_vf610_data = {
	.offset	= 0,
	.have_mclk = false,
};

static const struct fsl_sai_data fsl_sai_imx8mq_data = {
	.offset	= 8,
	.have_mclk = true,
};

static const struct of_device_id of_fsl_sai_clk_ids[] = {
	{ .compatible = "fsl,vf610-sai-clock", .data = &fsl_sai_vf610_data },
	{ .compatible = "fsl,imx8mq-sai-clock", .data = &fsl_sai_imx8mq_data },
	{ }
};
MODULE_DEVICE_TABLE(of, of_fsl_sai_clk_ids);

static struct platform_driver fsl_sai_clk_driver = {
	.probe = fsl_sai_clk_probe,
	.driver		= {
		.name	= "fsl-sai-clk",
		.of_match_table = of_fsl_sai_clk_ids,
	},
};
module_platform_driver(fsl_sai_clk_driver);

MODULE_DESCRIPTION("Freescale SAI bitclock-as-a-clock driver");
MODULE_AUTHOR("Michael Walle <michael@walle.cc>");
MODULE_ALIAS("platform:fsl-sai-clk");
