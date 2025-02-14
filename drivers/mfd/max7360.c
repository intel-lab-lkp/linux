// SPDX-License-Identifier: GPL-2.0-only
/*
 * Maxim MAX7360 Core Driver
 *
 * Copyright (C) 2024 Kamel Bouhara
 * Author: Kamel Bouhara <kamel.bouhara@bootlin.com>
 */

#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/mfd/core.h>
#include <linux/mfd/max7360.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regmap.h>

static DEFINE_SPINLOCK(request_lock);

struct max7360 {
	struct device *dev;
	struct regmap *regmap;
	unsigned int requested_ports;
};

static const struct mfd_cell max7360_cells[] = {
	{
		.name           = "max7360-pwm",
	},
	{
		.name           = "max7360-gpo",
		.of_compatible	= "maxim,max7360-gpo",
	},
	{
		.name           = "max7360-gpio",
		.of_compatible	= "maxim,max7360-gpio",
	},
	{
		.name           = "max7360-keypad",
	},
	{
		.name           = "max7360-rotary",
	},
};

static const struct regmap_range max7360_volatile_ranges[] = {
	{
		.range_min = MAX7360_REG_KEYFIFO,
		.range_max = MAX7360_REG_KEYFIFO,
	}, {
		.range_min = 0x48,
		.range_max = 0x4a,
	},
};

static const struct regmap_access_table max7360_volatile_table = {
	.yes_ranges = max7360_volatile_ranges,
	.n_yes_ranges = ARRAY_SIZE(max7360_volatile_ranges),
};

static const struct regmap_config max7360_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = 0xff,
	.volatile_table = &max7360_volatile_table,
	.cache_type = REGCACHE_MAPLE,
};

int max7360_port_pin_request(struct device *dev, unsigned int pin, bool request)
{
	struct i2c_client *client;
	struct max7360 *max7360;
	unsigned long flags;
	int ret = 0;

	client = to_i2c_client(dev);
	max7360 = i2c_get_clientdata(client);

	spin_lock_irqsave(&request_lock, flags);
	if (request) {
		if (max7360->requested_ports & BIT(pin))
			ret = -EBUSY;
		else
			max7360->requested_ports |= BIT(pin);
	} else {
		max7360->requested_ports &= ~BIT(pin);
	}
	spin_unlock_irqrestore(&request_lock, flags);

	return ret;
}
EXPORT_SYMBOL_GPL(max7360_port_pin_request);

static int max7360_mask_irqs(struct max7360 *max7360)
{
	unsigned int val;
	int ret;

	/*
	 * GPIO/PWM interrupts are not masked on reset: mask the during probe,
	 * avoiding repeated spurious interrupts if the corresponding drivers
	 * are not present.
	 */
	for (int i = 0; i < MAX7360_PORT_PWM_COUNT; i++) {
		ret = regmap_write_bits(max7360->regmap, MAX7360_REG_PWMCFG(i),
					MAX7360_PORT_CFG_INTERRUPT_MASK,
					MAX7360_PORT_CFG_INTERRUPT_MASK);
		if (ret) {
			dev_err(max7360->dev, "Failed to write max7360 port configuration");
			return ret;
		}
	}

	/* Read GPIO in register, to ACK any pending IRQ. */
	ret = regmap_read(max7360->regmap, MAX7360_REG_GPIOIN, &val);
	if (ret) {
		dev_err(max7360->dev, "Failed to read gpio values: %d\n", ret);
		return ret;
	}

	return 0;
}

static int max7360_reset(struct max7360 *max7360)
{
	int err;

	err = regmap_write(max7360->regmap, MAX7360_REG_GPIOCFG,
			   MAX7360_GPIO_CFG_GPIO_RST);
	if (err) {
		dev_err(max7360->dev, "Failed to reset GPIO configuration: %x\n", err);
		return err;
	}

	err = regcache_drop_region(max7360->regmap, MAX7360_REG_GPIOCFG,
				   MAX7360_REG_GPIO_LAST);
	if (err) {
		dev_err(max7360->dev, "Failed to drop regmap cache: %x\n", err);
		return err;
	}

	err = regmap_write(max7360->regmap, MAX7360_REG_SLEEP, 0);
	if (err) {
		dev_err(max7360->dev, "Failed to reset autosleep configuration: %x\n", err);
		return err;
	}

	err = regmap_write(max7360->regmap, MAX7360_REG_DEBOUNCE, 0);
	if (err) {
		dev_err(max7360->dev, "Failed to reset GPO port count: %x\n", err);
		return err;
	}

	return 0;
}

static int max7360_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct regmap *regmap;
	struct max7360 *max7360;
	int err;

	regmap = devm_regmap_init_i2c(client, &max7360_regmap_config);
	if (IS_ERR(regmap))
		return dev_err_probe(dev, PTR_ERR(regmap),
				     "Failed to initialise regmap\n");

	max7360 = devm_kzalloc(dev, sizeof(*max7360), GFP_KERNEL);
	if (!max7360)
		return -ENOMEM;

	max7360->regmap = regmap;
	max7360->dev = dev;
	i2c_set_clientdata(client, max7360);

	err = max7360_reset(max7360);
	if (err)
		return dev_err_probe(dev, err, "Failed to reset device\n");

	/* Get the device out of shutdown mode. */
	err = regmap_write_bits(regmap, MAX7360_REG_GPIOCFG,
				MAX7360_GPIO_CFG_GPIO_EN,
				MAX7360_GPIO_CFG_GPIO_EN);
	if (err)
		return dev_err_probe(dev, err, "Failed to enable GPIO and PWM module\n");

	err = max7360_mask_irqs(max7360);
	if (err)
		return dev_err_probe(dev, err, "Could not mask interrupts\n");

	err =  devm_mfd_add_devices(dev, PLATFORM_DEVID_NONE,
				    max7360_cells, ARRAY_SIZE(max7360_cells),
				    NULL, 0, NULL);
	if (err)
		return dev_err_probe(dev, err, "Failed to register child devices\n");

	return 0;
}

static const struct of_device_id max7360_dt_match[] = {
	{ .compatible = "maxim,max7360" },
	{},
};
MODULE_DEVICE_TABLE(of, max7360_dt_match);

static struct i2c_driver max7360_driver = {
	.driver = {
		.name = "max7360",
		.of_match_table = max7360_dt_match,
	},
	.probe = max7360_probe,
};
module_i2c_driver(max7360_driver);

MODULE_DESCRIPTION("Maxim MAX7360 I2C IO Expander core driver");
MODULE_AUTHOR("Kamel Bouhara <kamel.bouhara@bootlin.com>");
MODULE_LICENSE("GPL");
