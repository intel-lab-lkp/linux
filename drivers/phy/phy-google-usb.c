// SPDX-License-Identifier: GPL-2.0
/*
 * phy-google-usb.c - Google USB PHY driver
 *
 * Copyright (C) 2025, Google LLC
 */

#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/reset.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/phy/phy.h>
#include <linux/platform_device.h>
#include <linux/mutex.h>
#include <linux/cleanup.h>
#include <linux/usb/typec_mux.h>

#define USBCS_USB2PHY_CFG19_OFFSET 0x0
#define USBCS_USB2PHY_CFG19_PHY_CFG_PLL_FB_DIV GENMASK(19, 8)

#define USBCS_USB2PHY_CFG21_OFFSET 0x8
#define USBCS_USB2PHY_CFG21_PHY_ENABLE BIT(12)
#define USBCS_USB2PHY_CFG21_REF_FREQ_SEL GENMASK(15, 13)
#define USBCS_USB2PHY_CFG21_PHY_TX_DIG_BYPASS_SEL BIT(19)

#define USBCS_PHY_CFG1_OFFSET 0x28
#define USBCS_PHY_CFG1_SYS_VBUSVALID BIT(17)

enum google_usb_phy_id {
	GOOGLE_USB2_PHY,
	GOOGLE_USB_PHY_NUM,
};

struct google_usb_phy_instance {
	int index;
	struct phy *phy;
	int num_clks;
	struct clk_bulk_data *clks;
	struct reset_control *rsts;
};

struct google_usb_phy {
	struct device *dev;
	void __iomem *usb2_cfg_base;
	void __iomem *usb3_top_base;
	struct google_usb_phy_instance insts[GOOGLE_USB_PHY_NUM];
	/* serialize phy access */
	struct mutex phy_mutex;
	struct typec_switch_dev *sw;
	enum typec_orientation orientation;
};

static inline struct google_usb_phy *to_google_usb_phy(struct google_usb_phy_instance *inst)
{
	return container_of(inst, struct google_usb_phy, insts[inst->index]);
}

static void set_vbus_valid(struct google_usb_phy *gphy)
{
	u32 reg;

	if (gphy->orientation == TYPEC_ORIENTATION_NONE) {
		reg = readl(gphy->usb3_top_base + USBCS_PHY_CFG1_OFFSET);
		reg &= ~USBCS_PHY_CFG1_SYS_VBUSVALID;
		writel(reg, gphy->usb3_top_base + USBCS_PHY_CFG1_OFFSET);
	} else {
		reg = readl(gphy->usb3_top_base + USBCS_PHY_CFG1_OFFSET);
		reg |= USBCS_PHY_CFG1_SYS_VBUSVALID;
		writel(reg, gphy->usb3_top_base + USBCS_PHY_CFG1_OFFSET);
	}
}

static int google_usb_set_orientation(struct typec_switch_dev *sw,
				      enum typec_orientation orientation)
{
	struct google_usb_phy *gphy = typec_switch_get_drvdata(sw);

	dev_dbg(gphy->dev, "set orientation %d\n", orientation);

	gphy->orientation = orientation;

	if (pm_runtime_suspended(gphy->dev))
		return 0;

	guard(mutex)(&gphy->phy_mutex);

	set_vbus_valid(gphy);

	return 0;
}

static int google_usb2_phy_init(struct phy *_phy)
{
	struct google_usb_phy_instance *inst = phy_get_drvdata(_phy);
	struct google_usb_phy *gphy = to_google_usb_phy(inst);
	u32 reg;
	int ret = 0;

	dev_dbg(gphy->dev, "initializing usb2 phy\n");

	guard(mutex)(&gphy->phy_mutex);

	reg = readl(gphy->usb2_cfg_base + USBCS_USB2PHY_CFG21_OFFSET);
	reg &= ~USBCS_USB2PHY_CFG21_PHY_TX_DIG_BYPASS_SEL;
	reg &= ~USBCS_USB2PHY_CFG21_REF_FREQ_SEL;
	reg |= FIELD_PREP(USBCS_USB2PHY_CFG21_REF_FREQ_SEL, 0);
	writel(reg, gphy->usb2_cfg_base + USBCS_USB2PHY_CFG21_OFFSET);

	reg = readl(gphy->usb2_cfg_base + USBCS_USB2PHY_CFG19_OFFSET);
	reg &= ~USBCS_USB2PHY_CFG19_PHY_CFG_PLL_FB_DIV;
	reg |= FIELD_PREP(USBCS_USB2PHY_CFG19_PHY_CFG_PLL_FB_DIV, 368);
	writel(reg, gphy->usb2_cfg_base + USBCS_USB2PHY_CFG19_OFFSET);

	set_vbus_valid(gphy);

	ret = clk_bulk_prepare_enable(inst->num_clks, inst->clks);
	if (ret)
		return ret;

	ret = reset_control_deassert(inst->rsts);
	if (ret) {
		clk_bulk_disable_unprepare(inst->num_clks, inst->clks);
		return ret;
	}

	reg = readl(gphy->usb2_cfg_base + USBCS_USB2PHY_CFG21_OFFSET);
	reg |= USBCS_USB2PHY_CFG21_PHY_ENABLE;
	writel(reg, gphy->usb2_cfg_base + USBCS_USB2PHY_CFG21_OFFSET);

	return ret;
}

