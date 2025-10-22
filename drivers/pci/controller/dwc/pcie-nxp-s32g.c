// SPDX-License-Identifier: GPL-2.0
/*
 * PCIe host controller driver for NXP S32G SoCs
 *
 * Copyright 2019-2025 NXP
 */

#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/memblock.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/of_address.h>
#include <linux/pci.h>
#include <linux/phy/phy.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/sizes.h>
#include <linux/types.h>

#include "pcie-designware.h"
#include "pcie-nxp-s32g-regs.h"

struct s32g_pcie {
	struct dw_pcie	pci;
	void __iomem *ctrl_base;
	struct phy *phy;
};

#define to_s32g_from_dw_pcie(x) \
	container_of(x, struct s32g_pcie, pci)

static void s32g_pcie_writel_ctrl(struct s32g_pcie *s32g_pp, u32 reg, u32 val)
{
	writel(val, s32g_pp->ctrl_base + reg);
}

static u32 s32g_pcie_readl_ctrl(struct s32g_pcie *s32g_pp, u32 reg)
{
	return readl(s32g_pp->ctrl_base + reg);
}

static void s32g_pcie_enable_ltssm(struct s32g_pcie *s32g_pp)
{
	u32 reg;

	reg = s32g_pcie_readl_ctrl(s32g_pp, PCIE_S32G_PE0_GEN_CTRL_3);
	reg |= LTSSM_EN;
	s32g_pcie_writel_ctrl(s32g_pp, PCIE_S32G_PE0_GEN_CTRL_3, reg);
}

static void s32g_pcie_disable_ltssm(struct s32g_pcie *s32g_pp)
{
	u32 reg;

	reg = s32g_pcie_readl_ctrl(s32g_pp, PCIE_S32G_PE0_GEN_CTRL_3);
	reg &= ~LTSSM_EN;
	s32g_pcie_writel_ctrl(s32g_pp, PCIE_S32G_PE0_GEN_CTRL_3, reg);
}

static bool is_s32g_pcie_ltssm_enabled(struct s32g_pcie *s32g_pp)
{
	return (s32g_pcie_readl_ctrl(s32g_pp, PCIE_S32G_PE0_GEN_CTRL_3) & LTSSM_EN);
}

static enum dw_pcie_ltssm s32g_pcie_get_ltssm(struct dw_pcie *pci)
{
	struct s32g_pcie *s32g_pp = to_s32g_from_dw_pcie(pci);
	u32 reg = s32g_pcie_readl_ctrl(s32g_pp, PCIE_S32G_PE0_LINK_DBG_2);

	return (enum dw_pcie_ltssm)FIELD_GET(SMLH_LTSSM_STATE_MASK, reg);
}

#define PCIE_LINKUP	(SMLH_LINK_UP | RDLH_LINK_UP)

static bool s32g_has_data_phy_link(struct s32g_pcie *s32g_pp)
{
	u32 reg = s32g_pcie_readl_ctrl(s32g_pp, PCIE_S32G_PE0_LINK_DBG_2);

	if ((reg & PCIE_LINKUP) == PCIE_LINKUP) {
		switch (FIELD_GET(SMLH_LTSSM_STATE_MASK, reg)) {
		case DW_PCIE_LTSSM_L0:
		case DW_PCIE_LTSSM_L0S:
		case DW_PCIE_LTSSM_L1_IDLE:
			return true;
		default:
			return false;
		}
	}

	return false;
}

static bool s32g_pcie_link_up(struct dw_pcie *pci)
{
	struct s32g_pcie *s32g_pp = to_s32g_from_dw_pcie(pci);

	if (!is_s32g_pcie_ltssm_enabled(s32g_pp))
		return false;

	return s32g_has_data_phy_link(s32g_pp);
}

static int s32g_pcie_start_link(struct dw_pcie *pci)
{
	struct s32g_pcie *s32g_pp = to_s32g_from_dw_pcie(pci);

	s32g_pcie_enable_ltssm(s32g_pp);

	return 0;
}

