// SPDX-License-Identifier: GPL-2.0
/*
 * PCIe host controller driver for NXP S32G SoCs
 *
 * Copyright 2019-2025 NXP
 */

#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/of_address.h>
#include <linux/pci.h>
#include <linux/phy.h>
#include <linux/phy/phy.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/sizes.h>
#include <linux/types.h>

#include "pcie-designware.h"
#include "pcie-s32g-regs.h"

struct s32g_pcie {
	struct dw_pcie	pci;

	/*
	 * We have cfg in struct dw_pcie_rp and
	 * dbi in struct dw_pcie, so define only ctrl here
	 */
	void __iomem *ctrl_base;
	u64 coherency_base;

	struct phy *phy;
};

#define to_s32g_from_dw_pcie(x) \
	container_of(x, struct s32g_pcie, pci)

static void s32g_pcie_writel_ctrl(struct s32g_pcie *s32g_pp, u32 reg, u32 val)
{
	if (dw_pcie_write(s32g_pp->ctrl_base + reg, 0x4, val))
		dev_err(s32g_pp->pci.dev, "Write ctrl address failed\n");
}

static u32 s32g_pcie_readl_ctrl(struct s32g_pcie *s32g_pp, u32 reg)
{
	u32 val = 0;

	if (dw_pcie_read(s32g_pp->ctrl_base + reg, 0x4, &val))
		dev_err(s32g_pp->pci.dev, "Read ctrl address failed\n");

	return val;
}

static void s32g_pcie_enable_ltssm(struct s32g_pcie *s32g_pp)
{
	u32 reg;

	reg = s32g_pcie_readl_ctrl(s32g_pp, PE0_GEN_CTRL_3);
	reg |= LTSSM_EN;
	s32g_pcie_writel_ctrl(s32g_pp, PE0_GEN_CTRL_3, reg);
}

static void s32g_pcie_disable_ltssm(struct s32g_pcie *s32g_pp)
{
	u32 reg;

	reg = s32g_pcie_readl_ctrl(s32g_pp, PE0_GEN_CTRL_3);
	reg &= ~LTSSM_EN;
	s32g_pcie_writel_ctrl(s32g_pp, PE0_GEN_CTRL_3, reg);
}

static bool is_s32g_pcie_ltssm_enabled(struct s32g_pcie *s32g_pp)
{
	return (s32g_pcie_readl_ctrl(s32g_pp, PE0_GEN_CTRL_3) & LTSSM_EN);
}

static enum dw_pcie_ltssm s32g_pcie_get_ltssm(struct dw_pcie *pci)
{
	struct s32g_pcie *s32g_pp = to_s32g_from_dw_pcie(pci);
	u32 val = s32g_pcie_readl_ctrl(s32g_pp, PCIE_SS_PE0_LINK_DBG_2);

	return (enum dw_pcie_ltssm)FIELD_GET(PCIE_SS_SMLH_LTSSM_STATE_MASK, val);
}

#define PCIE_LINKUP	(PCIE_SS_SMLH_LINK_UP | PCIE_SS_RDLH_LINK_UP)

