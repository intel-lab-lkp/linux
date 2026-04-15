// SPDX-License-Identifier: GPL-2.0
/*
 * DWC PCIe RC driver for UltraRISC DP1000 SoC
 *
 * Copyright (C) 2026 UltraRISC Technology (Shanghai) Co., Ltd.
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/pci.h>
#include <linux/platform_device.h>
#include <linux/resource.h>
#include <linux/types.h>

#include "pcie-designware.h"

#define PCIE_CUS_CORE          0x400000

#define LTSSM_ENABLE           BIT(7)
#define FAST_LINK_MODE         BIT(12)
#define HOLD_PHY_RST           BIT(14)
#define L1SUB_DISABLE          BIT(15)

struct ultrarisc_pcie {
	struct dw_pcie *pci;
};

static struct pci_ops ultrarisc_pci_ops = {
	.map_bus = dw_pcie_own_conf_map_bus,
	.read = pci_generic_config_read32,
	.write = pci_generic_config_write32,
};

static int ultrarisc_pcie_host_init(struct dw_pcie_rp *pp)
{
	struct pci_host_bridge *bridge = pp->bridge;

	bridge->ops = &ultrarisc_pci_ops;

	return 0;
}

static void ultrarisc_pcie_pme_turn_off(struct dw_pcie_rp *pp)
{
	/*
	 * DP1000 does not support sending PME_Turn_Off from the RC.
	 * Keep this callback empty to skip the generic MSG TLP path.
	 */
}

static const struct dw_pcie_host_ops ultrarisc_pcie_host_ops = {
	.init = ultrarisc_pcie_host_init,
	.pme_turn_off = ultrarisc_pcie_pme_turn_off,
};

static int ultrarisc_pcie_start_link(struct dw_pcie *pci)
{
	u32 val;
	u8 cap_exp;

	val = dw_pcie_readl_dbi(pci, PCIE_CUS_CORE);
	val &= ~FAST_LINK_MODE;
	dw_pcie_writel_dbi(pci, PCIE_CUS_CORE, val);

	val = dw_pcie_readl_dbi(pci, PCIE_TIMER_CTRL_MAX_FUNC_NUM);
	FIELD_MODIFY(PORT_FLT_SF_MASK, &val, PORT_FLT_SF_VAL_64);
	dw_pcie_writel_dbi(pci, PCIE_TIMER_CTRL_MAX_FUNC_NUM, val);

	cap_exp = dw_pcie_find_capability(pci, PCI_CAP_ID_EXP);
	val = dw_pcie_readl_dbi(pci, cap_exp + PCI_EXP_LNKCTL2);
	FIELD_MODIFY(PCI_EXP_LNKCTL2_TLS, &val, PCI_EXP_LNKCTL2_TLS_16_0GT);
	dw_pcie_writel_dbi(pci, cap_exp + PCI_EXP_LNKCTL2, val);

	val = dw_pcie_readl_dbi(pci, PCIE_PORT_FORCE);
	FIELD_MODIFY(PORT_LINK_NUM_MASK, &val, 0);
	dw_pcie_writel_dbi(pci, PCIE_PORT_FORCE, val);

	val = dw_pcie_readl_dbi(pci, cap_exp + PCI_EXP_DEVCTL2);
	FIELD_MODIFY(PCI_EXP_DEVCTL2_COMP_TIMEOUT, &val, 0x6);
	dw_pcie_writel_dbi(pci, cap_exp + PCI_EXP_DEVCTL2, val);

	val = dw_pcie_readl_dbi(pci, PCIE_CUS_CORE);
	val &= ~(HOLD_PHY_RST | L1SUB_DISABLE);
	val |= LTSSM_ENABLE;
	dw_pcie_writel_dbi(pci, PCIE_CUS_CORE, val);

	return 0;
}

static const struct dw_pcie_ops dw_pcie_ops = {
	.start_link = ultrarisc_pcie_start_link,
};

static int ultrarisc_pcie_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct ultrarisc_pcie *pcie;
	struct dw_pcie *pci;
	struct dw_pcie_rp *pp;
	int ret;

	pcie = devm_kzalloc(dev, sizeof(*pcie), GFP_KERNEL);
	if (!pcie)
		return -ENOMEM;

	pci = devm_kzalloc(dev, sizeof(*pci), GFP_KERNEL);
	if (!pci)
		return -ENOMEM;

	pci->dev = dev;
	pci->ops = &dw_pcie_ops;

	/* Set a default value suitable for at most 16 in and 16 out windows */
	pci->atu_size = SZ_8K;
	pci->max_link_speed = 4;
	pcie->pci = pci;

	pp = &pci->pp;

	platform_set_drvdata(pdev, pcie);

	pp->irq = platform_get_irq(pdev, 1);
	if (pp->irq < 0)
		return pp->irq;

	pp->num_vectors = MAX_MSI_IRQS;
	/* No L2/L3 Ready indication is available on this platform. */
	pp->skip_l23_ready = true;
	pp->ops = &ultrarisc_pcie_host_ops;

	ret = dw_pcie_host_init(pp);
	if (ret) {
		dev_err(dev, "Failed to initialize host\n");
		return ret;
	}

	return 0;
}

static int ultrarisc_pcie_suspend_noirq(struct device *dev)
{
	struct ultrarisc_pcie *pcie = dev_get_drvdata(dev);
	struct dw_pcie *pci = pcie->pci;

	return dw_pcie_suspend_noirq(pci);
}

static int ultrarisc_pcie_resume_noirq(struct device *dev)
{
	struct ultrarisc_pcie *pcie = dev_get_drvdata(dev);
	struct dw_pcie *pci = pcie->pci;

	return dw_pcie_resume_noirq(pci);
}

static const struct dev_pm_ops ultrarisc_pcie_pm_ops = {
	NOIRQ_SYSTEM_SLEEP_PM_OPS(ultrarisc_pcie_suspend_noirq,
				  ultrarisc_pcie_resume_noirq)
};

static const struct of_device_id ultrarisc_pcie_of_match[] = {
	{
		.compatible = "ultrarisc,dp1000-pcie",
	},
	{},
};
MODULE_DEVICE_TABLE(of, ultrarisc_pcie_of_match);

static struct platform_driver ultrarisc_pcie_driver = {
	.driver = {
		.name	= "ultrarisc-pcie",
		.of_match_table = ultrarisc_pcie_of_match,
		.suppress_bind_attrs = true,
		.pm = &ultrarisc_pcie_pm_ops,
	},
	.probe = ultrarisc_pcie_probe,
};
module_platform_driver(ultrarisc_pcie_driver);

MODULE_DESCRIPTION("UltraRISC DP1000 DWC PCIe host controller");
MODULE_LICENSE("GPL");
