// SPDX-License-Identifier: GPL-2.0
/*
 * SpacemiT K1 PCIe host driver
 *
 * Copyright (C) 2025 by RISCstar Solutions Corporation.  All rights reserved.
 * Copyright (c) 2023, spacemit Corporation.
 */

#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/gfp.h>
#include <linux/irq.h>
#include <linux/mfd/syscon.h>
#include <linux/mod_devicetable.h>
#include <linux/of.h>
#include <linux/pci.h>
#include <linux/phy/phy.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/reset.h>
#include <linux/types.h>

#include "pcie-designware.h"

#define K1_PCIE_VENDOR_ID	0x201f
#define K1_PCIE_DEVICE_ID	0x0001

/* Offsets and field definitions of link management registers */

#define K1_PHY_AHB_IRQ_EN			0x0000
#define PCIE_INTERRUPT_EN		BIT(0)

#define K1_PHY_AHB_LINK_STS			0x0004
#define SMLH_LINK_UP			BIT(1)
#define RDLH_LINK_UP			BIT(12)

#define INTR_ENABLE				0x0014
#define MSI_CTRL_INT			BIT(11)

/* Offsets and field definitions for PMU registers */

#define PCIE_CLK_RESET_CONTROL			0x0000
#define LTSSM_EN			BIT(6)
#define PCIE_AUX_PWR_DET		BIT(9)
#define PCIE_RC_PERST			BIT(12)	/* 0: PERST# high; 1: low */
#define APP_HOLD_PHY_RST		BIT(30)
#define DEVICE_TYPE_RC			BIT(31)	/* 0: endpoint; 1: RC */

#define PCIE_CONTROL_LOGIC			0x0004
#define PCIE_SOFT_RESET			BIT(0)

struct k1_pcie {
	struct dw_pcie pci;
	void __iomem *link;
	struct regmap *pmu;
	u32 pmu_off;
	struct phy *phy;
	struct reset_control *global_reset;
};

#define to_k1_pcie(dw_pcie)	dev_get_drvdata((dw_pcie)->dev)

static int k1_pcie_toggle_soft_reset(struct k1_pcie *k1)
{
	u32 offset = k1->pmu_off + PCIE_CONTROL_LOGIC;
	const u32 mask = PCIE_SOFT_RESET;
	int ret;

	ret = regmap_set_bits(k1->pmu, offset, mask);
	if (ret)
		return ret;

	mdelay(2);

	return regmap_clear_bits(k1->pmu, offset, mask);
}

/* Enable app clocks, deassert app resets */
static int k1_pcie_app_enable(struct k1_pcie *k1)
{
	struct dw_pcie *pci = &k1->pci;
	u32 clock_count;
	u32 reset_count;
	int ret;

	clock_count = ARRAY_SIZE(pci->app_clks);
	ret = clk_bulk_prepare_enable(clock_count, pci->app_clks);
	if (ret)
		return ret;

	reset_count = ARRAY_SIZE(pci->app_rsts);
	ret = reset_control_bulk_deassert(reset_count, pci->app_rsts);
	if (ret)
		goto err_disable_clks;

	ret = reset_control_deassert(k1->global_reset);
	if (ret)
		goto err_assert_resets;

	return 0;

err_assert_resets:
	(void)reset_control_bulk_assert(reset_count, pci->app_rsts);
err_disable_clks:
	clk_bulk_disable_unprepare(clock_count, pci->app_clks);

	return ret;
}

/* Disable app clocks, assert app resets */
static void k1_pcie_app_disable(struct k1_pcie *k1)
{
	struct dw_pcie *pci = &k1->pci;
	u32 count;
	int ret;

	(void)reset_control_assert(k1->global_reset);

	count = ARRAY_SIZE(pci->app_rsts);
	ret = reset_control_bulk_assert(count, pci->app_rsts);
	if (ret)
		dev_err(pci->dev, "app reset assert failed (%d)\n", ret);

	count = ARRAY_SIZE(pci->app_clks);
	clk_bulk_disable_unprepare(count, pci->app_clks);
}

