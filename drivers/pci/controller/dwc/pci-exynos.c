// SPDX-License-Identifier: GPL-2.0
/*
 * PCIe host controller driver for Samsung Exynos SoCs
 *
 * Copyright (C) 2013-2020 Samsung Electronics Co., Ltd.
 *		https://www.samsung.com
 *
 * Author: Jingoo Han <jg1.han@samsung.com>
 *	   Jaehoon Chung <jh80.chung@samsung.com>
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/pci.h>
#include <linux/platform_device.h>
#include <linux/phy/phy.h>
#include <linux/regulator/consumer.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/regmap.h>
#include <linux/mfd/syscon.h>

#include "pcie-designware.h"

#define to_exynos_pcie(x)	dev_get_drvdata((x)->dev)

/* PCIe ELBI registers */
#define EXYNOS_PCIE_IRQ_PULSE			0x000
#define EXYNOS_PCIE_IRQ_EN_PULSE		0x00c
#define EXYNOS_IRQ_INTA_ASSERT			BIT(0)
#define EXYNOS_IRQ_INTB_ASSERT			BIT(2)
#define EXYNOS_IRQ_INTC_ASSERT			BIT(4)
#define EXYNOS_IRQ_INTD_ASSERT			BIT(6)
#define EXYNOS_PCIE_IRQ_EN_LEVEL		0x010
#define EXYNOS_PCIE_IRQ_EN_SPECIAL		0x014
#define EXYNOS_PCIE_SW_WAKE			0x018
#define EXYNOS_PCIE_BUS_EN			BIT(1)
#define EXYNOS_PCIE_CORE_RESET			0x01c
#define EXYNOS_PCIE_CORE_RESET_ENABLE		BIT(0)
#define EXYNOS_PCIE_STICKY_RESET		0x020
#define EXYNOS_PCIE_NONSTICKY_RESET		0x024
#define EXYNOS_PCIE_APP_INIT_RESET		0x028
#define EXYNOS_PCIE_APP_LTSSM_ENABLE		0x02c
#define EXYNOS_PCIE_ELBI_LTSSM_ENABLE		0x1
#define EXYNOS_PCIE_ELBI_RDLH_LINKUP		0x074
#define EXYNOS_PCIE_ELBI_XMLH_LINKUP		BIT(4)
#define EXYNOS_PCIE_ELBI_SLV_AWMISC		0x11c
#define EXYNOS_PCIE_ELBI_SLV_ARMISC		0x120
#define EXYNOS_PCIE_ELBI_SLV_DBI_ENABLE		BIT(21)

#define FSD_IRQ2_STS				0x008
#define FSD_IRQ_MSI_ENABLE			BIT(17)
#define FSD_IRQ2_EN				0x018
#define FSD_PCIE_APP_LTSSM_ENABLE		0x054
#define FSD_PCIE_LTSSM_ENABLE			0x1
#define FSD_PCIE_DEVICE_TYPE			0x080
#define FSD_DEVICE_TYPE_RC			0x4
#define FSD_DEVICE_TYPE_EP			0x0
#define FSD_PCIE_CXPL_DEBUG_00_31		0x2C8

/* to store different SoC variants of Samsung */
enum samsung_pcie_variants {
	FSD,
	EXYNOS_5433,
};

/* Values to be written to SYSREG to view DBI space as CDM/DBI2/IATU/DMA */
enum fsd_pcie_addr_type {
	ADDR_TYPE_DBI = 0x0,
	ADDR_TYPE_DBI2 = 0x12,
	ADDR_TYPE_ATU = 0x36,
	ADDR_TYPE_DMA = 0x3f,
};

struct samsung_pcie_pdata {
	struct pci_ops				*pci_ops;
	const struct dw_pcie_ops		*dwc_ops;
	const struct dw_pcie_host_ops		*host_ops;
	const struct dw_pcie_ep_ops		*ep_ops;
	const struct samsung_res_ops		*res_ops;
	unsigned int				soc_variant;
	enum dw_pcie_device_mode		device_mode;
};

struct exynos_pcie {
	struct dw_pcie			pci;
	void __iomem			*elbi_base;
	const struct samsung_pcie_pdata	*pdata;
	struct regmap			*sysreg;
	unsigned int			sysreg_offset;
	struct clk_bulk_data		*clks;
	struct phy			*phy;
	struct regulator_bulk_data	*supplies;
	int				supplies_cnt;
};