static bool has_data_phy_link(struct s32g_pcie *s32g_pp)
{
	u32 val = s32g_pcie_readl_ctrl(s32g_pp, PCIE_SS_PE0_LINK_DBG_2);

	if ((val & PCIE_LINKUP) == PCIE_LINKUP) {
		switch (val & PCIE_SS_SMLH_LTSSM_STATE_MASK) {
		case LTSSM_STATE_L0:
		case LTSSM_STATE_L0S:
		case LTSSM_STATE_L1_IDLE:
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

	return has_data_phy_link(s32g_pp);
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

struct dw_pcie_ops s32g_pcie_ops = {
	.get_ltssm = s32g_pcie_get_ltssm,
	.link_up = s32g_pcie_link_up,
	.start_link = s32g_pcie_start_link,
	.stop_link = s32g_pcie_stop_link,
};

static const struct dw_pcie_host_ops s32g_pcie_host_ops;

static void disable_equalization(struct dw_pcie *pci)
{
	u32 val;

	val = dw_pcie_readl_dbi(pci, GEN3_EQ_CONTROL_OFF);
	val &= ~(GEN3_EQ_CONTROL_OFF_FB_MODE |
		 GEN3_EQ_CONTROL_OFF_PSET_REQ_VEC);
	val |= FIELD_PREP(GEN3_EQ_CONTROL_OFF_FB_MODE, 1) |
	       FIELD_PREP(GEN3_EQ_CONTROL_OFF_PSET_REQ_VEC, 0x84);
	dw_pcie_dbi_ro_wr_en(pci);
	dw_pcie_writel_dbi(pci, GEN3_EQ_CONTROL_OFF, val);
	dw_pcie_dbi_ro_wr_dis(pci);
}

static void s32g_pcie_reset_mstr_ace(struct dw_pcie *pci, u64 ddr_base_addr)
{
	u32 ddr_base_low = lower_32_bits(ddr_base_addr);
	u32 ddr_base_high = upper_32_bits(ddr_base_addr);

	dw_pcie_dbi_ro_wr_en(pci);
	dw_pcie_writel_dbi(pci, PORT_LOGIC_COHERENCY_CONTROL_3, 0x0);

	/*
	 * Transactions to peripheral targets should be non-coherent,
	 * or Ncore might drop them. Define the start of DDR as seen by Linux
	 * as the boundary between "memory" and "peripherals", with peripherals
	 * being below this boundary, and memory addresses being above it.
	 * One example where this is needed are PCIe MSIs, which use NoSnoop=0
	 * and might end up routed to Ncore.
	 */
	dw_pcie_writel_dbi(pci, PORT_LOGIC_COHERENCY_CONTROL_1,
			   (ddr_base_low & CC_1_MEMTYPE_BOUNDARY_MASK) |
			   (CC_1_MEMTYPE_LOWER_PERIPH & CC_1_MEMTYPE_VALUE));
	dw_pcie_writel_dbi(pci, PORT_LOGIC_COHERENCY_CONTROL_2, ddr_base_high);
	dw_pcie_dbi_ro_wr_dis(pci);
}

static int init_pcie_controller(struct s32g_pcie *s32g_pp)
{
	struct dw_pcie *pci = &s32g_pp->pci;
	u8 offset = dw_pcie_find_capability(pci, PCI_CAP_ID_EXP);
	u32 val;

	/* Set RP mode */
	val = s32g_pcie_readl_ctrl(s32g_pp, PE0_GEN_CTRL_1);
	val &= ~SS_DEVICE_TYPE_MASK;
	val |= SS_DEVICE_TYPE(PCI_EXP_TYPE_ROOT_PORT);

	/* Use default CRNS */
	val &= ~SRIS_MODE_EN;

	s32g_pcie_writel_ctrl(s32g_pp, PE0_GEN_CTRL_1, val);

	/* Disable phase 2,3 equalization */
	disable_equalization(pci);

	/*
	 * Make sure we use the coherency defaults (just in case the settings
	 * have been changed from their reset values)
	 */
	s32g_pcie_reset_mstr_ace(pci, s32g_pp->coherency_base);

	val = dw_pcie_readl_dbi(pci, PCIE_PORT_FORCE);
	val |= PORT_FORCE_DO_DESKEW_FOR_SRIS;
	dw_pcie_dbi_ro_wr_en(pci);
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

	/*
	 * Enable the IO space, Memory space, Bus master,
	 * Parity error, Serr and disable INTx generation
	 */
	dw_pcie_writel_dbi(pci, PCI_COMMAND,
			   PCI_COMMAND_SERR | PCI_COMMAND_PARITY |
			   PCI_COMMAND_INTX_DISABLE | PCI_COMMAND_IO |
			   PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);

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

	/* Disable writing dbi registers */
	dw_pcie_dbi_ro_wr_dis(pci);

	return 0;
}

static int init_pcie_phy(struct s32g_pcie *s32g_pp)
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

static int deinit_pcie_phy(struct s32g_pcie *s32g_pp)
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

static struct pci_bus *s32g_get_child_downstream_bus(struct pci_bus *bus)
{
	struct pci_bus *child, *root_bus = NULL;

	list_for_each_entry(child, &bus->children, node) {
		if (child->parent == bus) {
			root_bus = child;
			break;
		}
	}

	if (!root_bus)
		return ERR_PTR(-ENODEV);

	return root_bus;
}

static void s32g_pcie_downstream_dev_to_D0(struct s32g_pcie *s32g_pp)
{
	struct dw_pcie *pci = &s32g_pp->pci;
	struct dw_pcie_rp *pp = &pci->pp;
	struct pci_bus *root_bus = NULL;
	struct pci_dev *pdev;

	/* Check if we did manage to initialize the host */
	if (!pp->bridge || !pp->bridge->bus)
		return;

	/*
	 * link doesn't go into L2 state with some of the Endpoints
	 * if they are not in D0 state. So, we need to make sure that
	 * immediate downstream devices are in D0 state before sending
	 * PME_TurnOff to put link into L2 state.
	 */

	root_bus = s32g_get_child_downstream_bus(pp->bridge->bus);
	if (IS_ERR(root_bus)) {
		dev_err(pci->dev, "Failed to find downstream devices\n");
		return;
	}

	list_for_each_entry(pdev, &root_bus->devices, bus_list) {
		if (PCI_SLOT(pdev->devfn) == 0) {
			if (pci_set_power_state(pdev, PCI_D0))
				dev_err(pci->dev,
					"Failed to transition %s to D0 state\n",
					dev_name(&pdev->dev));
		}
	}
}

static u64 s32g_get_coherency_boundary(struct device *dev)
{
	struct device_node *np;
	struct resource res;

	np = of_find_node_by_type(NULL, "memory");

	if (of_address_to_resource(np, 0, &res)) {
		dev_warn(dev, "Fail to get coherency boundary\n");
		res.start = 0;
	}

	of_node_put(np);

	return res.start;
}

static int s32g_pcie_get_resources(struct platform_device *pdev,
				   struct s32g_pcie *s32g_pp)
{
	struct device *dev = &pdev->dev;
	struct dw_pcie *pci = &s32g_pp->pci;
	struct phy *phy;

	pci->dev = dev;
	pci->ops = &s32g_pcie_ops;

	platform_set_drvdata(pdev, s32g_pp);

	phy = devm_phy_get(dev, NULL);
	if (IS_ERR(phy))
		return dev_err_probe(dev, PTR_ERR(phy),
				"Failed to get serdes PHY\n");
	s32g_pp->phy = phy;

	pci->dbi_base = devm_platform_ioremap_resource_byname(pdev, "dbi");
	if (IS_ERR(pci->dbi_base))
		return PTR_ERR(pci->dbi_base);

	s32g_pp->ctrl_base = devm_platform_ioremap_resource_byname(pdev, "ctrl");
	if (IS_ERR(s32g_pp->ctrl_base))
		return PTR_ERR(s32g_pp->ctrl_base);

	s32g_pp->coherency_base = s32g_get_coherency_boundary(dev);

	return 0;
}

static int s32g_pcie_init(struct device *dev,
			  struct s32g_pcie *s32g_pp)
{
	int ret;

	s32g_pcie_disable_ltssm(s32g_pp);

	ret = init_pcie_phy(s32g_pp);
	if (ret)
		return ret;

	ret = init_pcie_controller(s32g_pp);
	if (ret)
		goto err_deinit_phy;

	return 0;

err_deinit_phy:
	deinit_pcie_phy(s32g_pp);
	return ret;
}

static void s32g_pcie_deinit(struct s32g_pcie *s32g_pp)
{
	s32g_pcie_disable_ltssm(s32g_pp);
	deinit_pcie_phy(s32g_pp);
}

static int s32g_pcie_host_init(struct device *dev,
			       struct s32g_pcie *s32g_pp)
{
	struct dw_pcie *pci = &s32g_pp->pci;
	struct dw_pcie_rp *pp = &pci->pp;
	int ret;

	pp->ops = &s32g_pcie_host_ops;

	ret = dw_pcie_host_init(pp);
	if (ret) {
		dev_err(dev, "Failed to initialize host\n");
		goto err_host_deinit;
	}

	return 0;

err_host_deinit:
	dw_pcie_host_deinit(pp);
	return ret;
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

	devm_pm_runtime_enable(dev);
	ret = pm_runtime_get_sync(dev);
	if (ret < 0)
		goto err_pm_runtime_put;

	ret = s32g_pcie_init(dev, s32g_pp);
	if (ret)
		goto err_pm_runtime_put;

	ret = s32g_pcie_host_init(dev, s32g_pp);
	if (ret)
		goto err_deinit_controller;

	return 0;

err_deinit_controller:
	s32g_pcie_deinit(s32g_pp);
err_pm_runtime_put:
	pm_runtime_put(dev);

	return ret;
}

static int s32g_pcie_suspend(struct device *dev)
{
	struct s32g_pcie *s32g_pp = dev_get_drvdata(dev);
	struct dw_pcie *pci = &s32g_pp->pci;
	struct dw_pcie_rp *pp = &pci->pp;
	struct pci_bus *bus, *root_bus;

	s32g_pcie_downstream_dev_to_D0(s32g_pp);

	bus = pp->bridge->bus;
	root_bus = s32g_get_child_downstream_bus(bus);
	if (!IS_ERR(root_bus))
		pci_walk_bus(root_bus, pci_dev_set_disconnected, NULL);

	pci_stop_root_bus(bus);
	pci_remove_root_bus(bus);

	s32g_pcie_deinit(s32g_pp);

	return 0;
}

static int s32g_pcie_resume(struct device *dev)
{
	struct s32g_pcie *s32g_pp = dev_get_drvdata(dev);
	struct dw_pcie *pci = &s32g_pp->pci;
	struct dw_pcie_rp *pp = &pci->pp;
	int ret = 0;

	ret = s32g_pcie_init(dev, s32g_pp);
	if (ret < 0)
		return ret;

	ret = dw_pcie_setup_rc(pp);
	if (ret) {
		dev_err(dev, "Failed to resume DW RC: %d\n", ret);
		goto fail_host_init;
	}

	ret = dw_pcie_start_link(pci);
	if (ret) {
		/*
		 * We do not exit with error if link up was unsuccessful
		 * Endpoint may not be connected.
		 */
		if (dw_pcie_wait_for_link(pci))
			dev_warn(pci->dev,
				 "Link Up failed, Endpoint may not be connected\n");

		if (!phy_validate(s32g_pp->phy, PHY_MODE_PCIE, 0, NULL)) {
			dev_err(dev, "Failed to get link up with EP connected\n");
			goto fail_host_init;
		}
	}

	ret = pci_host_probe(pp->bridge);
	if (ret)
		goto fail_host_init;

	return 0;

fail_host_init:
	s32g_pcie_deinit(s32g_pp);
	return ret;
}

static const struct dev_pm_ops s32g_pcie_pm_ops = {
	SYSTEM_SLEEP_PM_OPS(s32g_pcie_suspend,
			    s32g_pcie_resume)
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
	},
	.probe = s32g_pcie_probe,
};

module_platform_driver(s32g_pcie_driver);

MODULE_AUTHOR("Ionut Vicovan <Ionut.Vicovan@nxp.com>");
MODULE_DESCRIPTION("NXP S32G PCIe Host controller driver");
MODULE_LICENSE("GPL");
