// SPDX-License-Identifier: GPL-2.0-only
/*
 * STMicroelectronics STM32MP25 PCIe root complex driver.
 *
 * Copyright (C) 2024 STMicroelectronics
 * Author: Christian Bruel <christian.bruel@foss.st.com>
 */

#include <linux/clk.h>
#include <linux/mfd/syscon.h>
#include <linux/of_platform.h>
#include <linux/phy/phy.h>
#include <linux/pinctrl/devinfo.h>
#include <linux/pm_runtime.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/reset.h>
#include "pcie-designware.h"
#include "pcie-stm32.h"
#include "../../pci.h"

struct stm32_pcie {
	struct dw_pcie *pci;
	struct regmap *regmap;
	struct reset_control *rst;
	struct phy *phy;
	struct clk *clk;
	struct gpio_desc *perst_gpio;
	struct gpio_desc *wake_gpio;
	unsigned int wake_irq;
	bool link_is_up;
};

static int stm32_pcie_start_link(struct dw_pcie *pci)
{
	struct stm32_pcie *stm32_pcie = to_stm32_pcie(pci);
	u32 ret;

	if (stm32_pcie->perst_gpio) {
		/* Make sure PERST# is asserted. */
		gpiod_set_value(stm32_pcie->perst_gpio, 1);

		fsleep(PCIE_T_PERST_CLK_US);
		gpiod_set_value(stm32_pcie->perst_gpio, 0);
	}

	ret = regmap_update_bits(stm32_pcie->regmap, SYSCFG_PCIECR,
				 STM32MP25_PCIECR_LTSSM_EN,
				 STM32MP25_PCIECR_LTSSM_EN);

	if (stm32_pcie->perst_gpio)
		msleep(PCIE_T_RRS_READY_MS);

	return ret;
}

static void stm32_pcie_stop_link(struct dw_pcie *pci)
{
	struct stm32_pcie *stm32_pcie = to_stm32_pcie(pci);

	regmap_update_bits(stm32_pcie->regmap, SYSCFG_PCIECR,
			   STM32MP25_PCIECR_LTSSM_EN, 0);

	/* Assert PERST# */
	if (stm32_pcie->perst_gpio)
		gpiod_set_value(stm32_pcie->perst_gpio, 1);
}

static int stm32_pcie_suspend(struct device *dev)
{
	struct stm32_pcie *stm32_pcie = dev_get_drvdata(dev);

	if (device_may_wakeup(dev) || device_wakeup_path(dev))
		enable_irq_wake(stm32_pcie->wake_irq);

	return 0;
}

static int stm32_pcie_resume(struct device *dev)
{
	struct stm32_pcie *stm32_pcie = dev_get_drvdata(dev);

	if (device_may_wakeup(dev) || device_wakeup_path(dev))
		disable_irq_wake(stm32_pcie->wake_irq);

	return 0;
}

static int stm32_pcie_suspend_noirq(struct device *dev)
{
	struct stm32_pcie *stm32_pcie = dev_get_drvdata(dev);

	stm32_pcie->link_is_up = dw_pcie_link_up(stm32_pcie->pci);

	stm32_pcie_stop_link(stm32_pcie->pci);
	clk_disable_unprepare(stm32_pcie->clk);

	if (!device_may_wakeup(dev) && !device_wakeup_path(dev))
		phy_exit(stm32_pcie->phy);

	return pinctrl_pm_select_sleep_state(dev);
}

static int stm32_pcie_resume_noirq(struct device *dev)
{
	struct stm32_pcie *stm32_pcie = dev_get_drvdata(dev);
	struct dw_pcie *pci = stm32_pcie->pci;
	struct dw_pcie_rp *pp = &pci->pp;
	int ret;

	/* init_state must be called first to force clk_req# gpio when no
	 * device is plugged.
	 */
	if (!IS_ERR(dev->pins->init_state))
		ret = pinctrl_select_state(dev->pins->p, dev->pins->init_state);
	else
		ret = pinctrl_pm_select_default_state(dev);

	if (ret) {
		dev_err(dev, "Failed to activate pinctrl pm state: %d\n", ret);
		return ret;
	}

	if (!device_may_wakeup(dev) && !device_wakeup_path(dev)) {
		ret = phy_init(stm32_pcie->phy);
		if (ret) {
			pinctrl_pm_select_default_state(dev);
			return ret;
		}
	}

	ret = clk_prepare_enable(stm32_pcie->clk);
	if (ret)
		goto clk_err;

	ret = dw_pcie_setup_rc(pp);
	if (ret)
		goto pcie_err;

	if (stm32_pcie->link_is_up) {
		ret = stm32_pcie_start_link(stm32_pcie->pci);
		if (ret)
			goto pcie_err;

		/* Ignore errors, the link may come up later */
		dw_pcie_wait_for_link(stm32_pcie->pci);
	}

	pinctrl_pm_select_default_state(dev);

	return 0;

pcie_err:
	dw_pcie_host_deinit(pp);
	clk_disable_unprepare(stm32_pcie->clk);
clk_err:
	phy_exit(stm32_pcie->phy);
	pinctrl_pm_select_default_state(dev);

	return ret;
}