struct samsung_res_ops {
	int (*init_regulator)(struct exynos_pcie *ep);
	irqreturn_t (*pcie_irq_handler)(int irq, void *arg);
	void (*set_device_mode)(struct exynos_pcie *ep);
};

static void exynos_pcie_writel(void __iomem *base, u32 val, u32 reg)
{
	writel(val, base + reg);
}

static u32 exynos_pcie_readl(void __iomem *base, u32 reg)
{
	return readl(base + reg);
}

static int samsung_regulator_enable(struct exynos_pcie *ep)
{
	struct device *dev = ep->pci.dev;
	int ret;

	if (ep->supplies_cnt == 0)
		return 0;

	ret = devm_regulator_bulk_get(dev, ep->supplies_cnt, ep->supplies);
	if (ret)
		return ret;

	ret = regulator_bulk_enable(ep->supplies_cnt, ep->supplies);

	return ret;
}

static void samsung_regulator_disable(struct exynos_pcie *ep)
{
	struct device *dev = ep->pci.dev;
	int ret;

	if (ep->supplies_cnt == 0)
		return;

	ret = regulator_bulk_disable(ep->supplies_cnt, ep->supplies);
	if (ret)
		dev_warn(dev, "failed to disable regulators: %d\n", ret);
}

static void exynos_pcie_sideband_dbi_w_mode(struct exynos_pcie *ep, bool on)
{
	u32 val;

	val = exynos_pcie_readl(ep->elbi_base, EXYNOS_PCIE_ELBI_SLV_AWMISC);
	if (on)
		val |= EXYNOS_PCIE_ELBI_SLV_DBI_ENABLE;
	else
		val &= ~EXYNOS_PCIE_ELBI_SLV_DBI_ENABLE;
	exynos_pcie_writel(ep->elbi_base, val, EXYNOS_PCIE_ELBI_SLV_AWMISC);
}

static void exynos_pcie_sideband_dbi_r_mode(struct exynos_pcie *ep, bool on)
{
	u32 val;

	val = exynos_pcie_readl(ep->elbi_base, EXYNOS_PCIE_ELBI_SLV_ARMISC);
	if (on)
		val |= EXYNOS_PCIE_ELBI_SLV_DBI_ENABLE;
	else
		val &= ~EXYNOS_PCIE_ELBI_SLV_DBI_ENABLE;
	exynos_pcie_writel(ep->elbi_base, val, EXYNOS_PCIE_ELBI_SLV_ARMISC);
}

static void exynos_pcie_assert_core_reset(struct exynos_pcie *ep)
{
	u32 val;

	val = exynos_pcie_readl(ep->elbi_base, EXYNOS_PCIE_CORE_RESET);
	val &= ~EXYNOS_PCIE_CORE_RESET_ENABLE;
	exynos_pcie_writel(ep->elbi_base, val, EXYNOS_PCIE_CORE_RESET);
	exynos_pcie_writel(ep->elbi_base, 0, EXYNOS_PCIE_STICKY_RESET);
	exynos_pcie_writel(ep->elbi_base, 0, EXYNOS_PCIE_NONSTICKY_RESET);
}

static void exynos_pcie_deassert_core_reset(struct exynos_pcie *ep)
{
	u32 val;

	val = exynos_pcie_readl(ep->elbi_base, EXYNOS_PCIE_CORE_RESET);
	val |= EXYNOS_PCIE_CORE_RESET_ENABLE;

	exynos_pcie_writel(ep->elbi_base, val, EXYNOS_PCIE_CORE_RESET);
	exynos_pcie_writel(ep->elbi_base, 1, EXYNOS_PCIE_STICKY_RESET);
	exynos_pcie_writel(ep->elbi_base, 1, EXYNOS_PCIE_NONSTICKY_RESET);
	exynos_pcie_writel(ep->elbi_base, 1, EXYNOS_PCIE_APP_INIT_RESET);
	exynos_pcie_writel(ep->elbi_base, 0, EXYNOS_PCIE_APP_INIT_RESET);
}

