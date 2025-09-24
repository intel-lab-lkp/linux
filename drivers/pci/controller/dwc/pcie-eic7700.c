// SPDX-License-Identifier: GPL-2.0
/*
 * ESWIN PCIe root complex driver
 *
 * Copyright 2025, Beijing ESWIN Computing Technology Co., Ltd.
 *
 * Authors: Yu Ning <ningyu@eswincomputing.com>
 *          Senchuan Zhang <zhangsenchuan@eswincomputing.com>
 *          Yanghui Ou <ouyanghui@eswincomputing.com>
 */

#include <linux/interrupt.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/pci.h>
#include <linux/platform_device.h>
#include <linux/resource.h>
#include <linux/reset.h>
#include <linux/types.h>

#include "pcie-designware.h"

/* PCIe top csr registers */
#define PCIEMGMT_CTRL0_OFFSET		0x0
#define PCIEMGMT_STATUS0_OFFSET		0x100

/* LTSSM register fields */
#define PCIEMGMT_APP_LTSSM_ENABLE	BIT(5)

/* APP_HOLD_PHY_RST register fields */
#define PCIEMGMT_APP_HOLD_PHY_RST	BIT(6)

/* PM_SEL_AUX_CLK register fields */
#define PCIEMGMT_PM_SEL_AUX_CLK		BIT(16)

/* ROOT_PORT register fields */
#define PCIEMGMT_CTRL0_ROOT_PORT_MASK	GENMASK(3, 0)

/* Vendor and device id value */
#define VENDOR_ID_VALUE			0x1fe1
#define DEVICE_ID_VALUE			0x2030

/* Disable MSI-X cap register fields */
#define PCIE_MSIX_DISABLE_MASK		GENMASK(15, 8)

struct eswin_pcie_data {
	bool msix_cap;
};

struct eswin_pcie_port {
	struct list_head list;
	struct reset_control *perst;
	int num_lanes;
};

struct eswin_pcie {
	struct dw_pcie pci;
	void __iomem *mgmt_base;
	struct clk_bulk_data *clks;
	struct reset_control *powerup_rst;
	struct reset_control *cfg_rst;
	struct list_head ports;
	int num_clks;
	bool suspended;
	bool msix_cap;
};

#define to_eswin_pcie(x) dev_get_drvdata((x)->dev)

static int eswin_pcie_start_link(struct dw_pcie *pci)
{
	struct eswin_pcie *pcie = to_eswin_pcie(pci);
	u32 val;

	/* Enable LTSSM */
	val = readl_relaxed(pcie->mgmt_base + PCIEMGMT_CTRL0_OFFSET);
	val |= PCIEMGMT_APP_LTSSM_ENABLE;
	writel_relaxed(val, pcie->mgmt_base + PCIEMGMT_CTRL0_OFFSET);

	return 0;
}

static bool eswin_pcie_link_up(struct dw_pcie *pci)
{
	u16 offset = dw_pcie_find_capability(pci, PCI_CAP_ID_EXP);
	u16 val = readw(pci->dbi_base + offset + PCI_EXP_LNKSTA);

	return val & PCI_EXP_LNKSTA_DLLLA;
}

static int eswin_pcie_deassert(struct eswin_pcie *pcie)
{
	int ret;

	ret = reset_control_deassert(pcie->cfg_rst);
	if (ret) {
		dev_err(pcie->pci.dev, "Failed to deassert CFG#");
		return ret;
	}

	ret = reset_control_deassert(pcie->powerup_rst);
	if (ret) {
		dev_err(pcie->pci.dev, "Failed to deassert POWERUP#");
		goto err_powerup;
	}

	return 0;

err_powerup:
	reset_control_assert(pcie->cfg_rst);

	return ret;
}

static void eswin_pcie_assert(struct eswin_pcie *pcie)
{
	reset_control_assert(pcie->powerup_rst);
	reset_control_assert(pcie->cfg_rst);
}

