// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2017 Cadence
// Cadence PCIe controller driver.
// Author: Cyrille Pitchen <cyrille.pitchen@free-electrons.com>

#include <linux/kernel.h>
#include <linux/of.h>

#include "pcie-cadence.h"

void cdns_pcie_disable_phy(struct cdns_pcie *pcie)
{
	int i = pcie->phy_count;

	while (i--) {
		phy_power_off(pcie->phy[i]);
		phy_exit(pcie->phy[i]);
	}
}
EXPORT_SYMBOL_GPL(cdns_pcie_disable_phy);

int cdns_pcie_enable_phy(struct cdns_pcie *pcie)
{
	int ret;
	int i;

	for (i = 0; i < pcie->phy_count; i++) {
		ret = phy_init(pcie->phy[i]);
		if (ret < 0)
			goto err_phy;

		ret = phy_power_on(pcie->phy[i]);
		if (ret < 0) {
			phy_exit(pcie->phy[i]);
			goto err_phy;
		}
	}

	return 0;

err_phy:
	while (--i >= 0) {
		phy_power_off(pcie->phy[i]);
		phy_exit(pcie->phy[i]);
	}

	return ret;
}
EXPORT_SYMBOL_GPL(cdns_pcie_enable_phy);

int cdns_pcie_init_phy(struct device *dev, struct cdns_pcie *pcie)
{
	struct device_node *np = dev->of_node;
	int phy_count;
	struct phy **phy;
	struct device_link **link;
	int i;
	int ret;
	const char *name;

	phy_count = of_property_count_strings(np, "phy-names");
	if (phy_count < 1) {
		dev_info(dev, "no \"phy-names\" property found; PHY will not be initialized\n");
		pcie->phy_count = 0;
		return 0;
	}

	phy = devm_kcalloc(dev, phy_count, sizeof(*phy), GFP_KERNEL);
	if (!phy)
		return -ENOMEM;

	link = devm_kcalloc(dev, phy_count, sizeof(*link), GFP_KERNEL);
	if (!link)
		return -ENOMEM;

	for (i = 0; i < phy_count; i++) {
		of_property_read_string_index(np, "phy-names", i, &name);
		phy[i] = devm_phy_get(dev, name);
		if (IS_ERR(phy[i])) {
			ret = PTR_ERR(phy[i]);
			goto err_phy;
		}
		link[i] = device_link_add(dev, &phy[i]->dev, DL_FLAG_STATELESS);
		if (!link[i]) {
			devm_phy_put(dev, phy[i]);
			ret = -EINVAL;
			goto err_phy;
		}
	}

	pcie->phy_count = phy_count;
	pcie->phy = phy;
	pcie->link = link;

	ret =  cdns_pcie_enable_phy(pcie);
	if (ret)
		goto err_phy;

	return 0;

err_phy:
	while (--i >= 0) {
		device_link_del(link[i]);
		devm_phy_put(dev, phy[i]);
	}

	return ret;
}
EXPORT_SYMBOL_GPL(cdns_pcie_init_phy);

static int cdns_pcie_suspend_noirq(struct device *dev)
{
	struct cdns_pcie *pcie = dev_get_drvdata(dev);

	cdns_pcie_disable_phy(pcie);

	return 0;
}

static int cdns_pcie_resume_noirq(struct device *dev)
{
	struct cdns_pcie *pcie = dev_get_drvdata(dev);
	int ret;

	ret = cdns_pcie_enable_phy(pcie);
	if (ret) {
		dev_err(dev, "failed to enable PHY\n");
		return ret;
	}

	return 0;
}

const struct dev_pm_ops cdns_pcie_pm_ops = {
	NOIRQ_SYSTEM_SLEEP_PM_OPS(cdns_pcie_suspend_noirq,
				  cdns_pcie_resume_noirq)
};
EXPORT_SYMBOL_GPL(cdns_pcie_pm_ops);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Cadence PCIe controller driver");