static int exynos_pcie_start_link(struct dw_pcie *pci)
{
	struct exynos_pcie *ep = to_exynos_pcie(pci);
	u32 val;

	val = exynos_pcie_readl(ep->elbi_base, EXYNOS_PCIE_SW_WAKE);
	val &= ~EXYNOS_PCIE_BUS_EN;
	exynos_pcie_writel(ep->elbi_base, val, EXYNOS_PCIE_SW_WAKE);

	/* assert LTSSM enable */
	exynos_pcie_writel(ep->elbi_base, EXYNOS_PCIE_ELBI_LTSSM_ENABLE,
			  EXYNOS_PCIE_APP_LTSSM_ENABLE);
	return 0;
}

static void exynos_pcie_clear_irq_pulse(struct exynos_pcie *ep)
{
	u32 val = exynos_pcie_readl(ep->elbi_base, EXYNOS_PCIE_IRQ_PULSE);

	exynos_pcie_writel(ep->elbi_base, val, EXYNOS_PCIE_IRQ_PULSE);
}

static irqreturn_t exynos_pcie_irq_handler(int irq, void *arg)
{
	struct exynos_pcie *ep = arg;

	exynos_pcie_clear_irq_pulse(ep);
	return IRQ_HANDLED;
}

static void exynos_pcie_enable_irq_pulse(struct exynos_pcie *ep)
{
	u32 val = EXYNOS_IRQ_INTA_ASSERT | EXYNOS_IRQ_INTB_ASSERT |
		  EXYNOS_IRQ_INTC_ASSERT | EXYNOS_IRQ_INTD_ASSERT;

	exynos_pcie_writel(ep->elbi_base, val, EXYNOS_PCIE_IRQ_EN_PULSE);
	exynos_pcie_writel(ep->elbi_base, 0, EXYNOS_PCIE_IRQ_EN_LEVEL);
	exynos_pcie_writel(ep->elbi_base, 0, EXYNOS_PCIE_IRQ_EN_SPECIAL);
}

static u32 exynos_pcie_read_dbi(struct dw_pcie *pci, void __iomem *base,
				u32 reg, size_t size)
{
	struct exynos_pcie *ep = to_exynos_pcie(pci);
	u32 val;

	exynos_pcie_sideband_dbi_r_mode(ep, true);
	dw_pcie_read(base + reg, size, &val);
	exynos_pcie_sideband_dbi_r_mode(ep, false);
	return val;
}

static void exynos_pcie_write_dbi(struct dw_pcie *pci, void __iomem *base,
				  u32 reg, size_t size, u32 val)
{
	struct exynos_pcie *ep = to_exynos_pcie(pci);

	exynos_pcie_sideband_dbi_w_mode(ep, true);
	dw_pcie_write(base + reg, size, val);
	exynos_pcie_sideband_dbi_w_mode(ep, false);
}

static int exynos_pcie_rd_own_conf(struct pci_bus *bus, unsigned int devfn,
				   int where, int size, u32 *val)
{
	struct dw_pcie *pci = to_dw_pcie_from_pp(bus->sysdata);

	if (PCI_SLOT(devfn))
		return PCIBIOS_DEVICE_NOT_FOUND;

	*val = dw_pcie_read_dbi(pci, where, size);
	return PCIBIOS_SUCCESSFUL;
}

static int exynos_pcie_wr_own_conf(struct pci_bus *bus, unsigned int devfn,
				   int where, int size, u32 val)
{
	struct dw_pcie *pci = to_dw_pcie_from_pp(bus->sysdata);

	if (PCI_SLOT(devfn))
		return PCIBIOS_DEVICE_NOT_FOUND;

	dw_pcie_write_dbi(pci, where, size, val);
	return PCIBIOS_SUCCESSFUL;
}

static struct pci_ops exynos_pci_ops = {
	.read = exynos_pcie_rd_own_conf,
	.write = exynos_pcie_wr_own_conf,
};

static int exynos_pcie_link_up(struct dw_pcie *pci)
{
	struct exynos_pcie *ep = to_exynos_pcie(pci);
	u32 val = exynos_pcie_readl(ep->elbi_base, EXYNOS_PCIE_ELBI_RDLH_LINKUP);

	return (val & EXYNOS_PCIE_ELBI_XMLH_LINKUP);
}