static const struct dev_pm_ops stm32_pcie_pm_ops = {
	NOIRQ_SYSTEM_SLEEP_PM_OPS(stm32_pcie_suspend_noirq,
				  stm32_pcie_resume_noirq)
	SYSTEM_SLEEP_PM_OPS(stm32_pcie_suspend, stm32_pcie_resume)
};

static const struct dw_pcie_host_ops stm32_pcie_host_ops = {
};

static const struct dw_pcie_ops dw_pcie_ops = {
	.start_link = stm32_pcie_start_link,
	.stop_link = stm32_pcie_stop_link
};

static irqreturn_t stm32_pcie_wake_irq_handler(int irq, void *priv)
{
	struct stm32_pcie *stm32_pcie = priv;
	struct device *dev = stm32_pcie->pci->dev;

	dev_dbg(dev, "PCIe host wakeup by EP");

	/* Notify PM core we are wakeup source */
	pm_wakeup_event(dev, 0);
	pm_system_wakeup();

	return IRQ_HANDLED;
}

static int stm32_add_pcie_port(struct stm32_pcie *stm32_pcie,
			       struct platform_device *pdev)
{
	struct dw_pcie *pci = stm32_pcie->pci;
	struct device *dev = pci->dev;
	struct dw_pcie_rp *pp = &pci->pp;
	int ret;

	ret = phy_set_mode(stm32_pcie->phy, PHY_MODE_PCIE);
	if (ret)
		return ret;

	ret = phy_init(stm32_pcie->phy);
	if (ret)
		return ret;

	ret = regmap_update_bits(stm32_pcie->regmap, SYSCFG_PCIECR,
				 STM32MP25_PCIECR_TYPE_MASK,
				 STM32MP25_PCIECR_RC);
	if (ret)
		goto phy_disable;

	reset_control_assert(stm32_pcie->rst);
	reset_control_deassert(stm32_pcie->rst);

	ret = clk_prepare_enable(stm32_pcie->clk);
	if (ret) {
		dev_err(dev, "Core clock enable failed %d\n", ret);
		goto phy_disable;
	}

	pp->ops = &stm32_pcie_host_ops;
	ret = dw_pcie_host_init(pp);
	if (ret) {
		dev_err(dev, "Failed to initialize host: %d\n", ret);
		clk_disable_unprepare(stm32_pcie->clk);
		goto phy_disable;
	}

	return 0;

phy_disable:
	phy_exit(stm32_pcie->phy);

	return ret;
}

