// SPDX-License-Identifier: GPL-2.0-only

#include <linux/delay.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/export.h>
#include <linux/gpio/consumer.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/pwrseq/provider.h>
#include <linux/regulator/consumer.h>
#include <linux/usb.h>
#include <linux/usb/chipidea.h>
#include <linux/usb/tegra_usb_phy.h>

struct pwrseq_tegra_modem_ctx {
	struct device *dev;
	struct pwrseq_device *pwrseq;

	struct gpio_desc *enable_gpio;
	struct regulator *power_supply;

	struct platform_device *usb_dev;
	struct tegra_usb *usb;
};

static int pwrseq_tegra_modem_power_enable(struct pwrseq_device *pwrseq)
{
	struct pwrseq_tegra_modem_ctx *ctx = pwrseq_device_get_drvdata(pwrseq);

	return regulator_enable(ctx->power_supply);
}

static int pwrseq_tegra_modem_power_disable(struct pwrseq_device *pwrseq)
{
	struct pwrseq_tegra_modem_ctx *ctx = pwrseq_device_get_drvdata(pwrseq);

	return regulator_disable(ctx->power_supply);
}

static const struct pwrseq_unit_data pwrseq_tegra_modem_power_unit_data = {
	.name = "power-enable",
	.enable = pwrseq_tegra_modem_power_enable,
	.disable = pwrseq_tegra_modem_power_disable,
};

static const struct pwrseq_unit_data *pwrseq_tegra_modem_unit_deps[] = {
	&pwrseq_tegra_modem_power_unit_data,
	NULL,
};

static int pwrseq_tegra_modem_enable(struct pwrseq_device *pwrseq)
{
	struct pwrseq_tegra_modem_ctx *ctx = pwrseq_device_get_drvdata(pwrseq);
	struct tegra_usb *usb = ctx->usb;
	int ret;

	/*
	 * USB controller registers shouldn't be touched before PHY is
	 * initialized, otherwise CPU will hang because clocks are gated.
	 * PHY driver controls gating of internal USB clocks on Tegra.
	 */
	ret = usb_phy_init(usb->phy);
	if (ret) {
		dev_err(ctx->dev, "failed to init USB PHY\n");
		return ret;
	}

	usb->dev = ci_hdrc_add_device(&ctx->usb_dev->dev,
				      ctx->usb_dev->resource,
				      ctx->usb_dev->num_resources,
				      &usb->data);
	if (IS_ERR(usb->dev)) {
		usb_phy_shutdown(usb->phy);
		dev_err(ctx->dev, "failed to register USB controller\n");
		return PTR_ERR(usb->dev);
	}

	gpiod_set_value_cansleep(ctx->enable_gpio, 1);

	return 0;
}

static int pwrseq_tegra_modem_disable(struct pwrseq_device *pwrseq)
{
	struct pwrseq_tegra_modem_ctx *ctx = pwrseq_device_get_drvdata(pwrseq);
	struct tegra_usb *usb = ctx->usb;

	gpiod_set_value_cansleep(ctx->enable_gpio, 0);
	ci_hdrc_remove_device(usb->dev);
	usb_phy_shutdown(usb->phy);

	/* For USB to settle after turning off */
	msleep(500);

	return 0;
}

static const struct pwrseq_unit_data pwrseq_tegra_modem_unit = {
	.name = "modem-power-sequence",
	.deps = pwrseq_tegra_modem_unit_deps,
	.enable = pwrseq_tegra_modem_enable,
	.disable = pwrseq_tegra_modem_disable,
};

static const struct pwrseq_target_data pwrseq_tegra_modem_target = {
	.name = "modem-power",
	.unit = &pwrseq_tegra_modem_unit,
};

static const struct pwrseq_target_data *pwrseq_tegra_modem_targets[] = {
	&pwrseq_tegra_modem_target,
	NULL
};

static int pwrseq_tegra_modem_match(struct pwrseq_device *pwrseq,
				    struct device *dev)
{
	/* We only match the specific modem compatible for now */
	if (!of_device_is_compatible(dev->of_node, "infineon,xmm6260"))
		return PWRSEQ_NO_MATCH;

	return PWRSEQ_MATCH_OK;
}

static void pwrseq_tegra_modem_put_device(void *dev)
{
	put_device(dev);
}

static int pwrseq_tegra_modem_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *usb_node;
	struct pwrseq_tegra_modem_ctx *ctx;
	struct pwrseq_config config = { };
	int ret;

	ctx = devm_kzalloc(dev, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	ctx->dev = dev;

	ctx->enable_gpio = devm_gpiod_get_optional(dev, "enable",
						   GPIOD_OUT_LOW);
	if (IS_ERR(ctx->enable_gpio))
		return dev_err_probe(dev, PTR_ERR(ctx->enable_gpio),
				     "failed to get enable GPIO\n");

	ctx->power_supply = devm_regulator_get(dev, "power");
	if (IS_ERR(ctx->power_supply))
		return dev_err_probe(dev, PTR_ERR(ctx->power_supply),
				     "failed to get power supply\n");

	usb_node = of_parse_phandle(dev->of_node, "nvidia,usb-bus", 0);
	if (!usb_node)
		return dev_err_probe(dev, -ENODEV,
				     "failed to parse modem USB bus\n");

	ctx->usb_dev = of_find_device_by_node(usb_node);
	of_node_put(usb_node);
	if (!ctx->usb_dev)
		return -EPROBE_DEFER;

	ret = devm_add_action_or_reset(dev, pwrseq_tegra_modem_put_device,
				       &ctx->usb_dev->dev);
	if (ret)
		return ret;

	ctx->usb = platform_get_drvdata(ctx->usb_dev);
	if (!ctx->usb)
		return -EPROBE_DEFER;

	config.parent = dev;
	config.owner = THIS_MODULE;
	config.drvdata = ctx;
	config.match = pwrseq_tegra_modem_match;
	config.targets = pwrseq_tegra_modem_targets;

	ctx->pwrseq = devm_pwrseq_device_register(dev, &config);
	if (IS_ERR(ctx->pwrseq))
		return dev_err_probe(dev, PTR_ERR(ctx->pwrseq),
				     "failed to register the power sequencer\n");

	return 0;
}

static const struct of_device_id pwrseq_tegra_modem_of_match[] = {
	{ .compatible = "nvidia,tegra-modem-pwrseq" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, pwrseq_tegra_modem_of_match);

static struct platform_driver pwrseq_tegra_modem_driver = {
	.driver = {
		.name = "pwrseq-tegra-modem",
		.of_match_table = pwrseq_tegra_modem_of_match,
	},
	.probe = pwrseq_tegra_modem_probe,
};
module_platform_driver(pwrseq_tegra_modem_driver);

MODULE_AUTHOR("Svyatolsav Ryhel <clamor95@gmail.com>");
MODULE_DESCRIPTION("Tegra modem power sequencer driver");
MODULE_LICENSE("GPL");