static int k1_pcie_init(struct dw_pcie_rp *pp)
{
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);
	struct k1_pcie *k1 = to_k1_pcie(pci);
	u32 offset;
	u32 mask;
	int ret;

	ret = k1_pcie_toggle_soft_reset(k1);
	if (ret)
		goto err_app_disable;

	ret = k1_pcie_app_enable(k1);
	if (ret)
		return ret;

	ret = phy_init(k1->phy);
	if (ret)
		goto err_app_disable;

	/* Set the PCI vendor and device ID */
	dw_pcie_dbi_ro_wr_en(pci);
	dw_pcie_writew_dbi(pci, PCI_VENDOR_ID, K1_PCIE_VENDOR_ID);
	dw_pcie_writew_dbi(pci, PCI_DEVICE_ID, K1_PCIE_DEVICE_ID);
	dw_pcie_dbi_ro_wr_dis(pci);

	/*
	 * Put the port in root complex mode, record that Vaux is present.
	 * Assert fundamental reset (drive PERST# low).
	 */
	offset = k1->pmu_off + PCIE_CLK_RESET_CONTROL;
	mask = DEVICE_TYPE_RC | PCIE_AUX_PWR_DET;
	mask |= PCIE_RC_PERST;
	ret = regmap_set_bits(k1->pmu, offset, mask);
	if (ret)
		goto err_phy_exit;

	/* Wait the PCIe-mandated 100 msec before deasserting PERST# */
	mdelay(100);

	ret = regmap_clear_bits(k1->pmu, offset, PCIE_RC_PERST);
	if (!ret)
		return 0;	/* Success! */

err_phy_exit:
	(void)phy_exit(k1->phy);
err_app_disable:
	k1_pcie_app_disable(k1);

	return ret;
}

/* Silently ignore any errors */
static void k1_pcie_deinit(struct dw_pcie_rp *pp)
{
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);
	struct k1_pcie *k1 = to_k1_pcie(pci);

	/* Re-assert fundamental reset (drive PERST# low) */
	(void)regmap_set_bits(k1->pmu, k1->pmu_off + PCIE_CLK_RESET_CONTROL,
			      PCIE_RC_PERST);

	(void)phy_exit(k1->phy);

	k1_pcie_app_disable(k1);
}

static const struct dw_pcie_host_ops k1_pcie_host_ops = {
	.init		= k1_pcie_init,
	.deinit		= k1_pcie_deinit,
};

static void k1_pcie_enable_interrupts(struct k1_pcie *k1)
{
	void __iomem *virt;
	u32 val;

	/* Enable the MSI interrupt */
	writel(MSI_CTRL_INT, k1->link + INTR_ENABLE);

	/* Top-level interrupt enable */
	virt = k1->link + K1_PHY_AHB_IRQ_EN;
	val = readl(virt);
	val |= PCIE_INTERRUPT_EN;
	writel(val, virt);
}

static void k1_pcie_disable_interrupts(struct k1_pcie *k1)
{
	void __iomem *virt;
	u32 val;

	virt = k1->link + K1_PHY_AHB_IRQ_EN;
	val = readl(virt);
	val &= ~PCIE_INTERRUPT_EN;
	writel(val, virt);

	writel(0, k1->link + INTR_ENABLE);
}

static bool k1_pcie_link_up(struct dw_pcie *pci)
{
	struct k1_pcie *k1 = to_k1_pcie(pci);
	u32 val;

	val = readl(k1->link + K1_PHY_AHB_LINK_STS);

	return (val & RDLH_LINK_UP) && (val & SMLH_LINK_UP);
}

