// SPDX-License-Identifier: GPL-2.0
/*
 * Author: Christian Marangi <ansuelsmth@gmail.com>
 */

#include <dt-bindings/soc/airoha,scu-ssr.h>
#include <linux/bitfield.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/soc/airoha/airoha-scu-ssr.h>

#define AIROHA_SCU_PCIC			0x88
#define   AIROHA_SCU_PCIE_2LANE_MODE	BIT(14)

#define AIROHA_SCU_SSR3			0x94
#define   AIROHA_SCU_SSUSB_HSGMII_SEL	BIT(29)

#define AIROHA_SCU_SSTR			0x9c
#define   AIROHA_SCU_PCIE_XSI0_SEL	GENMASK(14, 13)
#define   AIROHA_SCU_PCIE_XSI0_SEL_PCIE	FIELD_PREP_CONST(AIROHA_SCU_PCIE_XSI0_SEL, 0x0)
#define   AIROHA_SCU_PCIE_XSI1_SEL	GENMASK(12, 11)
#define   AIROHA_SCU_PCIE_XSI1_SEL_PCIE	FIELD_PREP_CONST(AIROHA_SCU_PCIE_XSI0_SEL, 0x0)
#define   AIROHA_SCU_USB_PCIE_SEL	BIT(3)

struct airoha_scu_ssr_priv {
	struct device *dev;
	struct regmap *regmap;

	u32 serdes_port[AIROHA_SCU_MAX_SERDES_PORT];
};

int airoha_scu_ssr_get_serdes_mode(struct device *dev,
				   enum airoha_scu_serdes_port port)
{
	struct airoha_scu_ssr_priv *priv;
	struct platform_device *pdev;
	struct device_node *np;

	np = of_parse_phandle(dev->of_node, "airoha,scu-ssr", 0);
	if (!np)
		return -ENODEV;

	if (!of_device_is_available(np)) {
		of_node_put(np);
		return -ENODEV;
	}

	pdev = of_find_device_by_node(np);
	of_node_put(np);
	if (!pdev || !platform_get_drvdata(pdev)) {
		if (pdev)
			put_device(&pdev->dev);
		return -EPROBE_DEFER;
	}

	priv = platform_get_drvdata(pdev);

	return priv->serdes_port[port];
}
EXPORT_SYMBOL_GPL(airoha_scu_ssr_get_serdes_mode);

static int airoha_scu_ssr_apply_modes(struct airoha_scu_ssr_priv *priv)
{
	int ret;

	/*
	 * This is a very bad scenario and needs to be correctly warned
	 * as it cause PCIe malfunction
	 */
	if ((priv->serdes_port[AIROHA_SCU_SERDES_WIFI1] == AIROHA_SCU_SSR_WIFI1_PCIE0_2LINE &&
	     priv->serdes_port[AIROHA_SCU_SERDES_WIFI2] != AIROHA_SCU_SSR_WIFI2_PCIE0_2LINE) ||
	    (priv->serdes_port[AIROHA_SCU_SERDES_WIFI1] != AIROHA_SCU_SSR_WIFI1_PCIE0_2LINE &&
	     priv->serdes_port[AIROHA_SCU_SERDES_WIFI2] == AIROHA_SCU_SSR_WIFI2_PCIE0_2LINE)) {
		WARN(true, "Wrong Serdes configuration for PCIe0 2 Line mode. Please check DT.\n");
		return -EINVAL;
	}

	/* PCS driver takes case of setting the SCU bit for HSGMII or USXGMII */
	if (priv->serdes_port[AIROHA_SCU_SERDES_WIFI1] == AIROHA_SCU_SSR_WIFI1_PCIE0_2LINE ||
	    priv->serdes_port[AIROHA_SCU_SERDES_WIFI1] == AIROHA_SCU_SSR_WIFI1_PCIE0) {
		ret = regmap_update_bits(priv->regmap, AIROHA_SCU_SSTR,
					 AIROHA_SCU_PCIE_XSI0_SEL,
					 AIROHA_SCU_PCIE_XSI0_SEL_PCIE);
		if (ret)
			return ret;
	}

	/* PCS driver takes case of setting the SCU bit for HSGMII or USXGMII */
	if (priv->serdes_port[AIROHA_SCU_SERDES_WIFI2] == AIROHA_SCU_SSR_WIFI2_PCIE0_2LINE ||
	    priv->serdes_port[AIROHA_SCU_SERDES_WIFI2] == AIROHA_SCU_SSR_WIFI2_PCIE1) {
		ret = regmap_update_bits(priv->regmap, AIROHA_SCU_SSTR,
					 AIROHA_SCU_PCIE_XSI1_SEL,
					 AIROHA_SCU_PCIE_XSI1_SEL_PCIE);
		if (ret)
			return ret;
	}

	/* Toggle PCIe0 2 Line mode if enabled or not */
	if (priv->serdes_port[AIROHA_SCU_SERDES_WIFI1] == AIROHA_SCU_SSR_WIFI1_PCIE0_2LINE)
		ret = regmap_set_bits(priv->regmap, AIROHA_SCU_PCIC,
				      AIROHA_SCU_PCIE_2LANE_MODE);
	else
		ret = regmap_clear_bits(priv->regmap, AIROHA_SCU_PCIC,
					AIROHA_SCU_PCIE_2LANE_MODE);
	if (ret)
		return ret;

	if (priv->serdes_port[AIROHA_SCU_SERDES_USB1] == AIROHA_SCU_SSR_USB1_ETHERNET)
		ret = regmap_clear_bits(priv->regmap, AIROHA_SCU_SSR3,
					AIROHA_SCU_SSUSB_HSGMII_SEL);
	else
		ret = regmap_set_bits(priv->regmap, AIROHA_SCU_SSR3,
				      AIROHA_SCU_SSUSB_HSGMII_SEL);
	if (ret)
		return ret;

	if (priv->serdes_port[AIROHA_SCU_SERDES_USB2] == AIROHA_SCU_SSR_USB2_PCIE2)
		ret = regmap_clear_bits(priv->regmap, AIROHA_SCU_SSTR,
					AIROHA_SCU_USB_PCIE_SEL);
	else
		ret = regmap_set_bits(priv->regmap, AIROHA_SCU_SSTR,
				      AIROHA_SCU_USB_PCIE_SEL);
	if (ret)
		return ret;

	return 0;
}

