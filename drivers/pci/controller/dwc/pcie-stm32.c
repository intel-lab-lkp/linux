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
	struct dw_pcie pci;
	struct regmap *regmap;
	struct reset_control *rst;
	struct phy *phy;
	struct clk *clk;
	struct gpio_desc *perst_gpio;
	struct gpio_desc *wake_gpio;
	unsigned int wake_irq;
};

static void stm32_pcie_deassert_perst(struct stm32_pcie *stm32_pcie)
{
	gpiod_set_value(stm32_pcie->perst_gpio, 0);

	if (stm32_pcie->perst_gpio)
		msleep(PCIE_T_RRS_READY_MS);
}

static void stm32_pcie_assert_perst(struct stm32_pcie *stm32_pcie)
{
	gpiod_set_value(stm32_pcie->perst_gpio, 1);
}

static int stm32_pcie_start_link(struct dw_pcie *pci)
{
	struct stm32_pcie *stm32_pcie = to_stm32_pcie(pci);

	return regmap_update_bits(stm32_pcie->regmap, SYSCFG_PCIECR,
				  STM32MP25_PCIECR_LTSSM_EN,
				  STM32MP25_PCIECR_LTSSM_EN);
}

static void stm32_pcie_stop_link(struct dw_pcie *pci)
{
	struct stm32_pcie *stm32_pcie = to_stm32_pcie(pci);

	regmap_update_bits(stm32_pcie->regmap, SYSCFG_PCIECR,
			   STM32MP25_PCIECR_LTSSM_EN, 0);
}

static int stm32_pcie_suspend(struct device *dev)
{
	struct stm32_pcie *stm32_pcie = dev_get_drvdata(dev);

	if (device_may_wakeup(dev))
		enable_irq_wake(stm32_pcie->wake_irq);

	return 0;
}

static int stm32_pcie_resume(struct device *dev)
{
	struct stm32_pcie *stm32_pcie = dev_get_drvdata(dev);

	if (device_may_wakeup(dev))
		disable_irq_wake(stm32_pcie->wake_irq);

	return 0;
}

static int stm32_pcie_suspend_noirq(struct device *dev)
{
	struct stm32_pcie *stm32_pcie = dev_get_drvdata(dev);

	stm32_pcie_stop_link(&stm32_pcie->pci);

	stm32_pcie_assert_perst(stm32_pcie);

	clk_disable_unprepare(stm32_pcie->clk);

	if (!device_may_wakeup(dev))
		phy_exit(stm32_pcie->phy);

	return pinctrl_pm_select_sleep_state(dev);
}

static int stm32_pcie_resume_noirq(struct device *dev)
{
	struct stm32_pcie *stm32_pcie = dev_get_drvdata(dev);
	struct dw_pcie_rp *pp = &stm32_pcie->pci.pp;
	int ret;

	/*
	 * The core clock is gated with CLKREQ# from the COMBOPHY REFCLK,
	 * thus if no device is present, must force it low with an init pinmux
	 * to be able to access the DBI registers.
	 */
	if (!IS_ERR(dev->pins->init_state))
		ret = pinctrl_select_state(dev->pins->p, dev->pins->init_state);
	else
		ret = pinctrl_pm_select_default_state(dev);

	if (ret) {
		dev_err(dev, "Failed to activate pinctrl pm state: %d\n", ret);
		return ret;
	}

	if (!device_may_wakeup(dev)) {
		ret = phy_init(stm32_pcie->phy);
		if (ret) {
			pinctrl_pm_select_default_state(dev);
			return ret;
		}
	}

	ret = clk_prepare_enable(stm32_pcie->clk);
	if (ret)
		goto err_phy_exit;

	stm32_pcie_deassert_perst(stm32_pcie);

	ret = dw_pcie_setup_rc(pp);
	if (ret)
		goto err_disable_clk;

	ret = stm32_pcie_start_link(&stm32_pcie->pci);
	if (ret)
		goto err_disable_clk;

	/* Ignore errors, the link may come up later */
	dw_pcie_wait_for_link(&stm32_pcie->pci);

	pinctrl_pm_select_default_state(dev);

	return 0;

err_disable_clk:
	stm32_pcie_assert_perst(stm32_pcie);
	clk_disable_unprepare(stm32_pcie->clk);

err_phy_exit:
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

static int stm32_add_pcie_port(struct stm32_pcie *stm32_pcie,
			       struct platform_device *pdev)
{
	struct device *dev = stm32_pcie->pci.dev;
	struct dw_pcie_rp *pp = &stm32_pcie->pci.pp;
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
		goto err_phy_exit;

	reset_control_assert(stm32_pcie->rst);
	reset_control_deassert(stm32_pcie->rst);

	ret = clk_prepare_enable(stm32_pcie->clk);
	if (ret) {
		dev_err(dev, "Core clock enable failed %d\n", ret);
		goto err_phy_exit;
	}

	stm32_pcie_deassert_perst(stm32_pcie);

	pp->ops = &stm32_pcie_host_ops;
	ret = dw_pcie_host_init(pp);
	if (ret) {
		dev_err(dev, "Failed to initialize host: %d\n", ret);
		goto err_disable_clk;
	}

	return 0;

err_disable_clk:
	clk_disable_unprepare(stm32_pcie->clk);
	stm32_pcie_assert_perst(stm32_pcie);

err_phy_exit:
	phy_exit(stm32_pcie->phy);

	return ret;
}

static int stm32_pcie_probe(struct platform_device *pdev)
{
	struct stm32_pcie *stm32_pcie;
	struct device *dev = &pdev->dev;
	struct device_node *root_port;
	int ret;

	stm32_pcie = devm_kzalloc(dev, sizeof(*stm32_pcie), GFP_KERNEL);
	if (!stm32_pcie)
		return -ENOMEM;

	stm32_pcie->pci.dev = dev;
	stm32_pcie->pci.ops = &dw_pcie_ops;

	stm32_pcie->regmap = syscon_regmap_lookup_by_compatible("st,stm32mp25-syscfg");
	if (IS_ERR(stm32_pcie->regmap))
		return dev_err_probe(dev, PTR_ERR(stm32_pcie->regmap),
				     "No syscfg specified\n");

	root_port = of_get_next_available_child(dev->of_node, NULL);
	stm32_pcie->phy = devm_of_phy_get(dev, root_port, "pcie-phy");
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
						dw_pcie_wake_irq_handler,
						IRQF_TRIGGER_FALLING | IRQF_ONESHOT,
						"wake_irq", stm32_pcie->pci.dev);

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
	struct dw_pcie_rp *pp = &stm32_pcie->pci.pp;

	if (stm32_pcie->wake_gpio)
		device_init_wakeup(&pdev->dev, false);

	dw_pcie_host_deinit(pp);

	stm32_pcie_assert_perst(stm32_pcie);

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
		.pm = &stm32_pcie_pm_ops,
		.probe_type = PROBE_PREFER_ASYNCHRONOUS,
	},
};

module_platform_driver(stm32_pcie_driver);

MODULE_AUTHOR("Christian Bruel <christian.bruel@foss.st.com>");
MODULE_DESCRIPTION("STM32MP25 PCIe Controller driver");
MODULE_LICENSE("GPL");
MODULE_DEVICE_TABLE(of, stm32_pcie_of_match);