static int exynos_pcie_host_init(struct dw_pcie_rp *pp)
{
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);
	struct exynos_pcie *ep = to_exynos_pcie(pci);

	pp->bridge->ops = ep->pdata->pci_ops;

	exynos_pcie_assert_core_reset(ep);

	phy_init(ep->phy);
	phy_power_on(ep->phy);

	exynos_pcie_deassert_core_reset(ep);
	exynos_pcie_enable_irq_pulse(ep);

	return 0;
}

static const struct dw_pcie_host_ops exynos_pcie_host_ops = {
	.init = exynos_pcie_host_init,
};

static int exynos_init_regulator(struct exynos_pcie *ep)
{
	struct device *dev = ep->pci.dev;

	ep->supplies_cnt = 2;

	ep->supplies = devm_kcalloc(dev, ep->supplies_cnt, sizeof(*ep->supplies), GFP_KERNEL);
	if (!ep->supplies)
		return -ENOMEM;

	ep->supplies[0].supply = "vdd18";
	ep->supplies[1].supply = "vdd10";

	return 0;
}

static int samsung_irq_init(struct exynos_pcie *ep,
				       struct platform_device *pdev)
{
	struct dw_pcie *pci = &ep->pci;
	struct dw_pcie_rp *pp = &pci->pp;
	struct device *dev = &pdev->dev;
	int ret;

	pp->irq = platform_get_irq(pdev, 0);
	if (pp->irq < 0)
		return pp->irq;

	ret = devm_request_irq(dev, pp->irq, ep->pdata->res_ops->pcie_irq_handler,
			       IRQF_SHARED, "exynos-pcie", ep);
	if (ret) {
		dev_err(dev, "failed to request irq\n");
		return ret;
	}

	pp->msi_irq[0] = -ENODEV;

	return 0;
}

static const struct dw_pcie_ops exynos_dw_pcie_ops = {
	.read_dbi = exynos_pcie_read_dbi,
	.write_dbi = exynos_pcie_write_dbi,
	.link_up = exynos_pcie_link_up,
	.start_link = exynos_pcie_start_link,
};

static void fsd_pcie_stop_link(struct dw_pcie *pci)
{
	u32 val;
	struct exynos_pcie *ep = to_exynos_pcie(pci);

	val = readl(ep->elbi_base + FSD_PCIE_APP_LTSSM_ENABLE);
	val &= ~FSD_PCIE_LTSSM_ENABLE;
	writel(val, ep->elbi_base + FSD_PCIE_APP_LTSSM_ENABLE);
}

static int fsd_pcie_start_link(struct dw_pcie *pci)
{
	struct exynos_pcie *ep = to_exynos_pcie(pci);
	struct dw_pcie_ep *dw_ep = &pci->ep;

	if (dw_pcie_link_up(pci))
		return 0;

	writel(FSD_PCIE_LTSSM_ENABLE, ep->elbi_base + FSD_PCIE_APP_LTSSM_ENABLE);

	/* no need to wait for link in case of host as core files take care */
	if (ep->pdata->device_mode == DW_PCIE_RC_TYPE)
		return 0;

	/* check if the link is up or not in case of EP */
	if (!dw_pcie_wait_for_link(pci)) {
		dw_pcie_ep_linkup(dw_ep);
		return 0;
	}

	return -ETIMEDOUT;
}

static irqreturn_t fsd_pcie_irq_handler(int irq, void *arg)
{
	u32 val;
	struct exynos_pcie *ep = arg;
	struct dw_pcie *pci = &ep->pci;
	struct dw_pcie_rp *pp = &pci->pp;

	val = readl(ep->elbi_base + FSD_IRQ2_STS);
	if ((val & FSD_IRQ_MSI_ENABLE) == FSD_IRQ_MSI_ENABLE) {
		val &= FSD_IRQ_MSI_ENABLE;
		writel(val, ep->elbi_base + FSD_IRQ2_STS);
		dw_handle_msi_irq(pp);
	}

	return IRQ_HANDLED;
}

