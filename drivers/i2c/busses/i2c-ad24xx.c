// SPDX-License-Identifier: GPL-2.0-only
/*
 * AD24xx I2C controller (master) driver
 *
 * Copyright (c) 2023-2024 Alvin Šipraga <alsi@bang-olufsen.dk>
 */

#include <linux/a2b/a2b.h>
#include <linux/a2b/ad24xx.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/of_irq.h>

struct ad24xx_i2c_adapter {
	struct device *dev;
	struct a2b_func *func;
	struct a2b_node *node;
	struct i2c_adapter adap;
};

static int ad24xx_i2c_adapter_xfer(struct i2c_adapter *adap,
				  struct i2c_msg *msgs, int num)
{
	struct ad24xx_i2c_adapter *ada = i2c_get_adapdata(adap);
	struct a2b_node *node = ada->node;

	return a2b_node_i2c_xfer(node, msgs, num);
}

static u32 ad24xx_i2c_adapter_functionality(struct i2c_adapter *adap)
{
	return I2C_FUNC_I2C | I2C_FUNC_SMBUS_EMUL;
}

static const struct i2c_adapter_quirks ad24xx_i2c_adapter_quirks = {
	.flags = I2C_AQ_COMB | I2C_AQ_COMB_SAME_ADDR,
};

static const struct i2c_algorithm ad24xx_i2c_adapter_algo = {
	.master_xfer = ad24xx_i2c_adapter_xfer,
	.functionality = ad24xx_i2c_adapter_functionality,
};

static int ad24xx_i2c_adapter_probe(struct device *dev)
{
	struct a2b_func *func = to_a2b_func(dev);
	struct device_node *np = dev->of_node;
	struct ad24xx_i2c_adapter *ada;
	unsigned int val = 0;
	u32 bus_speed;
	int ret;

	ada = devm_kzalloc(dev, sizeof(*ada), GFP_KERNEL);
	if (!ada)
		return -ENOMEM;

	ada->dev = dev;
	ada->func = func;
	ada->node = func->node;

	ada->adap.owner = THIS_MODULE;
	ada->adap.algo = &ad24xx_i2c_adapter_algo;
	ada->adap.dev.parent = dev;
	ada->adap.dev.of_node = dev->of_node;
	ada->adap.quirks = &ad24xx_i2c_adapter_quirks;
	strscpy(ada->adap.name, dev_name(dev), sizeof(ada->adap.name));
	i2c_set_adapdata(&ada->adap, ada);

	ret = of_property_read_u32(np, "clock-frequency", &bus_speed);
	if (ret)
		bus_speed = I2C_MAX_STANDARD_MODE_FREQ;

	if (bus_speed != I2C_MAX_STANDARD_MODE_FREQ &&
	    bus_speed != I2C_MAX_FAST_MODE_FREQ)
		return -EINVAL;

	val |= FIELD_PREP(A2B_I2CCFG_DATARATE_MASK,
			  bus_speed == I2C_MAX_FAST_MODE_FREQ ? 1 : 0);
	val |= FIELD_PREP(A2B_I2CCFG_FRAMERATE_MASK,
			  func->node->bus->sff == A2B_SFF_44100 ? 1 : 0);

	ret = a2b_node_write(func->node, A2B_I2CCFG, val);
	if (ret)
		return ret;

	ret = devm_i2c_add_adapter(dev, &ada->adap);
	if (ret)
		return ret;

	return 0;
}

static const struct of_device_id ad24xx_i2c_adapter_of_match_table[] = {
	{ .compatible = "adi,ad2401-i2c" },
	{ .compatible = "adi,ad2402-i2c" },
	{ .compatible = "adi,ad2403-i2c" },
	{ .compatible = "adi,ad2410-i2c" },
	{ .compatible = "adi,ad2420-i2c" },
	{ .compatible = "adi,ad2421-i2c" },
	{ .compatible = "adi,ad2422-i2c" },
	{ .compatible = "adi,ad2425-i2c" },
	{ .compatible = "adi,ad2426-i2c" },
	{ .compatible = "adi,ad2427-i2c" },
	{ .compatible = "adi,ad2428-i2c" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, ad24xx_i2c_adapter_of_match_table);

static struct a2b_driver ad24xx_i2c_adapter_driver = {
	.driver = {
		.name = "ad24xx-i2c-adapter",
		.of_match_table = ad24xx_i2c_adapter_of_match_table,
		.probe_type = PROBE_PREFER_ASYNCHRONOUS,
	},
	.probe = ad24xx_i2c_adapter_probe,
};
module_a2b_driver(ad24xx_i2c_adapter_driver);

MODULE_AUTHOR("Alvin Šipraga <alsi@bang-olufsen.dk>");
MODULE_DESCRIPTION("AD24xx I2C controller driver");
MODULE_LICENSE("GPL");
