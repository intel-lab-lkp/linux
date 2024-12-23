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

struct max7360_mfd {
	struct regmap *regmap;
	unsigned int requested_ports;
	struct device *dev;
};

#define GPO_COMPATIBLE "maxim,max7360-gpo"
#define GPIO_COMPATIBLE "maxim,max7360-gpio"

static const struct mfd_cell max7360_cells[] = {
	{
		.name           = MAX7360_DRVNAME_PWM,
	},
	{
		.name           = MAX7360_DRVNAME_GPO,
		.of_compatible	= GPO_COMPATIBLE,
	},
	{
		.name           = MAX7360_DRVNAME_GPIO,
		.of_compatible	= GPIO_COMPATIBLE,
	},
	{
		.name           = MAX7360_DRVNAME_KEYPAD,
	},
	{
		.name           = MAX7360_DRVNAME_ROTARY,
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
	.cache_type = REGCACHE_RBTREE,
};

static int max7360_set_gpos_count(struct max7360_mfd *max7360_mfd)
{
	/*
	 * Max7360 COL0 to COL7 pins can be used either as keypad columns,
	 * general purpose output or a mix of both.
	 * Get the number of pins requested by the corresponding drivers, ensure
	 * they are compatible with each others and apply the corresponding
	 * configuration.
	 */
	struct device_node *np;
	u32 gpos = 0;
	u32 columns = 0;
	unsigned int val;
	int ret;

	np = of_get_compatible_child(max7360_mfd->dev->of_node, GPO_COMPATIBLE);
	if (np) {
		ret = of_property_read_u32(np, "ngpios", &gpos);
		if (ret < 0) {
			dev_err(max7360_mfd->dev, "Failed to read gpos count\n");
			return ret;
		}
	}

	ret = device_property_read_u32(max7360_mfd->dev,
				       "keypad,num-columns", &columns);
	if (ret < 0) {
		dev_err(max7360_mfd->dev, "Failed to read columns count\n");
		return ret;
	}

	if (gpos > MAX7360_MAX_GPO ||
	    (gpos + columns > MAX7360_MAX_KEY_COLS)) {
		dev_err(max7360_mfd->dev,
			"Incompatible gpos and columns count (%u, %u)\n",
			gpos, columns);
		return -EINVAL;
	}

	/*
	 * MAX7360_REG_DEBOUNCE contains configuration both for keypad debounce
	 * timings and gpos/keypad columns repartition. Only the later is
	 * modified here.
	 */
	val = FIELD_PREP(MAX7360_PORTS, gpos);
	ret = regmap_write_bits(max7360_mfd->regmap, MAX7360_REG_DEBOUNCE,
				MAX7360_PORTS, val);
	if (ret) {
		dev_err(max7360_mfd->dev,
			"Failed to write max7360 columns/gpos configuration");
		return ret;
	}

	return 0;
}

int max7360_port_pin_request(struct device *dev, unsigned int pin, bool request)
{
	struct i2c_client *client;
	struct max7360_mfd *max7360_mfd;
	unsigned long flags;
	int ret = 0;

	client = to_i2c_client(dev);
	max7360_mfd = i2c_get_clientdata(client);

	spin_lock_irqsave(&request_lock, flags);
	if (request) {
		if (max7360_mfd->requested_ports & BIT(pin))
			ret = -EBUSY;
		else
			max7360_mfd->requested_ports |= BIT(pin);
	} else {
		max7360_mfd->requested_ports &= ~BIT(pin);
	}
	spin_unlock_irqrestore(&request_lock, flags);

	return ret;
}
EXPORT_SYMBOL_GPL(max7360_port_pin_request);

static int max7360_mask_irqs(struct max7360_mfd *max7360_mfd)
{
	unsigned int i;
	unsigned int val;
	int ret;

	/*
	 * GPIO/PWM interrupts are not masked on reset: mask the during probe,
	 * avoiding repeated spurious interrupts if the corresponding drivers
	 * are not present.
	 */
	for (i = 0; i < MAX7360_PORT_PWM_COUNT; i++) {
		ret = regmap_write_bits(max7360_mfd->regmap,
					MAX7360_REG_PWMCFG + i,
					MAX7360_PORT_CFG_INTERRUPT_MASK,
					MAX7360_PORT_CFG_INTERRUPT_MASK);
		if (ret) {
			dev_err(max7360_mfd->dev,
				"failed to write max7360 port configuration");
			return ret;
		}
	}

	/* Read gpio in register, to ack any pending IRQ.
	 */
	ret = regmap_read(max7360_mfd->regmap, MAX7360_REG_GPIOIN, &val);
	if (ret) {
		dev_err(max7360_mfd->dev, "Failed to read gpio values: %d\n",
			ret);
		return ret;
	}

	return 0;
}

static int max7360_reset(struct max7360_mfd *max7360_mfd)
{
	int err;

	/*
	 * Set back the default values.
	 * We do not use GPIO reset function here, as it does not work reliably.
	 */
	err = regmap_write(max7360_mfd->regmap, MAX7360_REG_GPIODEB, 0x00);
	if (err) {
		dev_err(max7360_mfd->dev, "Failed to set configuration\n");
		return err;
	}

	err = regmap_write(max7360_mfd->regmap, MAX7360_REG_GPIOCURR, MAX7360_REG_GPIOCURR_FIXED);
	if (err) {
		dev_err(max7360_mfd->dev, "Failed to set configuration\n");
		return err;
	}

	err = regmap_write(max7360_mfd->regmap, MAX7360_REG_GPIOOUTM, 0x00);
	if (err) {
		dev_err(max7360_mfd->dev, "Failed to set configuration\n");
		return err;
	}

	err = regmap_write(max7360_mfd->regmap, MAX7360_REG_PWMCOM, 0x00);
	if (err) {
		dev_err(max7360_mfd->dev, "Failed to set configuration\n");
		return err;
	}

	err = regmap_write(max7360_mfd->regmap, MAX7360_REG_SLEEP, 0);
	if (err) {
		dev_err(max7360_mfd->dev, "Failed to set configuration\n");
		return err;
	}

	return 0;
}

static int max7360_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct regmap *regmap;
	struct max7360_mfd *max7360_mfd;
	int err;

	regmap = devm_regmap_init_i2c(client, &max7360_regmap_config);
	if (IS_ERR(regmap))
		return dev_err_probe(dev, PTR_ERR(regmap),
				     "Failed to initialise regmap\n");

	max7360_mfd = devm_kzalloc(dev, sizeof(*max7360_mfd), GFP_KERNEL);
	if (!max7360_mfd)
		return -ENOMEM;

	max7360_mfd->regmap = regmap;
	max7360_mfd->dev = dev;
	i2c_set_clientdata(client, max7360_mfd);

	err = max7360_reset(max7360_mfd);
	if (err)
		return dev_err_probe(dev, err, "Failed to reset device\n");

	err = max7360_set_gpos_count(max7360_mfd);
	if (err)
		return dev_err_probe(dev, err, "Failed to set GPOS pin count\n");

	/*
	 * Get the device out of shutdown mode.
	 */
	err = regmap_write_bits(regmap, MAX7360_REG_GPIOCFG,
				MAX7360_GPIO_CFG_GPIO_EN,
				MAX7360_GPIO_CFG_GPIO_EN);
	if (err)
		return dev_err_probe(dev, err, "Failed to set device out of shutdown\n");

	err = max7360_mask_irqs(max7360_mfd);
	if (err)
		return dev_err_probe(dev, err, "could not mask interrupts\n");

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

MODULE_DESCRIPTION("Maxim MAX7360 MFD core driver");
MODULE_AUTHOR("Kamel Bouhara <kamel.bouhara@bootlin.com>");
MODULE_LICENSE("GPL");