static void fsd_pcie_msi_init(struct exynos_pcie *ep)
{
	int val;

	val = readl(ep->elbi_base + FSD_IRQ2_EN);
	val |= FSD_IRQ_MSI_ENABLE;
	writel(val, ep->elbi_base + FSD_IRQ2_EN);
}

static void __iomem *fsd_atu_setting(struct dw_pcie *pci, void __iomem *base)
{
	struct exynos_pcie *ep = to_exynos_pcie(pci);

	if (base >= pci->atu_base) {
		regmap_write(ep->sysreg, ep->sysreg_offset, ADDR_TYPE_ATU);
		return (base - DEFAULT_DBI_ATU_OFFSET);
	} else if (base == pci->dbi_base) {
		regmap_write(ep->sysreg, ep->sysreg_offset, ADDR_TYPE_DBI);
	} else if (base == pci->dbi_base2) {
		regmap_write(ep->sysreg, ep->sysreg_offset, ADDR_TYPE_DBI2);
	}

	return base;
}

static u32 fsd_pcie_read_dbi(struct dw_pcie *pci, void __iomem *base,
				u32 reg, size_t size)
{
	void __iomem *addr;
	u32 val;

	addr = fsd_atu_setting(pci, base);

	dw_pcie_read(addr + reg, size, &val);

	return val;
}

static void fsd_pcie_write_dbi(struct dw_pcie *pci, void __iomem *base,
				u32 reg, size_t size, u32 val)
{
	void __iomem *addr;

	addr = fsd_atu_setting(pci, base);

	dw_pcie_write(addr + reg, size, val);
}

static void fsd_pcie_write_dbi2(struct dw_pcie *pci, void __iomem *base,
				u32 reg, size_t size, u32 val)
{
	struct exynos_pcie *ep = to_exynos_pcie(pci);

	fsd_atu_setting(pci, base);
	dw_pcie_write(pci->dbi_base + reg, size, val);
	regmap_write(ep->sysreg, ep->sysreg_offset, ADDR_TYPE_DBI);
}

static int fsd_pcie_link_up(struct dw_pcie *pci)
{
	u32 val;
	struct exynos_pcie *ep = to_exynos_pcie(pci);

	val = readl(ep->elbi_base + FSD_PCIE_CXPL_DEBUG_00_31);
	val &= PORT_LOGIC_LTSSM_STATE_MASK;

	return (val == PORT_LOGIC_LTSSM_STATE_L0);
}

static int fsd_pcie_host_init(struct dw_pcie_rp *pp)
{
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);
	struct exynos_pcie *ep = to_exynos_pcie(pci);

	phy_init(ep->phy);
	fsd_pcie_msi_init(ep);

	return 0;
}

static const struct dw_pcie_host_ops fsd_pcie_host_ops = {
	.init = fsd_pcie_host_init,
};

static int fsd_pcie_raise_irq(struct dw_pcie_ep *ep, u8 func_no,
				 unsigned int type, u16 interrupt_num)
{
	struct dw_pcie *pci = to_dw_pcie_from_ep(ep);

	switch (type) {
	case PCI_IRQ_INTX:
	case PCI_IRQ_MSIX:
		dev_err(pci->dev, "EP does not support legacy IRQs\n");
		return -EINVAL;
	case PCI_IRQ_MSI:
		return dw_pcie_ep_raise_msi_irq(ep, func_no, interrupt_num);
	default:
		dev_err(pci->dev, "UNKNOWN IRQ type\n");
	}

	return 0;
}

static const struct pci_epc_features fsd_pcie_epc_features = {
	.linkup_notifier = false,
	.msi_capable = true,
	.msix_capable = false,
};

static const struct pci_epc_features *fsd_pcie_get_features(struct dw_pcie_ep *ep)
{
	return &fsd_pcie_epc_features;
}

static const struct dw_pcie_ep_ops fsd_ep_ops = {
	.raise_irq	= fsd_pcie_raise_irq,
	.get_features	= fsd_pcie_get_features,
};

static void fsd_set_device_mode(struct exynos_pcie *ep)
{
	if (ep->pdata->device_mode == DW_PCIE_RC_TYPE)
		writel(FSD_DEVICE_TYPE_RC, ep->elbi_base + FSD_PCIE_DEVICE_TYPE);
	else
		writel(FSD_DEVICE_TYPE_EP, ep->elbi_base + FSD_PCIE_DEVICE_TYPE);
}