static void s32g_pcie_stop_link(struct dw_pcie *pci)
{
	struct s32g_pcie *s32g_pp = to_s32g_from_dw_pcie(pci);

	s32g_pcie_disable_ltssm(s32g_pp);
}

static struct dw_pcie_ops s32g_pcie_ops = {
	.get_ltssm = s32g_pcie_get_ltssm,
	.link_up = s32g_pcie_link_up,
	.start_link = s32g_pcie_start_link,
	.stop_link = s32g_pcie_stop_link,
};

static void s32g_pcie_pme_turn_off(struct dw_pcie_rp *pp)
{
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);
	struct s32g_pcie *s32g_pp = to_s32g_from_dw_pcie(pci);
	u32 reg;

	reg = s32g_pcie_readl_ctrl(s32g_pp, PCIE_S32G_PE0_TX_MSG_REQ);
	reg |= PME_TURN_OFF_REQ;
	s32g_pcie_writel_ctrl(s32g_pp, PCIE_S32G_PE0_TX_MSG_REQ, reg);
}

static const struct dw_pcie_host_ops s32g_pcie_host_ops = {
	.pme_turn_off = s32g_pcie_pme_turn_off,
};

static void s32g_pcie_disable_equalization(struct dw_pcie *pci)
{
	u32 reg;

	reg = dw_pcie_readl_dbi(pci, GEN3_EQ_CONTROL_OFF);
	reg &= ~(GEN3_EQ_CONTROL_OFF_FB_MODE |
		 GEN3_EQ_CONTROL_OFF_PSET_REQ_VEC);
	reg |= FIELD_PREP(GEN3_EQ_CONTROL_OFF_FB_MODE, 1) |
	       FIELD_PREP(GEN3_EQ_CONTROL_OFF_PSET_REQ_VEC, 0x84);

	dw_pcie_dbi_ro_wr_en(pci);
	dw_pcie_writel_dbi(pci, GEN3_EQ_CONTROL_OFF, reg);
	dw_pcie_dbi_ro_wr_dis(pci);
}

/* Configure the AMBA AXI Coherency Extensions (ACE) interface */
static void s32g_pcie_reset_mstr_ace(struct dw_pcie *pci, u64 ddr_base_addr)
{
	u32 ddr_base_low = lower_32_bits(ddr_base_addr);
	u32 ddr_base_high = upper_32_bits(ddr_base_addr);

	dw_pcie_dbi_ro_wr_en(pci);
	dw_pcie_writel_dbi(pci, COHERENCY_CONTROL_3_OFF, 0x0);

	/*
	 * Ncore is a cache-coherent interconnect module that enables the
	 * integration of heterogeneous coherent and non-coherent agents in
	 * the chip. Ncore Transactions to peripheral should be non-coherent
	 * or it might drop them.
	 * One example where this is needed are PCIe MSIs, which use NoSnoop=0
	 * and might end up routed to Ncore.
	 * Define the start of DDR as seen by Linux as the boundary between
	 * "memory" and "peripherals", with peripherals being below.
	 */
	dw_pcie_writel_dbi(pci, COHERENCY_CONTROL_1_OFF,
			   (ddr_base_low & CFG_MEMTYPE_BOUNDARY_LOW_ADDR_MASK));
	dw_pcie_writel_dbi(pci, COHERENCY_CONTROL_2_OFF, ddr_base_high);
	dw_pcie_dbi_ro_wr_dis(pci);
}