static int eswin_pcie_perst_deassert(struct eswin_pcie_port *port,
				     struct eswin_pcie *pcie)
{
	int ret;

	ret = reset_control_assert(port->perst);
	if (ret) {
		dev_err(pcie->pci.dev, "Failed to assert PERST#");
		goto err_perst;
	}

	/* Ensure that PERST has been asserted for at least 100 ms */
	msleep(PCIE_T_PVPERL_MS);

	ret = reset_control_deassert(port->perst);
	if (ret) {
		dev_err(pcie->pci.dev, "Failed to deassert PERST#");
		goto err_perst;
	}

	return 0;

err_perst:
	list_for_each_entry(port, &pcie->ports, list)
		reset_control_put(port->perst);

	return ret;
}

static int eswin_pcie_parse_port(struct eswin_pcie *pcie,
				 struct device_node *node)
{
	struct device *dev = pcie->pci.dev;
	struct eswin_pcie_port *port;

	port = devm_kzalloc(dev, sizeof(*port), GFP_KERNEL);
	if (!port)
		return -ENOMEM;

	port->perst = of_reset_control_get(node, "perst");
	if (IS_ERR(port->perst)) {
		dev_err(dev, "Failed to get perst reset\n");
		return PTR_ERR(port->perst);
	}

	/*
	 * Since the root port node is separated out by pcie devicetree, the
	 * DWC core initialization code cannot parse the num-lanes attribute
	 * in the root port. Before entering the DWC core initialization code,
	 * the platform driver code parses the root port node. The EIC7700 only
	 * supports one root port node, and the num-lanes attribute is suitable
	 * for the case of one root port.
	*/
	of_property_read_u32(node, "num-lanes", &port->num_lanes);
	pcie->pci.num_lanes = port->num_lanes;

	INIT_LIST_HEAD(&port->list);
	list_add_tail(&port->list, &pcie->ports);

	return 0;
}

static int eswin_pcie_parse_ports(struct eswin_pcie *pcie)
{
	struct device *dev = pcie->pci.dev;
	struct eswin_pcie_port *port, *tmp;
	int ret;

	for_each_available_child_of_node_scoped(dev->of_node, of_port) {
		ret = eswin_pcie_parse_port(pcie, of_port);
		if (ret)
			goto err_port;
	}

	return ret;

err_port:
	list_for_each_entry_safe(port, tmp, &pcie->ports, list)
		list_del(&port->list);
	return ret;
}

static void eswin_pcie_hide_broken_msix_cap(struct dw_pcie *pci)
{
	u16 offset, val;

	/*
	 * Hardware doesn't support MSI-X but it advertises MSI-X capability,
	 * to avoid this problem, the MSI-X capability in the PCIe capabilities
	 * linked-list needs to be disabled. Since the PCI Express capability
	 * structure's next pointer points to the MSI-X capability, and the
	 * MSI-X capability's next pointer is null (00H), so only the PCI
	 * Express capability structure's next pointer needs to be set 00H.
	 */
	offset = dw_pcie_find_capability(pci, PCI_CAP_ID_EXP);
	val = dw_pcie_readl_dbi(pci, offset);
	val &= ~PCIE_MSIX_DISABLE_MASK;
	dw_pcie_writel_dbi(pci, offset, val);
}

