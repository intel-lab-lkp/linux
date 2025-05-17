// SPDX-License-Identifier: GPL-2.0-only
/*
 * SpacemiT K1 PCIE/USB3 PHY driver
 *
 * Copyright (C) 2025 SpacemiT (Hangzhou) Technology Co. Ltd
 * Copyright (C) 2025 Ze Huang <huangze@whut.edu.cn>
 */

#include <dt-bindings/phy/phy.h>
#include <linux/clk.h>
#include <linux/iopoll.h>
#include <linux/phy/phy.h>
#include <linux/platform_device.h>
#include <linux/reset.h>
#include <linux/usb/of.h>

#define COMBPHY_USB_REG1		0x68
#define  COMBPHY_USB_REG1_VAL		0x00
#define COMBPHY_USB_REG2		0x48
#define  COMBPHY_USB_REG2_VAL		0x603a2276
#define COMBPHY_USB_REG3		0x08
#define  COMBPHY_USB_REG3_VAL		0x97c
#define COMBPHY_USB_REG4		0x18
#define  COMBPHY_USB_REG4_VAL		0x00
#define  COMBPHY_USB_TERM_SHORT_MASK	0x3000
#define  COMBPHY_USB_TERM_SHORT_VAL	0x3000
#define COMBPHY_USB_PLL_REG		0x08
#define  COMBPHY_USB_PLL_MASK		0x01
#define  COMBPHY_USB_PLL_VAL		0x01
#define COMBPHY_USB_LFPS_REG		0x58
#define  COMBPHY_USB_LFPS_MASK		0x700
#define  COMBPHY_USB_LFPS_THRES_DEFAULT	0x03

#define COMBPHY_MODE_SEL	BIT(3)
#define COMBPHY_WAIT_TIMEOUT	1000

struct spacemit_combphy_priv {
	struct device *dev;
	struct phy *phy;
	struct reset_control *phy_rst;
	void __iomem *phy_ctrl;
	void __iomem *phy_sel;
	bool rx_always_on;
	u8 lfps_threshold;
	u8 type;
};

static void spacemit_reg_update(void __iomem *reg, u32 offset, u32 mask, u32 val)
{
	u32 tmp;

	tmp = readl(reg + offset);
	tmp = (tmp & ~(mask)) | val;
	writel(tmp, reg + offset);
}

static int spacemit_combphy_wait_ready(struct spacemit_combphy_priv *priv,
				       u32 offset, u32 mask, u32 val)
{
	u32 reg_val;

	return read_poll_timeout(readl, reg_val, (reg_val & mask) == val,
				 1000, COMBPHY_WAIT_TIMEOUT * 1000, false,
				 priv->phy_ctrl + offset);
}

static int spacemit_combphy_set_mode(struct spacemit_combphy_priv *priv)
{
	int ret = 0;

	switch (priv->type) {
	case PHY_TYPE_USB3:
		spacemit_reg_update(priv->phy_sel, 0, 0, COMBPHY_MODE_SEL);
		break;
	default:
		dev_err(priv->dev, "PHY type %x not supported\n", priv->type);
		ret = -EINVAL;
		break;
	}

	return ret;
}

static int spacemit_combphy_init_usb(struct spacemit_combphy_priv *priv)
{
	void __iomem *base = priv->phy_ctrl;
	int ret;

	writel(COMBPHY_USB_REG1_VAL, base + COMBPHY_USB_REG1);
	writel(COMBPHY_USB_REG2_VAL, base + COMBPHY_USB_REG2);
	writel(COMBPHY_USB_REG3_VAL, base + COMBPHY_USB_REG3);
	writel(COMBPHY_USB_REG4_VAL, base + COMBPHY_USB_REG4);

	ret = spacemit_combphy_wait_ready(priv, COMBPHY_USB_PLL_REG,
					  COMBPHY_USB_PLL_MASK,
					  COMBPHY_USB_PLL_VAL);

	dev_dbg(priv->dev, "USB3 PHY init lfps threshold %d\n", priv->lfps_threshold);
	spacemit_reg_update(base, COMBPHY_USB_LFPS_REG,
			    COMBPHY_USB_LFPS_MASK,
			    (priv->lfps_threshold << 8));

	if (priv->rx_always_on)
		spacemit_reg_update(base, COMBPHY_USB_REG4,
				    COMBPHY_USB_TERM_SHORT_MASK,
				    COMBPHY_USB_TERM_SHORT_VAL);

	if (ret)
		dev_err(priv->dev, "USB3 PHY init timeout!\n");

	return ret;
}