static int stm32_pcie_probe(struct platform_device *pdev)
{
	struct stm32_pcie *stm32_pcie;
	struct dw_pcie *dw;
	struct device *dev = &pdev->dev;
	struct device_node *np = pdev->dev.of_node;
	int ret;

	stm32_pcie = devm_kzalloc(dev, sizeof(*stm32_pcie), GFP_KERNEL);
	if (!stm32_pcie)
		return -ENOMEM;

	dw = devm_kzalloc(dev, sizeof(*dw), GFP_KERNEL);
	if (!dw)
		return -ENOMEM;
	stm32_pcie->pci = dw;

	dw->dev = dev;
	dw->ops = &dw_pcie_ops;

	stm32_pcie->regmap = syscon_regmap_lookup_by_compatible("st,stm32mp25-syscfg");
	if (IS_ERR(stm32_pcie->regmap))
		return dev_err_probe(dev, PTR_ERR(stm32_pcie->regmap),
				     "No syscfg specified\n");

	stm32_pcie->phy = devm_phy_get(dev, "pcie-phy");
	if (IS_ERR(stm32_pcie->phy))
		return dev_err_probe(dev, PTR_ERR(stm32_pcie->phy),
				     "Failed to get pcie-phy\n");

	stm32_pcie->clk = devm_clk_get(dev, NULL);
	if (IS_ERR(stm32_pcie->clk))
		return dev_err_probe(dev, PTR_ERR(stm32_pcie->clk),
				     "Failed to get PCIe clock source\n");

	stm32_pcie->rst = devm_reset_control_get_exclusive(dev, NULL);
	if (IS_ERR(stm32_pcie->rst))
		return dev_err_probe(dev, PTR_ERR(stm32_pcie->rst),
				     "Failed to get PCIe reset\n");

	stm32_pcie->perst_gpio = devm_gpiod_get_optional(dev, "reset",
							 GPIOD_OUT_HIGH);
	if (IS_ERR(stm32_pcie->perst_gpio))
		return dev_err_probe(dev, PTR_ERR(stm32_pcie->perst_gpio),
				     "Failed to get reset GPIO\n");

	platform_set_drvdata(pdev, stm32_pcie);

	if (device_property_read_bool(dev, "wakeup-source")) {
		stm32_pcie->wake_gpio = devm_gpiod_get_optional(dev, "wake",
								GPIOD_IN);
		if (IS_ERR(stm32_pcie->wake_gpio))
			return dev_err_probe(dev, PTR_ERR(stm32_pcie->wake_gpio),
					     "Failed to get wake GPIO\n");
	}

	if (stm32_pcie->wake_gpio) {
		stm32_pcie->wake_irq = gpiod_to_irq(stm32_pcie->wake_gpio);

		ret = devm_request_threaded_irq(&pdev->dev,
						stm32_pcie->wake_irq, NULL,
						stm32_pcie_wake_irq_handler,
						IRQF_TRIGGER_FALLING | IRQF_ONESHOT,
						"wake_irq", stm32_pcie);

		if (ret)
			return dev_err_probe(dev, ret, "Failed to request WAKE IRQ: %d\n", ret);
	}

	ret = devm_pm_runtime_enable(dev);
	if (ret < 0) {
		dev_err(dev, "Failed to enable runtime PM %d\n", ret);
		return ret;
	}

	ret = pm_runtime_resume_and_get(dev);
	if (ret < 0) {
		dev_err(dev, "Failed to get runtime PM %d\n", ret);
		return ret;
	}

	ret = stm32_add_pcie_port(stm32_pcie, pdev);
	if (ret)  {
		pm_runtime_put_sync(&pdev->dev);
		return ret;
	}

	if (stm32_pcie->wake_gpio)
		device_set_wakeup_capable(dev, true);

	return 0;
}

static void stm32_pcie_remove(struct platform_device *pdev)
{
	struct stm32_pcie *stm32_pcie = platform_get_drvdata(pdev);
	struct dw_pcie_rp *pp = &stm32_pcie->pci->pp;

	if (stm32_pcie->wake_gpio)
		device_init_wakeup(&pdev->dev, false);

	dw_pcie_host_deinit(pp);
	clk_disable_unprepare(stm32_pcie->clk);

	phy_exit(stm32_pcie->phy);

	pm_runtime_put_sync(&pdev->dev);
}

static const struct of_device_id stm32_pcie_of_match[] = {
	{ .compatible = "st,stm32mp25-pcie-rc" },
	{},
};

static struct platform_driver stm32_pcie_driver = {
	.probe = stm32_pcie_probe,
	.remove = stm32_pcie_remove,
	.driver = {
		.name = "stm32-pcie",
		.of_match_table = stm32_pcie_of_match,
		.pm		= &stm32_pcie_pm_ops,
	},
};

static bool is_stm32_pcie_driver(struct device *dev)
{
	/* PCI bridge */
	dev = get_device(dev);

	/* Platform driver */
	dev = get_device(dev->parent);

	return (dev->driver == &stm32_pcie_driver.driver);
}

/*
 * DMA masters can only access the first 4GB of memory space,
 * so we setup the bus DMA limit accordingly.
 */
static int stm32_dma_limit(struct pci_dev *pdev, void *data)
{
	dev_dbg(&pdev->dev, "disabling DMA DAC for device");

	pdev->dev.bus_dma_limit = DMA_BIT_MASK(32);

	return 0;
}

static void quirk_stm32_dma_mask(struct pci_dev *pci)
{
	struct pci_dev *root_port;

	root_port = pcie_find_root_port(pci);

	if (root_port && is_stm32_pcie_driver(root_port->dev.parent))
		pci_walk_bus(pci->bus, stm32_dma_limit, NULL);
}
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_SYNOPSYS, 0x0550, quirk_stm32_dma_mask);

module_platform_driver(stm32_pcie_driver);

MODULE_AUTHOR("Christian Bruel <christian.bruel@foss.st.com>");
MODULE_DESCRIPTION("STM32MP25 PCIe Controller driver");
MODULE_LICENSE("GPL");
MODULE_DEVICE_TABLE(of, stm32_pcie_of_match);