static int google_usb2_phy_exit(struct phy *_phy)
{
	struct google_usb_phy_instance *inst = phy_get_drvdata(_phy);
	struct google_usb_phy *gphy = to_google_usb_phy(inst);
	u32 reg;

	dev_dbg(gphy->dev, "exiting usb2 phy\n");

	guard(mutex)(&gphy->phy_mutex);

	reg = readl(gphy->usb2_cfg_base + USBCS_USB2PHY_CFG21_OFFSET);
	reg &= ~USBCS_USB2PHY_CFG21_PHY_ENABLE;
	writel(reg, gphy->usb2_cfg_base + USBCS_USB2PHY_CFG21_OFFSET);

	reset_control_assert(inst->rsts);
	clk_bulk_disable_unprepare(inst->num_clks, inst->clks);

	return 0;
}

static const struct phy_ops google_usb2_phy_ops = {
	.init		= google_usb2_phy_init,
	.exit		= google_usb2_phy_exit,
};

static struct phy *google_usb_phy_xlate(struct device *dev,
					const struct of_phandle_args *args)
{
	struct google_usb_phy *gphy = dev_get_drvdata(dev);

	if (args->args[0] >= GOOGLE_USB_PHY_NUM) {
		dev_err(dev, "invalid PHY index requested from DT\n");
		return ERR_PTR(-ENODEV);
	}
	return gphy->insts[args->args[0]].phy;
}

static int google_usb_phy_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct google_usb_phy *gphy;
	struct phy *phy;
	struct google_usb_phy_instance *inst;
	struct phy_provider *phy_provider;
	struct typec_switch_desc sw_desc = { };
	int ret;

	gphy = devm_kzalloc(dev, sizeof(*gphy), GFP_KERNEL);
	if (!gphy)
		return -ENOMEM;

	dev_set_drvdata(dev, gphy);
	gphy->dev = dev;

	ret = devm_mutex_init(dev, &gphy->phy_mutex);
	if (ret)
		return ret;

	gphy->usb2_cfg_base = devm_platform_ioremap_resource_byname(pdev,
								    "usb2_cfg");
	if (IS_ERR(gphy->usb2_cfg_base))
		return dev_err_probe(dev, PTR_ERR(gphy->usb2_cfg_base),
				    "invalid usb2 cfg\n");

	gphy->usb3_top_base = devm_platform_ioremap_resource_byname(pdev,
								    "usb3_top");
	if (IS_ERR(gphy->usb3_top_base))
		return dev_err_probe(dev, PTR_ERR(gphy->usb3_top_base),
				    "invalid usb3 top\n");

	inst = &gphy->insts[GOOGLE_USB2_PHY];
	inst->index = GOOGLE_USB2_PHY;
	phy = devm_phy_create(dev, NULL, &google_usb2_phy_ops);
	if (IS_ERR(phy))
		return dev_err_probe(dev, PTR_ERR(phy),
				     "failed to create usb2 phy instance\n");
	inst->phy = phy;
	phy_set_drvdata(phy, inst);
	ret = devm_clk_bulk_get_all_enabled(dev, &inst->clks);
	if (ret < 0)
		return dev_err_probe(dev, ret, "failed to get u2 phy clks\n");
	inst->num_clks = ret;

	inst->rsts = devm_reset_control_array_get_exclusive(dev);
	if (IS_ERR(inst->rsts))
		return dev_err_probe(dev, PTR_ERR(inst->rsts),
				     "failed to get u2 phy resets\n");

	phy_provider = devm_of_phy_provider_register(dev, google_usb_phy_xlate);
	if (IS_ERR(phy_provider))
		return dev_err_probe(dev, PTR_ERR(phy_provider),
				     "failed to register phy provider\n");

	pm_runtime_enable(dev);

	sw_desc.fwnode = dev_fwnode(dev);
	sw_desc.drvdata = gphy;
	sw_desc.name = fwnode_get_name(dev_fwnode(dev));
	sw_desc.set = google_usb_set_orientation;

	gphy->sw = typec_switch_register(dev, &sw_desc);
	if (IS_ERR(gphy->sw))
		return dev_err_probe(dev, PTR_ERR(gphy->sw),
				     "failed to register typec switch\n");

	return 0;
}

static void google_usb_phy_remove(struct platform_device *pdev)
{
	struct google_usb_phy *gphy = dev_get_drvdata(&pdev->dev);

	typec_switch_unregister(gphy->sw);
	pm_runtime_disable(&pdev->dev);
}

static const struct of_device_id google_usb_phy_of_match[] = {
	{
		.compatible = "google,gs5-usb-phy",
	},
	{ }
};
MODULE_DEVICE_TABLE(of, google_usb_phy_of_match);

static struct platform_driver google_usb_phy = {
	.probe	= google_usb_phy_probe,
	.remove = google_usb_phy_remove,
	.driver = {
		.name		= "google-usb-phy",
		.of_match_table	= google_usb_phy_of_match,
	}
};

module_platform_driver(google_usb_phy);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Google USB phy driver");