static const struct dw_pcie_ops fsd_dw_pcie_ops = {
	.read_dbi	= fsd_pcie_read_dbi,
	.write_dbi	= fsd_pcie_write_dbi,
	.write_dbi2	= fsd_pcie_write_dbi2,
	.start_link	= fsd_pcie_start_link,
	.stop_link	= fsd_pcie_stop_link,
	.link_up	= fsd_pcie_link_up,
};

static const struct samsung_res_ops exynos_res_ops_data = {
	.init_regulator		= exynos_init_regulator,
	.pcie_irq_handler	= exynos_pcie_irq_handler,
};

static const struct samsung_res_ops fsd_res_ops_data = {
	.pcie_irq_handler	= fsd_pcie_irq_handler,
	.set_device_mode	= fsd_set_device_mode,
};

static int exynos_pcie_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct exynos_pcie *ep;
	const struct samsung_pcie_pdata *pdata;
	struct device_node *np = dev->of_node;
	int ret;

	ep = devm_kzalloc(dev, sizeof(*ep), GFP_KERNEL);
	if (!ep)
		return -ENOMEM;

	pdata = of_device_get_match_data(dev);

	ep->pdata = pdata;
	ep->pci.dev = dev;
	ep->pci.ops = pdata->dwc_ops;

	ep->phy = devm_of_phy_get(dev, np, NULL);
	if (IS_ERR(ep->phy))
		return PTR_ERR(ep->phy);

	if (ep->pdata->soc_variant == FSD) {
		ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(36));
		if (ret)
			return ret;

		ep->sysreg = syscon_regmap_lookup_by_phandle(dev->of_node,
				"samsung,syscon-pcie");
		if (IS_ERR(ep->sysreg)) {
			dev_err(dev, "sysreg regmap lookup failed.\n");
			return PTR_ERR(ep->sysreg);
		}

		ret = of_property_read_u32_index(dev->of_node, "samsung,syscon-pcie", 1,
						 &ep->sysreg_offset);
		if (ret) {
			dev_err(dev, "couldn't get the register offset for syscon!\n");
			return ret;
		}
	}

	/* External Local Bus interface (ELBI) registers */
	ep->elbi_base = devm_platform_ioremap_resource_byname(pdev, "elbi");
	if (IS_ERR(ep->elbi_base))
		return PTR_ERR(ep->elbi_base);

	ret = devm_clk_bulk_get_all_enabled(dev, &ep->clks);
	if (ret < 0)
		return ret;

	if (pdata->res_ops && pdata->res_ops->init_regulator) {
		ret = ep->pdata->res_ops->init_regulator(ep);
		if (ret)
			return ret;
	}

	ret = samsung_regulator_enable(ep);
	if (ret)
		return ret;

	platform_set_drvdata(pdev, ep);

	if (pdata->res_ops->set_device_mode)
		pdata->res_ops->set_device_mode(ep);

	switch (ep->pdata->device_mode) {
	case DW_PCIE_RC_TYPE:
		ret = samsung_irq_init(ep, pdev);
		if (ret)
			goto fail_regulator;

		ep->pci.pp.ops = pdata->host_ops;

		ret = dw_pcie_host_init(&ep->pci.pp);
		if (ret < 0)
			goto fail_phy_init;

		break;
	case DW_PCIE_EP_TYPE:
		phy_init(ep->phy);

		ep->pci.ep.ops = pdata->ep_ops;

		ret = dw_pcie_ep_init(&ep->pci.ep);
		if (ret < 0)
			goto fail_phy_init;

		ret = dw_pcie_ep_init_registers(&ep->pci.ep);
		if (ret)
			goto fail_phy_init;

		pci_epc_init_notify(ep->pci.ep.epc);

		break;
	default:
		dev_err(dev, "invalid device type\n");
		goto fail_phy_init;
	}

	return 0;

fail_phy_init:
	phy_exit(ep->phy);
fail_regulator:
	samsung_regulator_disable(ep);

	return ret;
}

