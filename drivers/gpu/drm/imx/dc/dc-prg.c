// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright 2025 NXP
 */

#include <linux/bitfield.h>
#include <linux/bitops.h>
#include <linux/bits.h>
#include <linux/cleanup.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>

#include "dc-dprc.h"
#include "dc-prg.h"

#define SET			0x4
#define CLR			0x8
#define TOG			0xc

#define PRG_CTRL		0x00
#define  SHADOW_EN		BIT(31)
#define  SOFTRST		BIT(30)
#define  DES_DATA_TYPE_MASK	GENMASK(17, 16)
#define  DES_DATA_TYPE_32BPP	FIELD_PREP(DES_DATA_TYPE_MASK, 0)
#define  DES_DATA_TYPE_24BPP	FIELD_PREP(DES_DATA_TYPE_MASK, 1)
#define  DES_DATA_TYPE_16BPP	FIELD_PREP(DES_DATA_TYPE_MASK, 2)
#define  DES_DATA_TYPE_8BPP	FIELD_PREP(DES_DATA_TYPE_MASK, 3)
#define  SHADOW_LOAD_MODE	BIT(5)
#define  HANDSHAKE_MODE_4LINES	0
#define  SC_DATA_TYPE_8BIT	0
#define  BYPASS			BIT(0)

#define PRG_STATUS		0x10

#define PRG_REG_UPDATE		0x20
#define  REG_UPDATE		BIT(0)

#define PRG_STRIDE		0x30
#define  STRIDE(n)		FIELD_PREP(GENMASK(15, 0), (n) - 1)

#define PRG_HEIGHT		0x40
#define  HEIGHT(n)		FIELD_PREP(GENMASK(15, 0), (n) - 1)

#define PRG_BADDR		0x50
#define PRG_OFFSET		0x60

#define PRG_WIDTH		0x70
#define  WIDTH(n)		FIELD_PREP(GENMASK(15, 0), (n) - 1)

#define DPU_PRG_MAX_STRIDE	0x10000

struct dc_prg {
	struct device *dev;
	struct regmap *reg;
	struct list_head list;
	struct clk_bulk_data *clks;
	int num_clks;
	struct dc_dprc *dprc;
};

static DEFINE_MUTEX(dc_prg_list_mutex);
static LIST_HEAD(dc_prg_list);

static const struct regmap_config dc_prg_regmap_config = {
	.reg_bits = 32,
	.reg_stride = 4,
	.val_bits = 32,
	.fast_io = true,
	.max_register = PRG_WIDTH + TOG,
};

static void dc_prg_reset(struct dc_prg *prg)
{
	regmap_write(prg->reg, PRG_CTRL + SET, SOFTRST);
	fsleep(10);
	regmap_write(prg->reg, PRG_CTRL + CLR, SOFTRST);
	fsleep(10);
}

void dc_prg_enable(struct dc_prg *prg)
{
	regmap_write(prg->reg, PRG_CTRL + CLR, BYPASS);
}

void dc_prg_disable(struct dc_prg *prg)
{
	regmap_write(prg->reg, PRG_CTRL, BYPASS);

	pm_runtime_put(prg->dev);
}

void dc_prg_disable_at_boot(struct dc_prg *prg)
{
	regmap_write(prg->reg, PRG_CTRL, BYPASS);

	clk_bulk_disable_unprepare(prg->num_clks, prg->clks);
}

static unsigned int dc_prg_burst_size_fixup(dma_addr_t baddr)
{
	unsigned int burst_size;

	burst_size = 1 << __ffs(baddr);
	burst_size = round_up(burst_size, 8);
	burst_size = min(burst_size, 128U);

	return burst_size;
}

static unsigned int
dc_prg_stride_fixup(unsigned int stride, unsigned int burst_size)
{
	return round_up(stride, burst_size);
}

void dc_prg_configure(struct dc_prg *prg,
		      unsigned int width, unsigned int height,
		      unsigned int stride, unsigned int bits_per_pixel,
		      dma_addr_t baddr, bool start)
{
	struct device *dev = prg->dev;
	unsigned int burst_size;
	u32 val;
	int ret;

	if (start) {
		ret = pm_runtime_resume_and_get(dev);
		if (ret < 0) {
			dev_err(dev, "failed to get RPM: %d\n", ret);
			return;
		}
	}

	burst_size = dc_prg_burst_size_fixup(baddr);

	stride = dc_prg_stride_fixup(stride, burst_size);

	regmap_write(prg->reg, PRG_STRIDE, STRIDE(stride));
	regmap_write(prg->reg, PRG_WIDTH, WIDTH(width));
	regmap_write(prg->reg, PRG_HEIGHT, HEIGHT(height));
	regmap_write(prg->reg, PRG_OFFSET, 0);
	regmap_write(prg->reg, PRG_BADDR, baddr);

	val = SHADOW_LOAD_MODE | SC_DATA_TYPE_8BIT | BYPASS |
	      HANDSHAKE_MODE_4LINES;

	switch (bits_per_pixel) {
	case 32:
		val |= DES_DATA_TYPE_32BPP;
		break;
	case 24:
		val |= DES_DATA_TYPE_24BPP;
		break;
	case 16:
		val |= DES_DATA_TYPE_16BPP;
		break;
	case 8:
		val |= DES_DATA_TYPE_8BPP;
		break;
	}

	/* no shadow for the first frame */
	if (!start)
		val |= SHADOW_EN;
	regmap_write(prg->reg, PRG_CTRL, val);
}