static int eswin_pcie_host_init(struct dw_pcie_rp *pp)
{
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);
	struct eswin_pcie *pcie = to_eswin_pcie(pci);
	struct eswin_pcie_port *port;
	u32 retries;
	u8 msi_cap;
	u32 val;
	int ret;

	pcie->num_clks = devm_clk_bulk_get_all_enabled(pci->dev, &pcie->clks);
	if (pcie->num_clks < 0)
		return dev_err_probe(pci->dev, pcie->num_clks,
				     "Failed to get pcie clocks\n");

	ret = eswin_pcie_deassert(pcie);
	if (ret)
		return ret;

	/* Configure root port type */
	val = readl_relaxed(pcie->mgmt_base + PCIEMGMT_CTRL0_OFFSET);
	val &= ~PCIEMGMT_CTRL0_ROOT_PORT_MASK;
	writel_relaxed(val | PCI_EXP_TYPE_ROOT_PORT,
		       pcie->mgmt_base + PCIEMGMT_CTRL0_OFFSET);

	list_for_each_entry(port, &pcie->ports, list) {
		ret = eswin_pcie_perst_deassert(port, pcie);
			if (ret)
				goto err_perst;
	}

	/* Configure app_hold_phy_rst */
	val = readl_relaxed(pcie->mgmt_base + PCIEMGMT_CTRL0_OFFSET);
	val &= ~PCIEMGMT_APP_HOLD_PHY_RST;
	writel_relaxed(val, pcie->mgmt_base + PCIEMGMT_CTRL0_OFFSET);

	/* The maximum waiting time for the clock switch lock is 20ms */
	retries = 20;
	do {
		val = readl_relaxed(pcie->mgmt_base + PCIEMGMT_STATUS0_OFFSET);
		if (!(val & PCIEMGMT_PM_SEL_AUX_CLK))
			break;
		fsleep(1000);
		retries--;
	} while (retries);

	if (!retries) {
		dev_err(pci->dev, "Timeout waiting for PM_SEL_AUX_CLK ready\n");
		ret = -ETIMEDOUT;
		goto err_phy_init;
	}

	/*
	 * Configure ESWIN VID:DID for Root Port as the default values are
	 * invalid.
	 */
	dw_pcie_writew_dbi(pci, PCI_VENDOR_ID, VENDOR_ID_VALUE);
	dw_pcie_writew_dbi(pci, PCI_DEVICE_ID, DEVICE_ID_VALUE);

	/* Configure support 32 MSI vectors */
	msi_cap = dw_pcie_find_capability(pci, PCI_CAP_ID_MSI);
	val = dw_pcie_readw_dbi(pci, msi_cap + PCI_MSI_FLAGS);
	val &= ~PCI_MSI_FLAGS_QMASK;
	val |= FIELD_PREP(PCI_MSI_FLAGS_QMASK, 5);
	dw_pcie_writew_dbi(pci, msi_cap + PCI_MSI_FLAGS, val);

	/* Configure disable MSI-X cap */
	if (!pcie->msix_cap)
		eswin_pcie_hide_broken_msix_cap(pci);

	return 0;

err_phy_init:
	list_for_each_entry(port, &pcie->ports, list)
		reset_control_assert(port->perst);
err_perst:
	eswin_pcie_assert(pcie);

	return ret;
}

static const struct dw_pcie_host_ops eswin_pcie_host_ops = {
	.init = eswin_pcie_host_init,
};

static const struct dw_pcie_ops dw_pcie_ops = {
	.start_link = eswin_pcie_start_link,
	.link_up = eswin_pcie_link_up,
};

