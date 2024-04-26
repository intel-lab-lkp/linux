// SPDX-License-Identifier: GPL-2.0
/*
 * Mule I2C device multiplexer
 *
 * Copyright (C) 2024 Theobroma Systems Design und Consulting GmbH
 */

#include <linux/i2c-mux.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/property.h>
#include <linux/regmap.h>

#define MUX_CONFIG_REG	0xff
#define MUX_DEFAULT_DEV	0x0

struct mule_i2c_reg_mux {
	struct regmap *regmap;
};

static const struct regmap_config mule_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
};

static const struct of_device_id mule_i2c_mux_of_match[] = {
	{.compatible = "tsd,mule-i2c-mux",},
	{},
};
MODULE_DEVICE_TABLE(of, mule_i2c_mux_of_match);

static inline int __mux_select(struct regmap *regmap, u32 dev)
{
	return regmap_write(regmap, MUX_CONFIG_REG, dev);
}

static int mux_select(struct i2c_mux_core *muxc, u32 dev)
{
	struct mule_i2c_reg_mux *mux = muxc->priv;

	return __mux_select(mux->regmap, dev);
}

static int mux_deselect(struct i2c_mux_core *muxc, u32 dev)
{
	return mux_select(muxc, MUX_DEFAULT_DEV);
}

static void mux_remove(void *data)
{
	struct i2c_mux_core *muxc = data;

	i2c_mux_del_adapters(muxc);

	mux_deselect(muxc, MUX_DEFAULT_DEV);
}

static int mule_i2c_mux_probe(struct i2c_client *client)
{
	struct i2c_adapter *adap = client->adapter;
	struct mule_i2c_reg_mux *priv;
	struct i2c_mux_core *muxc;
	struct device_node *dev;
	unsigned int readback;
	bool old_fw;
	int ndev, ret;

	/* Count devices on the mux */
	ndev = of_get_child_count(client->dev.of_node);
	dev_dbg(&client->dev, "%u devices on the mux\n", ndev);

	muxc = i2c_mux_alloc(adap, &client->dev,
						 ndev, sizeof(*priv),
						 I2C_MUX_LOCKED,
						 mux_select, mux_deselect);
	if (!muxc)
		return -ENOMEM;

	muxc->share_addr_with_children = 1;
	priv = i2c_mux_priv(muxc);

	priv->regmap = devm_regmap_init_i2c(client, &mule_regmap_config);
	if (IS_ERR(priv->regmap))
		return dev_err_probe(&client->dev, PTR_ERR(priv->regmap),
							 "Failed to allocate i2c register map\n");

	i2c_set_clientdata(client, muxc);

	/*
	 * Mux 0 is guaranteed to exist on all old and new mule fw.
	 * mule fw without mux support will accept write ops to the
	 * config register, but readback returns 0xff (register not updated).
	 */
	ret = mux_select(muxc, 0);
	if (ret)
		return ret;

	ret = regmap_read(priv->regmap, MUX_CONFIG_REG, &readback);
	if (ret)
		return ret;

	old_fw = (readback == 0);

	ret = devm_add_action_or_reset(&client->dev, mux_remove, muxc);
	if (ret)
		return ret;

	/* Create device adapters */
	for_each_child_of_node(client->dev.of_node, dev) {
		u32 reg;

		ret = of_property_read_u32(dev, "reg", &reg);
		if (ret) {
			dev_err(&client->dev, "No reg property found for %s: %d\n",
					of_node_full_name(dev), ret);
			return ret;
		}

		if (!old_fw && reg != 0) {
			dev_warn(&client->dev,
					 "Mux %d not supported, please update Mule FW\n", reg);
			continue;
		}

		ret = mux_select(muxc, reg);
		if (ret) {
			dev_warn(&client->dev,
					 "Mux %d not supported, please update Mule FW\n", reg);
			continue;
		}

		ret = i2c_mux_add_adapter(muxc, 0, reg, 0);
		if (ret) {
			dev_err(&client->dev, "Failed to add i2c mux adapter %d: %d\n", reg, ret);
			return ret;
		}
	}

	mux_deselect(muxc, MUX_DEFAULT_DEV);

	return 0;
}

static struct i2c_driver mule_i2c_mux_driver = {
	.driver		= {
		.name	= "mule-i2c-mux",
		.of_match_table = mule_i2c_mux_of_match,
	},
	.probe		= mule_i2c_mux_probe,
};

module_i2c_driver(mule_i2c_mux_driver);

MODULE_AUTHOR("Farouk Bouabid <farouk.bouabid@theobroma-systems.com>");
MODULE_DESCRIPTION("I2C mux driver for Mule");
MODULE_LICENSE("GPL");
