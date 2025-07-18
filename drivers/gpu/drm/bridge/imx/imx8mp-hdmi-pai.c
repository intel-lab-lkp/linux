// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright 2025 NXP
 */

#include <drm/bridge/dw_hdmi.h>
#include <linux/module.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>

#define HTX_PAI_CTRL                   0x00
#define HTX_PAI_CTRL_EXT               0x04
#define HTX_PAI_FIELD_CTRL             0x08
#define HTX_PAI_STAT                   0x0c
#define HTX_PAI_IRQ_NOMASK             0x10
#define HTX_PAI_IRQ_MASKED             0x14
#define HTX_PAI_IRQ_MASK               0x18

#define CTRL_ENABLE                    BIT(0)

#define CTRL_EXT_WTMK_HIGH_MASK                GENMASK(31, 24)
#define CTRL_EXT_WTMK_HIGH             (0x3 << 24)
#define CTRL_EXT_WTMK_LOW_MASK         GENMASK(23, 16)
#define CTRL_EXT_WTMK_LOW              (0x3 << 16)
#define CTRL_EXT_NUM_CH_MASK           GENMASK(10, 8)
#define CTRL_EXT_NUM_CH_SHIFT          8

#define FIELD_CTRL_B_FILT              BIT(31)
#define FIELD_CTRL_PARITY_EN           BIT(30)
#define FIELD_CTRL_END_SEL             BIT(29)
#define FIELD_CTRL_PRE_SEL             GENMASK(28, 24)
#define FIELD_CTRL_PRE_SEL_SHIFT       24
#define FIELD_CTRL_D_SEL               GENMASK(23, 20)
#define FIELD_CTRL_D_SEL_SHIFT         20
#define FIELD_CTRL_V_SEL               GENMASK(19, 15)
#define FIELD_CTRL_V_SEL_SHIFT         15
#define FIELD_CTRL_U_SEL               GENMASK(14, 10)
#define FIELD_CTRL_U_SEL_SHIFT         10
#define FIELD_CTRL_C_SEL               GENMASK(9, 5)
#define FIELD_CTRL_C_SEL_SHIFT         5
#define FIELD_CTRL_P_SEL               GENMASK(4, 0)
#define FIELD_CTRL_P_SEL_SHIFT         0

struct imx8mp_hdmi_pai {
	struct device	*dev;
	void __iomem	*base;
};

static void imx8mp_hdmi_pai_enable(struct dw_hdmi *dw_hdmi, int channel,
				   int width, int rate, int non_pcm)
{
	const struct dw_hdmi_plat_data *pdata = dw_hdmi_to_plat_data(dw_hdmi);
	struct imx8mp_hdmi_pai *hdmi_pai = (struct imx8mp_hdmi_pai *)pdata->priv_audio;
	int val;

	/* PAI set */
	val = CTRL_EXT_WTMK_HIGH | CTRL_EXT_WTMK_LOW;
	val |= ((channel - 1) << CTRL_EXT_NUM_CH_SHIFT);
	writel(val, hdmi_pai->base + HTX_PAI_CTRL_EXT);

	/* IEC60958 format */
	val = 31 << FIELD_CTRL_P_SEL_SHIFT;
	val |= 30 << FIELD_CTRL_C_SEL_SHIFT;
	val |= 29 << FIELD_CTRL_U_SEL_SHIFT;
	val |= 28 << FIELD_CTRL_V_SEL_SHIFT;
	val |= 4 << FIELD_CTRL_D_SEL_SHIFT;
	val |= 0 << FIELD_CTRL_PRE_SEL_SHIFT;

	writel(val, hdmi_pai->base + HTX_PAI_FIELD_CTRL);
	/* PAI start running */
	writel(CTRL_ENABLE, hdmi_pai->base + HTX_PAI_CTRL);
}

static void imx8mp_hdmi_pai_disable(struct dw_hdmi *dw_hdmi)
{
	const struct dw_hdmi_plat_data *pdata = dw_hdmi_to_plat_data(dw_hdmi);
	struct imx8mp_hdmi_pai *hdmi_pai = (struct imx8mp_hdmi_pai *)pdata->priv_audio;

	/* Stop PAI */
	writel(0, hdmi_pai->base + HTX_PAI_CTRL);
}

static int imx8mp_hdmi_pai_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct dw_hdmi_plat_data *plat_data;
	struct imx8mp_hdmi_pai *hdmi_pai;
	struct device_node *remote;
	struct platform_device *hdmi_tx;
	struct resource *res;

	hdmi_pai = devm_kzalloc(dev, sizeof(*hdmi_pai), GFP_KERNEL);
	if (!hdmi_pai)
		return -ENOMEM;

	hdmi_pai->base = devm_platform_get_and_ioremap_resource(pdev, 0, &res);
	if (IS_ERR(hdmi_pai->base))
		return PTR_ERR(hdmi_pai->base);

	hdmi_pai->dev = dev;

	remote = of_graph_get_remote_node(pdev->dev.of_node, 0, -1);
	if (!remote)
		return -EINVAL;

	hdmi_tx = of_find_device_by_node(remote);
	if (!hdmi_tx)
		return -EINVAL;

	plat_data = platform_get_drvdata(hdmi_tx);
	plat_data->enable_audio = imx8mp_hdmi_pai_enable;
	plat_data->disable_audio = imx8mp_hdmi_pai_disable;
	plat_data->priv_audio = hdmi_pai;

	return 0;
}

static const struct of_device_id imx8mp_hdmi_pai_of_table[] = {
	{ .compatible = "fsl,imx8mp-hdmi-pai" },
	{ /* Sentinel */ }
};
MODULE_DEVICE_TABLE(of, imx8mp_hdmi_pai_of_table);

static struct platform_driver imx8mp_hdmi_pai_platform_driver = {
	.probe		= imx8mp_hdmi_pai_probe,
	.driver		= {
		.name	= "imx8mp-hdmi-pai",
		.of_match_table = imx8mp_hdmi_pai_of_table,
	},
};
module_platform_driver(imx8mp_hdmi_pai_platform_driver);

MODULE_DESCRIPTION("i.MX8MP HDMI PAI driver");
MODULE_LICENSE("GPL");