static void s32g_init_pcie_controller(struct s32g_pcie *s32g_pp)
{
	struct dw_pcie *pci = &s32g_pp->pci;
	u8 offset = dw_pcie_find_capability(pci, PCI_CAP_ID_EXP);
	u32 val;

	/* Set RP mode */
	val = s32g_pcie_readl_ctrl(s32g_pp, PCIE_S32G_PE0_GEN_CTRL_1);
	val &= ~DEVICE_TYPE_MASK;
	val |= DEVICE_TYPE(PCI_EXP_TYPE_ROOT_PORT);

	/* Use default CRNS */
	val &= ~SRIS_MODE;

	s32g_pcie_writel_ctrl(s32g_pp, PCIE_S32G_PE0_GEN_CTRL_1, val);

	/* Disable phase 2,3 equalization */
	s32g_pcie_disable_equalization(pci);

	/*
	 * Make sure we use the coherency defaults (just in case the settings
	 * have been changed from their reset values)
	 */
	s32g_pcie_reset_mstr_ace(pci, memblock_start_of_DRAM());

	dw_pcie_dbi_ro_wr_en(pci);

	val = dw_pcie_readl_dbi(pci, PCIE_PORT_FORCE);
	val |= PORT_FORCE_DO_DESKEW_FOR_SRIS;
	dw_pcie_writel_dbi(pci, PCIE_PORT_FORCE, val);

	/*
	 * Set max payload supported, 256 bytes and
	 * relaxed ordering.
	 */
	val = dw_pcie_readl_dbi(pci, offset + PCI_EXP_DEVCTL);
	val &= ~(PCI_EXP_DEVCTL_RELAX_EN |
		 PCI_EXP_DEVCTL_PAYLOAD |
		 PCI_EXP_DEVCTL_READRQ);
	val |= PCI_EXP_DEVCTL_RELAX_EN |
	       PCI_EXP_DEVCTL_PAYLOAD_256B |
	       PCI_EXP_DEVCTL_READRQ_256B;
	dw_pcie_writel_dbi(pci, offset + PCI_EXP_DEVCTL, val);

	/* Enable errors */
	val = dw_pcie_readl_dbi(pci, offset + PCI_EXP_DEVCTL);
	val |= PCI_EXP_DEVCTL_CERE |
	       PCI_EXP_DEVCTL_NFERE |
	       PCI_EXP_DEVCTL_FERE |
	       PCI_EXP_DEVCTL_URRE;
	dw_pcie_writel_dbi(pci, offset + PCI_EXP_DEVCTL, val);

	val = dw_pcie_readl_dbi(pci, GEN3_RELATED_OFF);
	val |= GEN3_RELATED_OFF_EQ_PHASE_2_3;
	dw_pcie_writel_dbi(pci, GEN3_RELATED_OFF, val);

	dw_pcie_dbi_ro_wr_dis(pci);
}

static int s32g_init_pcie_phy(struct s32g_pcie *s32g_pp)
{
	struct dw_pcie *pci = &s32g_pp->pci;
	struct device *dev = pci->dev;
	int ret;

	ret = phy_init(s32g_pp->phy);
	if (ret) {
		dev_err(dev, "Failed to init serdes PHY\n");
		return ret;
	}

	ret = phy_set_mode_ext(s32g_pp->phy, PHY_MODE_PCIE, 0);
	if (ret) {
		dev_err(dev, "Failed to set mode on serdes PHY\n");
		goto err_phy_exit;
	}

	ret = phy_power_on(s32g_pp->phy);
	if (ret) {
		dev_err(dev, "Failed to power on serdes PHY\n");
		goto err_phy_exit;
	}

	return 0;

err_phy_exit:
	phy_exit(s32g_pp->phy);
	return ret;
}

static int s32g_deinit_pcie_phy(struct s32g_pcie *s32g_pp)
{
	struct dw_pcie *pci = &s32g_pp->pci;
	struct device *dev = pci->dev;
	int ret;

	ret = phy_power_off(s32g_pp->phy);
	if (ret) {
		dev_err(dev, "Failed to power off serdes PHY\n");
		return ret;
	}

	ret = phy_exit(s32g_pp->phy);
	if (ret) {
		dev_err(dev, "Failed to exit serdes PHY\n");
		return ret;
	}

	return 0;
}

static int s32g_pcie_init(struct device *dev,
			  struct s32g_pcie *s32g_pp)
{
	int ret;

	s32g_pcie_disable_ltssm(s32g_pp);

	ret = s32g_init_pcie_phy(s32g_pp);
	if (ret)
		return ret;

	s32g_init_pcie_controller(s32g_pp);

	return 0;
}

static void s32g_pcie_deinit(struct s32g_pcie *s32g_pp)
{
	s32g_pcie_disable_ltssm(s32g_pp);
	s32g_deinit_pcie_phy(s32g_pp);
}