static int spacemit_combphy_init(struct phy *phy)
{
	struct spacemit_combphy_priv *priv = phy_get_drvdata(phy);
	int ret;

	ret = spacemit_combphy_set_mode(priv);
	if (ret) {
		dev_err(priv->dev, "failed to set mode for PHY type %x\n",
			priv->type);
		goto out;
	}

	ret = reset_control_deassert(priv->phy_rst);
	if (ret) {
		dev_err(priv->dev, "failed to deassert rst\n");
		goto err_rst;
	}

	switch (priv->type) {
	case PHY_TYPE_USB3:
		ret = spacemit_combphy_init_usb(priv);
		break;
	default:
		dev_err(priv->dev, "PHY type %x not supported\n", priv->type);
		ret = -EINVAL;
		break;
	}

	if (ret)
		goto err_rst;

	return 0;

err_rst:
	reset_control_assert(priv->phy_rst);
out:
	return ret;
}

static int spacemit_combphy_exit(struct phy *phy)
{
	struct spacemit_combphy_priv *priv = phy_get_drvdata(phy);

	reset_control_assert(priv->phy_rst);

	return 0;
}

static struct phy *spacemit_combphy_xlate(struct device *dev,
					  const struct of_phandle_args *args)
{
	struct spacemit_combphy_priv *priv = dev_get_drvdata(dev);

	if (args->args_count != 1) {
		dev_err(dev, "invalid number of arguments\n");
		return ERR_PTR(-EINVAL);
	}

	if (priv->type != PHY_NONE && priv->type != args->args[0])
		dev_warn(dev, "PHY type %d is selected to override %d\n",
			 args->args[0], priv->type);

	priv->type = args->args[0];

	if (args->args_count > 1)
		dev_dbg(dev, "combo phy idx: %d selected",  args->args[1]);

	return priv->phy;
}

static const struct phy_ops spacemit_combphy_ops = {
	.init = spacemit_combphy_init,
	.exit = spacemit_combphy_exit,
	.owner = THIS_MODULE,
};

static int spacemit_combphy_probe(struct platform_device *pdev)
{
	struct spacemit_combphy_priv *priv;
	struct phy_provider *phy_provider;
	struct device *dev = &pdev->dev;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->phy_ctrl = devm_platform_ioremap_resource_byname(pdev, "ctrl");
	if (IS_ERR(priv->phy_ctrl))
		return PTR_ERR(priv->phy_ctrl);

	priv->phy_sel = devm_platform_ioremap_resource_byname(pdev, "sel");
	if (IS_ERR(priv->phy_sel))
		return PTR_ERR(priv->phy_sel);

	priv->lfps_threshold = COMBPHY_USB_LFPS_THRES_DEFAULT;
	device_property_read_u8(&pdev->dev, "spacemit,lfps-threshold", &priv->lfps_threshold);

	priv->rx_always_on = device_property_read_bool(&pdev->dev, "spacemit,rx-always-on");
	priv->type = PHY_NONE;
	priv->dev = dev;

	priv->phy_rst = devm_reset_control_get_exclusive(dev, NULL);
	if (IS_ERR(priv->phy_rst))
		return dev_err_probe(dev, PTR_ERR(priv->phy_rst),
				     "failed to get phy reset\n");

	priv->phy = devm_phy_create(dev, NULL, &spacemit_combphy_ops);
	if (IS_ERR(priv->phy))
		return dev_err_probe(dev, PTR_ERR(priv->phy),
				     "failed to create combphy\n");

	dev_set_drvdata(dev, priv);
	phy_set_drvdata(priv->phy, priv);
	phy_provider = devm_of_phy_provider_register(dev, spacemit_combphy_xlate);

	return PTR_ERR_OR_ZERO(phy_provider);
}

static const struct of_device_id spacemit_combphy_of_match[] = {
	{ .compatible = "spacemit,k1-combphy", },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, spacemit_combphy_of_match);

static struct platform_driver spacemit_combphy_driver = {
	.probe	= spacemit_combphy_probe,
	.driver = {
		.name = "spacemit-k1-combphy",
		.of_match_table = spacemit_combphy_of_match,
	},
};
module_platform_driver(spacemit_combphy_driver);

MODULE_DESCRIPTION("Spacemit PCIE/USB3.0 COMBO PHY driver");
MODULE_LICENSE("GPL");