void dc_prg_reg_update(struct dc_prg *prg)
{
	regmap_write(prg->reg, PRG_REG_UPDATE, REG_UPDATE);
}

void dc_prg_shadow_enable(struct dc_prg *prg)
{
	regmap_write(prg->reg, PRG_CTRL + SET, SHADOW_EN);
}

bool dc_prg_stride_supported(struct dc_prg *prg,
			     unsigned int stride, dma_addr_t baddr)
{
	unsigned int burst_size;

	burst_size = dc_prg_burst_size_fixup(baddr);

	stride = dc_prg_stride_fixup(stride, burst_size);

	if (stride > DPU_PRG_MAX_STRIDE)
		return false;

	return true;
}

struct dc_prg *
dc_prg_lookup_by_phandle(struct device *dev, const char *name, int index)
{
	struct device_node *prg_node __free(device_node);
	struct dc_prg *prg;

	prg_node = of_parse_phandle(dev->of_node, name, index);
	if (!prg_node)
		return NULL;

	guard(mutex)(&dc_prg_list_mutex);
	list_for_each_entry(prg, &dc_prg_list, list) {
		if (prg_node == prg->dev->of_node)
			return prg;
	}

	return NULL;
}

void dc_prg_set_dprc(struct dc_prg *prg, struct dc_dprc *dprc)
{
	prg->dprc = dprc;
}

struct dc_dprc *dc_prg_get_dprc(struct dc_prg *prg)
{
	return prg->dprc;
}

static int dc_prg_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct resource *res;
	void __iomem *base;
	struct dc_prg *prg;
	int ret;

	prg = devm_kzalloc(dev, sizeof(*prg), GFP_KERNEL);
	if (!prg)
		return -ENOMEM;

	base = devm_platform_get_and_ioremap_resource(pdev, 0, &res);
	if (IS_ERR(base))
		return PTR_ERR(base);

	prg->reg = devm_regmap_init_mmio(dev, base, &dc_prg_regmap_config);
	if (IS_ERR(prg->reg))
		return PTR_ERR(prg->reg);

	prg->num_clks = devm_clk_bulk_get_all(dev, &prg->clks);
	if (prg->num_clks < 0)
		return dev_err_probe(dev, prg->num_clks, "failed to get clocks\n");

	dev_set_drvdata(dev, prg);

	ret = devm_pm_runtime_enable(dev);
	if (ret)
		return dev_err_probe(dev, ret, "failed to enable PM runtime\n");

	prg->dev = dev;

	guard(mutex)(&dc_prg_list_mutex);
	list_add(&prg->list, &dc_prg_list);

	return 0;
}

static void dc_prg_remove(struct platform_device *pdev)
{
	struct dc_prg *prg = dev_get_drvdata(&pdev->dev);

	guard(mutex)(&dc_prg_list_mutex);
	list_del(&prg->list);
}

static int dc_prg_runtime_suspend(struct device *dev)
{
	struct dc_prg *prg = dev_get_drvdata(dev);

	clk_bulk_disable_unprepare(prg->num_clks, prg->clks);

	return 0;
}

static int dc_prg_runtime_resume(struct device *dev)
{
	struct dc_prg *prg = dev_get_drvdata(dev);
	int ret;

	ret = clk_bulk_prepare_enable(prg->num_clks, prg->clks);
	if (ret) {
		dev_err(dev, "failed to enable clocks: %d\n", ret);
		return ret;
	}

	dc_prg_reset(prg);

	return 0;
}

static const struct dev_pm_ops dc_prg_pm_ops = {
	RUNTIME_PM_OPS(dc_prg_runtime_suspend, dc_prg_runtime_resume, NULL)
};

static const struct of_device_id dc_prg_dt_ids[] = {
	{ .compatible = "fsl,imx8qxp-prg", },
	{ /* sentinel */ }
};

struct platform_driver dc_prg_driver = {
	.probe = dc_prg_probe,
	.remove = dc_prg_remove,
	.driver = {
		.name = "imx8-dc-prg",
		.suppress_bind_attrs = true,
		.of_match_table = dc_prg_dt_ids,
		.pm = pm_ptr(&dc_prg_pm_ops),
	},
};