static void exynos_pcie_remove(struct platform_device *pdev)
{
	struct exynos_pcie *ep = platform_get_drvdata(pdev);

	if (ep->pdata->device_mode == DW_PCIE_EP_TYPE)
		return;
	dw_pcie_host_deinit(&ep->pci.pp);
	if (ep->pdata->soc_variant == EXYNOS_5433)
		exynos_pcie_assert_core_reset(ep);
	phy_power_off(ep->phy);
	phy_exit(ep->phy);
	samsung_regulator_disable(ep);
}

static int exynos_pcie_suspend_noirq(struct device *dev)
{
	struct exynos_pcie *ep = dev_get_drvdata(dev);
	struct dw_pcie *pci = &ep->pci;

	if (ep->pdata->device_mode == DW_PCIE_EP_TYPE)
		return 0;

	if (ep->pdata->dwc_ops->stop_link)
		ep->pdata->dwc_ops->stop_link(pci);

	if (ep->pdata->soc_variant == EXYNOS_5433)
		exynos_pcie_assert_core_reset(ep);
	phy_power_off(ep->phy);
	phy_exit(ep->phy);
	samsung_regulator_disable(ep);

	return 0;
}

static int exynos_pcie_resume_noirq(struct device *dev)
{
	struct exynos_pcie *ep = dev_get_drvdata(dev);
	struct dw_pcie *pci = &ep->pci;
	struct dw_pcie_rp *pp = &pci->pp;
	int ret;

	if (ep->pdata->device_mode == DW_PCIE_EP_TYPE)
		return 0;

	ret = samsung_regulator_enable(ep);
	if (ret)
		return ret;

	/* exynos_pcie_host_init controls ep->phy */
	ep->pdata->host_ops->init(pp);
	dw_pcie_setup_rc(pp);
	ep->pdata->dwc_ops->start_link(pci);
	return dw_pcie_wait_for_link(pci);
}

static const struct dev_pm_ops exynos_pcie_pm_ops = {
	NOIRQ_SYSTEM_SLEEP_PM_OPS(exynos_pcie_suspend_noirq,
				  exynos_pcie_resume_noirq)
};


static const struct samsung_pcie_pdata fsd_hw3_pcie_rc_pdata = {
	.dwc_ops		= &fsd_dw_pcie_ops,
	.host_ops		= &fsd_pcie_host_ops,
	.res_ops		= &fsd_res_ops_data,
	.soc_variant		= FSD,
	.device_mode		= DW_PCIE_RC_TYPE,
};

static const struct samsung_pcie_pdata fsd_hw3_pcie_ep_pdata = {
	.dwc_ops		= &fsd_dw_pcie_ops,
	.ep_ops			= &fsd_ep_ops,
	.res_ops		= &fsd_res_ops_data,
	.soc_variant		= FSD,
	.device_mode		= DW_PCIE_EP_TYPE,
};

static const struct samsung_pcie_pdata exynos_5433_pcie_rc_pdata = {
	.dwc_ops		= &exynos_dw_pcie_ops,
	.pci_ops		= &exynos_pci_ops,
	.host_ops		= &exynos_pcie_host_ops,
	.res_ops		= &exynos_res_ops_data,
	.soc_variant		= EXYNOS_5433,
	.device_mode		= DW_PCIE_RC_TYPE,
};

static const struct of_device_id exynos_pcie_of_match[] = {
	{
		.compatible = "samsung,exynos5433-pcie",
		.data = (void *) &exynos_5433_pcie_rc_pdata,
	},
	{
		.compatible = "tesla,fsd-pcie",
		.data = (void *) &fsd_hw3_pcie_rc_pdata,
	},
	{
		.compatible = "tesla,fsd-pcie-ep",
		.data = (void *) &fsd_hw3_pcie_ep_pdata,
	},
	{ },
};

static struct platform_driver exynos_pcie_driver = {
	.probe		= exynos_pcie_probe,
	.remove		= exynos_pcie_remove,
	.driver = {
		.name	= "exynos-pcie",
		.of_match_table = exynos_pcie_of_match,
		.pm		= &exynos_pcie_pm_ops,
	},
};
module_platform_driver(exynos_pcie_driver);
MODULE_DESCRIPTION("Samsung Exynos PCIe host controller driver");
MODULE_LICENSE("GPL v2");
MODULE_DEVICE_TABLE(of, exynos_pcie_of_match);