static int eswin_pcie_probe(struct platform_device *pdev)
{
	const struct eswin_pcie_data *data;
	struct eswin_pcie_port *port, *tmp;
	struct device *dev = &pdev->dev;
	struct eswin_pcie *pcie;
	struct dw_pcie *pci;
	int ret;

	data = of_device_get_match_data(dev);
	if (!data)
		return dev_err_probe(dev, -EINVAL, "OF data missing\n");

	pcie = devm_kzalloc(dev, sizeof(*pcie), GFP_KERNEL);
	if (!pcie)
		return -ENOMEM;

	INIT_LIST_HEAD(&pcie->ports);

	pci = &pcie->pci;
	pci->dev = dev;
	pci->ops = &dw_pcie_ops;
	pci->pp.ops = &eswin_pcie_host_ops;
	pcie->msix_cap = data->msix_cap;

	pcie->mgmt_base = devm_platform_ioremap_resource_byname(pdev, "mgmt");
	if (IS_ERR(pcie->mgmt_base))
		return dev_err_probe(dev, PTR_ERR(pcie->mgmt_base),
				     "Failed to map mgmt registers\n");

	pcie->powerup_rst = devm_reset_control_get(&pdev->dev, "powerup");
	if (IS_ERR(pcie->powerup_rst))
		return dev_err_probe(dev, PTR_ERR(pcie->powerup_rst),
				     "Failed to get powerup reset\n");

	pcie->cfg_rst = devm_reset_control_get(&pdev->dev, "cfg");
	if (IS_ERR(pcie->cfg_rst))
		return dev_err_probe(dev, PTR_ERR(pcie->cfg_rst),
				     "Failed to get cfg reset\n");

	ret = eswin_pcie_parse_ports(pcie);
	if (ret)
		dev_err_probe(pci->dev, ret,
			      "Failed to parse Root Port: %d\n", ret);

	platform_set_drvdata(pdev, pcie);

	ret = dw_pcie_host_init(&pci->pp);
	if (ret) {
		dev_err(dev, "Failed to initialize host\n");
		goto err_init;
	}

	return ret;

err_init:
	list_for_each_entry_safe(port, tmp, &pcie->ports, list) {
		list_del(&port->list);
		reset_control_put(port->perst);
	}
	return ret;
}

static int eswin_pcie_suspend(struct device *dev)
{
	struct eswin_pcie *pcie = dev_get_drvdata(dev);
	struct eswin_pcie_port *port;

	/*
	 * For controllers with active devices, resources are retained and
	 * cannot be turned off.
	 */
	if (!dw_pcie_link_up(&pcie->pci)) {
		list_for_each_entry(port, &pcie->ports, list)
			reset_control_assert(port->perst);
		eswin_pcie_assert(pcie);
		clk_bulk_disable_unprepare(pcie->num_clks, pcie->clks);
		pcie->suspended = true;
	}

	return 0;
}

static int eswin_pcie_resume(struct device *dev)
{
	struct eswin_pcie *pcie = dev_get_drvdata(dev);
	int ret;

	if (!pcie->suspended)
		return 0;

	ret = eswin_pcie_host_init(&pcie->pci.pp);
	if (ret) {
		dev_err(dev, "Failed to init host: %d\n", ret);
		return ret;
	}

	dw_pcie_setup_rc(&pcie->pci.pp);
	eswin_pcie_start_link(&pcie->pci);
	dw_pcie_wait_for_link(&pcie->pci);

	pcie->suspended = false;

	return 0;
}

static const struct dev_pm_ops eswin_pcie_pm_ops = {
	NOIRQ_SYSTEM_SLEEP_PM_OPS(eswin_pcie_suspend, eswin_pcie_resume)
};

static const struct eswin_pcie_data eswin_7700_data = {
	.msix_cap = false,
};

static const struct of_device_id eswin_pcie_of_match[] = {
	{ .compatible = "eswin,eic7700-pcie", .data = &eswin_7700_data },
	{},
};

static struct platform_driver eswin_pcie_driver = {
	.probe = eswin_pcie_probe,
	.driver = {
		.name = "eic7700-pcie",
		.of_match_table = eswin_pcie_of_match,
		.suppress_bind_attrs = true,
		.pm = &eswin_pcie_pm_ops,
	},
};
builtin_platform_driver(eswin_pcie_driver);

MODULE_DESCRIPTION("PCIe host controller driver for EIC7700 SoCs");
MODULE_AUTHOR("Yu Ning <ningyu@eswincomputing.com>");
MODULE_AUTHOR("Senchuan Zhang <zhangsenchuan@eswincomputing.com>");
MODULE_AUTHOR("Yanghui Ou <ouyanghui@eswincomputing.com>");
MODULE_LICENSE("GPL");