static int airoha_scu_ssr_probe(struct platform_device *pdev)
{
	struct airoha_scu_ssr_priv *priv;
	struct device *dev = &pdev->dev;
	int ret;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->dev = dev;

	/* Get regmap from MFD */
	priv->regmap = dev_get_regmap(dev->parent, NULL);
	if (!priv->regmap)
		return -EINVAL;

	/* If not set, default to PCIE0 1 line */
	if (of_property_read_u32(dev->of_node, "airoha,serdes-wifi1",
				 &priv->serdes_port[AIROHA_SCU_SERDES_WIFI1]))
		priv->serdes_port[AIROHA_SCU_SERDES_WIFI1] = AIROHA_SCU_SSR_WIFI1_PCIE0;

	/* If not set, default to PCIE1 1 line */
	if (of_property_read_u32(dev->of_node, "airoha,serdes-wifi2",
				 &priv->serdes_port[AIROHA_SCU_SERDES_WIFI2]))
		priv->serdes_port[AIROHA_SCU_SERDES_WIFI1] = AIROHA_SCU_SSR_WIFI2_PCIE1;

	/* If not set, default to USB1 USB 3.0 */
	if (of_property_read_u32(dev->of_node, "airoha,serdes-usb1",
				 &priv->serdes_port[AIROHA_SCU_SERDES_USB1]))
		priv->serdes_port[AIROHA_SCU_SERDES_WIFI1] = AIROHA_SCU_SSR_USB1_USB;

	/* If not set, default to USB2 USB 3.0 */
	if (of_property_read_u32(dev->of_node, "airoha,serdes-usb2",
				 &priv->serdes_port[AIROHA_SCU_SERDES_USB2]))
		priv->serdes_port[AIROHA_SCU_SERDES_WIFI1] = AIROHA_SCU_SSR_USB2_USB;

	ret = airoha_scu_ssr_apply_modes(priv);
	if (ret)
		return ret;

	platform_set_drvdata(pdev, priv);

	return 0;
}

static const struct of_device_id airoha_phy_id_table[] = {
	{ .compatible = "airoha,an7581-scu-ssr" },
	{ },
};
MODULE_DEVICE_TABLE(of, airoha_phy_id_table);

static struct platform_driver airoha_scu_ssr_driver = {
	.probe		= airoha_scu_ssr_probe,
	.driver		= {
		.name	= "airoha-scu-ssr",
		.of_match_table = airoha_phy_id_table,
	},
};

module_platform_driver(airoha_scu_ssr_driver);

MODULE_AUTHOR("Christian Marangi <ansuelsmth@gmail.com>");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Airoha SCU SSR/STR driver");