static int k1_pcie_start_link(struct dw_pcie *pci)
{
	struct k1_pcie *k1 = to_k1_pcie(pci);
	int ret;

	/* Stop holding the PHY in reset, and enable link training */
	ret = regmap_update_bits(k1->pmu, k1->pmu_off + PCIE_CLK_RESET_CONTROL,
				 APP_HOLD_PHY_RST | LTSSM_EN, LTSSM_EN);
	if (ret)
		return ret;

	k1_pcie_enable_interrupts(k1);

	return 0;
}

static void k1_pcie_stop_link(struct dw_pcie *pci)
{
	struct k1_pcie *k1 = to_k1_pcie(pci);
	int ret;

	k1_pcie_disable_interrupts(k1);

	/* Disable the link and hold the PHY in reset */
	ret = regmap_update_bits(k1->pmu, k1->pmu_off + PCIE_CLK_RESET_CONTROL,
				 APP_HOLD_PHY_RST | LTSSM_EN, APP_HOLD_PHY_RST);
	if (ret)
		dev_err(pci->dev, "disable LTSSM failed (%d)\n", ret);
}

static const struct dw_pcie_ops k1_pcie_ops = {
	.link_up	= k1_pcie_link_up,
	.start_link	= k1_pcie_start_link,
	.stop_link	= k1_pcie_stop_link,
};

static int k1_pcie_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct dw_pcie_rp *pp;
	struct dw_pcie *pci;
	struct k1_pcie *k1;
	int ret;

	k1 = devm_kzalloc(dev, sizeof(*k1), GFP_KERNEL);
	if (!k1)
		return -ENOMEM;
	dev_set_drvdata(dev, k1);

	k1->pmu = syscon_regmap_lookup_by_phandle_args(dev_of_node(dev),
						       "spacemit,syscon-pmu",
						       1, &k1->pmu_off);
	if (IS_ERR(k1->pmu))
		return dev_err_probe(dev, PTR_ERR(k1->pmu),
				     "lookup PMU regmap failed\n");

	k1->link = devm_platform_ioremap_resource_byname(pdev, "link");
	if (!k1->link)
		return dev_err_probe(dev, -ENOMEM, "map link regs failed\n");

	k1->global_reset = devm_reset_control_get_shared(dev, "global");
	if (IS_ERR(k1->global_reset))
		return dev_err_probe(dev, PTR_ERR(k1->global_reset),
				     "get global reset failed\n");

	/* Hold the PHY in reset until we start the link */
	ret = regmap_set_bits(k1->pmu, k1->pmu_off + PCIE_CLK_RESET_CONTROL,
			      APP_HOLD_PHY_RST);
	if (ret)
		return dev_err_probe(dev, ret, "hold PHY in reset failed\n");

	k1->phy = devm_phy_get(dev, NULL);
	if (IS_ERR(k1->phy))
		return dev_err_probe(dev, PTR_ERR(k1->phy), "get PHY failed\n");

	pci = &k1->pci;
	dw_pcie_cap_set(pci, REQ_RES);
	pci->dev = dev;
	pci->ops = &k1_pcie_ops;

	pp = &pci->pp;
	pp->num_vectors = MAX_MSI_IRQS;
	pp->ops = &k1_pcie_host_ops;

	ret = dw_pcie_host_init(pp);
	if (ret)
		return dev_err_probe(dev, ret, "host init failed\n");

	return 0;
}

static void k1_pcie_remove(struct platform_device *pdev)
{
	struct k1_pcie *k1 = dev_get_drvdata(&pdev->dev);
	struct dw_pcie_rp *pp = &k1->pci.pp;

	dw_pcie_host_deinit(pp);
}

static const struct of_device_id k1_pcie_of_match_table[] = {
	{ .compatible = "spacemit,k1-pcie-rc", },
	{ },
};

static struct platform_driver k1_pcie_driver = {
	.probe	= k1_pcie_probe,
	.remove	= k1_pcie_remove,
	.driver = {
		.name			= "k1-dwc-pcie",
		.of_match_table		= k1_pcie_of_match_table,
		.suppress_bind_attrs	= true,
	},
};
module_platform_driver(k1_pcie_driver);
