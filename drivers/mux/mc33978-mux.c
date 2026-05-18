// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Pengutronix, Oleksij Rempel <kernel@pengutronix.de>
/*
 * MC33978/MC34978 Analog Multiplexer (AMUX) Driver
 *
 * This driver provides mux-control for the 24-to-1 analog multiplexer.
 * The AMUX routes one of the following signals to the external AMUX pin:
 * - Channels 0-13: SG0-SG13 switch voltages
 * - Channels 14-21: SP0-SP7 switch voltages
 * - Channel 22: Internal temperature diode
 * - Channel 23: Battery voltage (VBATP)
 *
 * Consumer drivers (typically IIO ADC drivers) use the mux-control
 * subsystem to select which signal to measure.
 *
 * Architecture:
 * The MC33978 does not have an internal ADC. Instead, it routes analog
 * signals to an external AMUX pin that must be connected to an external
 * ADC (such as the SoC's internal ADC). The IIO subsystem is responsible
 * for coordinating the mux selection and ADC sampling.
 */

#include <linux/device.h>
#include <linux/err.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/mux/driver.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/regmap.h>

#include <linux/mfd/mc33978.h>

/* AMUX_CTRL register field definitions */
#define MC33978_AMUX_CTRL_MASK	GENMASK(5, 0)	/* 6-bit channel select */

struct mc33978_mux_priv {
	struct device *dev;
	struct regmap *map;
};

static int mc33978_mux_set(struct mux_control *mux, int state)
{
	struct mux_chip *mux_chip = mux->chip;
	struct mc33978_mux_priv *priv = mux_chip_priv(mux_chip);
	int ret;

	if (state < 0 || state >= MC33978_NUM_AMUX_CH)
		return -EINVAL;

	ret = regmap_update_bits(priv->map, MC33978_REG_AMUX_CTRL,
				 MC33978_AMUX_CTRL_MASK, state);
	if (ret)
		dev_err(priv->dev, "failed to set AMUX channel %d: %d\n",
			state, ret);

	return ret;
}

static const struct mux_control_ops mc33978_mux_ops = {
	.set = mc33978_mux_set,
};

static int mc33978_mux_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mc33978_mux_priv *priv;
	struct fwnode_handle *fwnode;
	struct mux_chip *mux_chip;
	struct mux_control *mux;
	s32 idle_state;
	int ret;

	mux_chip = devm_mux_chip_alloc(dev, 1, sizeof(*priv));
	if (IS_ERR(mux_chip))
		return dev_err_probe(dev, PTR_ERR(mux_chip), "failed to allocate mux chip\n");

	fwnode = dev_fwnode(dev->parent);
	if (!fwnode)
		return dev_err_probe(dev, -ENODEV, "missing parent firmware node\n");

	/* Borrow the parent's firmware node so consumers can find this mux chip */
	device_set_node(&mux_chip->dev, fwnode);

	priv = mux_chip_priv(mux_chip);
	priv->dev = dev;

	priv->map = dev_get_regmap(dev->parent, NULL);
	if (!priv->map)
		return dev_err_probe(dev, -ENODEV, "failed to get parent regmap\n");

	mux_chip->ops = &mc33978_mux_ops;

	mux = &mux_chip->mux[0];
	mux->states = MC33978_NUM_AMUX_CH;

	ret = device_property_read_u32(&mux_chip->dev, "idle-state",
				       (u32 *)&idle_state);
	if (ret < 0 && ret != -EINVAL) {
		return dev_err_probe(dev, ret, "failed to parse idle-state\n");
	} else if (ret == -EINVAL) {
		mux->idle_state = MUX_IDLE_AS_IS;
	} else {
		if (idle_state == MUX_IDLE_DISCONNECT)
			return dev_err_probe(dev, -EINVAL,
					     "idle-disconnect not supported by hardware\n");
		if (idle_state != MUX_IDLE_AS_IS &&
		    (idle_state < 0 || idle_state >= MC33978_NUM_AMUX_CH))
			return dev_err_probe(dev, -EINVAL, "invalid idle-state %d\n",
					     idle_state);
		mux->idle_state = idle_state;
	}

	ret = devm_mux_chip_register(dev, mux_chip);
	if (ret)
		return dev_err_probe(dev, ret, "failed to register mux chip\n");

	platform_set_drvdata(pdev, mux_chip);

	return 0;
}

static const struct platform_device_id mc33978_mux_id[] = {
	{ .name = "mc33978-mux" },
	{ .name = "mc34978-mux" },
	{ }
};
MODULE_DEVICE_TABLE(platform, mc33978_mux_id);

static struct platform_driver mc33978_mux_driver = {
	.driver = {
		.name = "mc33978-mux",
	},
	.probe = mc33978_mux_probe,
	.id_table = mc33978_mux_id,
};
module_platform_driver(mc33978_mux_driver);

MODULE_AUTHOR("Oleksij Rempel <kernel@pengutronix.de>");
MODULE_DESCRIPTION("NXP MC33978/MC34978 Analog Multiplexer Driver");
MODULE_LICENSE("GPL");