static int s32g_pcie_host_init(struct s32g_pcie *s32g_pp)
{
	struct dw_pcie *pci = &s32g_pp->pci;
	struct dw_pcie_rp *pp = &pci->pp;
	int ret;

	pp->ops = &s32g_pcie_host_ops;

	ret = dw_pcie_host_init(pp);

	return ret;
}

static int s32g_pcie_get_resources(struct platform_device *pdev,
				   struct s32g_pcie *s32g_pp)
{
	struct device *dev = &pdev->dev;
	struct dw_pcie *pci = &s32g_pp->pci;

	s32g_pp->phy = devm_phy_get(dev, NULL);
	if (IS_ERR(s32g_pp->phy))
		return dev_err_probe(dev, PTR_ERR(s32g_pp->phy),
				"Failed to get serdes PHY\n");
	s32g_pp->ctrl_base = devm_platform_ioremap_resource_byname(pdev, "ctrl");
	if (IS_ERR(s32g_pp->ctrl_base))
		return PTR_ERR(s32g_pp->ctrl_base);

	pci->dbi_base = devm_platform_ioremap_resource_byname(pdev, "dbi");
	if (IS_ERR(pci->dbi_base))
		return PTR_ERR(pci->dbi_base);

	pci->dev = dev;
	pci->ops = &s32g_pcie_ops;

	platform_set_drvdata(pdev, s32g_pp);

	return 0;
}

static int s32g_pcie_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct s32g_pcie *s32g_pp;
	int ret;

	s32g_pp = devm_kzalloc(dev, sizeof(*s32g_pp), GFP_KERNEL);
	if (!s32g_pp)
		return -ENOMEM;

	ret = s32g_pcie_get_resources(pdev, s32g_pp);
	if (ret)
		return ret;

	pm_runtime_no_callbacks(dev);
	devm_pm_runtime_enable(dev);
	ret = pm_runtime_get_sync(dev);
	if (ret < 0)
		goto err_pm_runtime_put;

	ret = s32g_pcie_init(dev, s32g_pp);
	if (ret)
		goto err_pm_runtime_put;

	ret = s32g_pcie_host_init(s32g_pp);
	if (ret)
		goto err_pcie_deinit;

	return 0;

err_pcie_deinit:
	s32g_pcie_deinit(s32g_pp);
err_pm_runtime_put:
	pm_runtime_put(dev);

	return ret;
}

static int s32g_pcie_suspend_noirq(struct device *dev)
{
	struct s32g_pcie *s32g_pp = dev_get_drvdata(dev);
	struct dw_pcie *pci = &s32g_pp->pci;

	if (!dw_pcie_link_up(pci))
		return 0;

	return dw_pcie_suspend_noirq(pci);
}

static int s32g_pcie_resume_noirq(struct device *dev)
{
	struct s32g_pcie *s32g_pp = dev_get_drvdata(dev);
	struct dw_pcie *pci = &s32g_pp->pci;

	s32g_init_pcie_controller(s32g_pp);

	return dw_pcie_resume_noirq(pci);
}

static const struct dev_pm_ops s32g_pcie_pm_ops = {
	NOIRQ_SYSTEM_SLEEP_PM_OPS(s32g_pcie_suspend_noirq,
				  s32g_pcie_resume_noirq)
};

static const struct of_device_id s32g_pcie_of_match[] = {
	{ .compatible = "nxp,s32g2-pcie"},
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(of, s32g_pcie_of_match);

static struct platform_driver s32g_pcie_driver = {
	.driver = {
		.name	= "s32g-pcie",
		.of_match_table = s32g_pcie_of_match,
		.suppress_bind_attrs = true,
		.pm = pm_sleep_ptr(&s32g_pcie_pm_ops),
		.probe_type = PROBE_PREFER_ASYNCHRONOUS,
	},
	.probe = s32g_pcie_probe,
};

module_platform_driver(s32g_pcie_driver);

MODULE_AUTHOR("Ionut Vicovan <Ionut.Vicovan@nxp.com>");
MODULE_DESCRIPTION("NXP S32G PCIe Host controller driver");
MODULE_LICENSE("GPL");
