// SPDX-License-Identifier: GPL-2.0
/*
 * Renesas RZ/V2H(P) USB2PHY control driver
 *
 * Copyright (C) 2025 Renesas Electronics Corporation
 */

#include <linux/cleanup.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/reset.h>
#include <linux/reset-controller.h>

struct rzv2h_usb2phy_regval {
	u16 reg;
	u16 val;
};

struct rzv2h_usb2phy_data {
	const struct rzv2h_usb2phy_regval *init_vals;
	unsigned int init_val_count;

	u16 ctrl_reg;
	u16 ctrl_assert_val;
	u16 ctrl_deassert_val;
	u16 ctrl_status_bits;
	u16 ctrl_release_val;

	u16 ctrl2_reg;
	u16 ctrl2_acquire_val;
	u16 ctrl2_release_val;
};

struct rzv2h_usb2phy_ctrl_priv {
	const struct rzv2h_usb2phy_data *data;
	void __iomem *base;
	struct device *dev;
	struct reset_controller_dev rcdev;
	spinlock_t lock;
};

#define rcdev_to_priv(x) container_of(x, struct rzv2h_usb2phy_ctrl_priv, rcdev)

static int rzv2h_usbphy_ctrl_assert(struct reset_controller_dev *rcdev,
				    unsigned long id)
{
	struct rzv2h_usb2phy_ctrl_priv *priv = rcdev_to_priv(rcdev);
	const struct rzv2h_usb2phy_data *data = priv->data;
	struct device *dev = priv->dev;
	int ret;

	ret = pm_runtime_resume_and_get(dev);
	if (ret) {
		dev_err(dev, "pm_runtime_resume_and_get failed\n");
		return ret;
	}
	scoped_guard(spinlock, &priv->lock) {
		writel(data->ctrl2_acquire_val, priv->base + data->ctrl2_reg);
		writel(data->ctrl_assert_val, priv->base + data->ctrl_reg);
	}

	/* The reset line needs to be asserted for more than 10 microseconds. */
	udelay(11);
	pm_runtime_put(dev);

	return 0;
}

static int rzv2h_usbphy_ctrl_deassert(struct reset_controller_dev *rcdev,
				      unsigned long id)
{
	struct rzv2h_usb2phy_ctrl_priv *priv = rcdev_to_priv(rcdev);
	const struct rzv2h_usb2phy_data *data = priv->data;
	struct device *dev = priv->dev;
	int ret;

	ret = pm_runtime_resume_and_get(dev);
	if (ret) {
		dev_err(dev, "pm_runtime_resume_and_get failed\n");
		return ret;
	}

	scoped_guard(spinlock, &priv->lock) {
		writel(data->ctrl_deassert_val, priv->base + data->ctrl_reg);
		writel(data->ctrl2_release_val, priv->base + data->ctrl2_reg);
		writel(data->ctrl_release_val, priv->base + data->ctrl_reg);
	}

	pm_runtime_put(dev);

	return 0;
}

static int rzv2h_usbphy_ctrl_status(struct reset_controller_dev *rcdev,
				    unsigned long id)
{
	struct rzv2h_usb2phy_ctrl_priv *priv = rcdev_to_priv(rcdev);
	struct device *dev = priv->dev;
	int ret;
	u32 reg;

	ret = pm_runtime_resume_and_get(dev);
	if (ret) {
		dev_err(dev, "pm_runtime_resume_and_get failed\n");
		return ret;
	}

	scoped_guard(spinlock, &priv->lock)
		reg = readl(priv->base + priv->data->ctrl_reg);

	pm_runtime_put(dev);

	return (reg & priv->data->ctrl_status_bits) == priv->data->ctrl_status_bits;
}

static const struct reset_control_ops rzv2h_usbphy_ctrl_reset_ops = {
	.assert = rzv2h_usbphy_ctrl_assert,
	.deassert = rzv2h_usbphy_ctrl_deassert,
	.status = rzv2h_usbphy_ctrl_status,
};

static int rzv2h_reset_of_xlate(struct reset_controller_dev *rcdev,
				const struct of_phandle_args *reset_spec)
{
	/* No special handling needed, we have only one reset line per device */
	return 0;
}

static int rzv2h_usb2phy_ctrl_probe(struct platform_device *pdev)
{
	const struct rzv2h_usb2phy_data *data;
	struct rzv2h_usb2phy_ctrl_priv *priv;
	struct device *dev = &pdev->dev;
	struct reset_control *rstc;
	int error;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	data = of_device_get_match_data(dev);
	if (!data)
		return dev_err_probe(dev, -ENODEV,
				     "failed to match device\n");

	priv->data = data;
	priv->dev = dev;
	priv->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(priv->base))
		return PTR_ERR(priv->base);

	rstc = devm_reset_control_get_shared_deasserted(dev, NULL);
	if (IS_ERR(rstc))
		return dev_err_probe(dev, PTR_ERR(rstc),
				     "failed to get deasserted reset\n");

	spin_lock_init(&priv->lock);
	dev_set_drvdata(dev, priv);

	error = devm_pm_runtime_enable(dev);
	if (error)
		return dev_err_probe(dev, error, "Failed to enable pm_runtime\n");

	error = pm_runtime_resume_and_get(dev);
	if (error)
		return dev_err_probe(dev, error, "pm_runtime_resume_and_get failed\n");

	for (unsigned int i = 0; i < data->init_val_count; i++)
		writel(data->init_vals[i].val, priv->base + data->init_vals[i].reg);

	pm_runtime_put(dev);

	priv->rcdev.ops = &rzv2h_usbphy_ctrl_reset_ops;
	priv->rcdev.of_reset_n_cells = 0;
	priv->rcdev.nr_resets = 1;
	priv->rcdev.of_xlate = rzv2h_reset_of_xlate;
	priv->rcdev.of_node = dev->of_node;
	priv->rcdev.dev = dev;

	return devm_reset_controller_register(dev, &priv->rcdev);
}

static const struct rzv2h_usb2phy_regval rzv2h_init_vals[] = {
	{ .reg = 0xc10, .val = 0x67c },
	{ .reg = 0xc14, .val = 0x1f },
	{ .reg = 0x600, .val = 0x909 },
};

static const struct rzv2h_usb2phy_data rzv2h_of_data = {
	.init_vals = rzv2h_init_vals,
	.init_val_count = ARRAY_SIZE(rzv2h_init_vals),
	.ctrl_reg = 0,
	.ctrl_assert_val = 0x206,
	.ctrl_status_bits = BIT(2),
	.ctrl_deassert_val = 0x200,
	.ctrl_release_val = 0x0,
	.ctrl2_reg = 0xb04,
	.ctrl2_acquire_val = 0x303,
	.ctrl2_release_val = 0x3,
};

static const struct of_device_id rzv2h_usb2phy_ctrl_match_table[] = {
	{ .compatible = "renesas,r9a09g057-usb2phy-ctrl", .data = &rzv2h_of_data },
	{ /* Sentinel */ }
};
MODULE_DEVICE_TABLE(of, rzv2h_usb2phy_ctrl_match_table);

static struct platform_driver rzv2h_usb2phy_ctrl_driver = {
	.driver = {
		.name		= "rzv2h_usb2phy_ctrl",
		.of_match_table	= rzv2h_usb2phy_ctrl_match_table,
	},
	.probe = rzv2h_usb2phy_ctrl_probe,
};
module_platform_driver(rzv2h_usb2phy_ctrl_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Lad Prabhakar <prabhakar.mahadev-lad.rj@bp.renesas.com>");
MODULE_DESCRIPTION("Renesas RZ/V2H(P) USB2PHY Control");
