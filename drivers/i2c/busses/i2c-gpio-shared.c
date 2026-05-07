// SPDX-License-Identifier: GPL-2.0
/*
 * Bitbanging driver for multiple I2C busses with shared SCL pin using the GPIO API
 * Copyright (c) 2025 Markus Stockhausen <markus.stockhausen at gmx.de>
 */

#include <linux/gpio/consumer.h>
#include <linux/i2c-algo-bit.h>
#include <linux/mod_devicetable.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>

struct gpio_shared_ctx;

struct gpio_shared_bus {
	struct gpio_desc *sda;
	struct i2c_adapter adap;
	struct i2c_algo_bit_data bit_data;
	struct gpio_shared_ctx *ctx;
};

struct gpio_shared_ctx {
	struct device *dev;
	struct gpio_desc *scl;
	struct mutex lock;
	struct gpio_shared_bus bus[];
};

static void gpio_shared_setsda(void *data, int state)
{
	struct gpio_shared_bus *bus = data;

	gpiod_set_value_cansleep(bus->sda, state);
}

static void gpio_shared_setscl(void *data, int state)
{
	struct gpio_shared_bus *bus = data;
	struct gpio_shared_ctx *ctx = bus->ctx;

	gpiod_set_value_cansleep(ctx->scl, state);
}

static int gpio_shared_getsda(void *data)
{
	struct gpio_shared_bus *bus = data;

	return gpiod_get_value_cansleep(bus->sda);
}

static int gpio_shared_getscl(void *data)
{
	struct gpio_shared_bus *bus = data;
	struct gpio_shared_ctx *ctx = bus->ctx;

	return gpiod_get_value_cansleep(ctx->scl);
}

static int gpio_shared_pre_xfer(struct i2c_adapter *adap)
{
	struct gpio_shared_bus *bus = container_of(adap, struct gpio_shared_bus, adap);
	struct gpio_shared_ctx *ctx = bus->ctx;

	return mutex_lock_interruptible(&ctx->lock);
}

static void gpio_shared_post_xfer(struct i2c_adapter *adap)
{
	struct gpio_shared_bus *bus = container_of(adap, typeof(*bus), adap);
	struct gpio_shared_ctx *ctx = bus->ctx;

	mutex_unlock(&ctx->lock);
}

static void gpio_shared_del_adapter(void *data)
{
	i2c_del_adapter(data);
}

static int gpio_shared_probe(struct platform_device *pdev)
{
	int bus_count, msecs, ret, bus_num = 0;
	struct device *dev = &pdev->dev;
	struct gpio_shared_ctx *ctx;

	bus_count = device_get_child_node_count(dev);
	if (!bus_count)
		return dev_err_probe(dev, -EINVAL, "no busses defined\n");

	ctx = devm_kzalloc(dev, struct_size(ctx, bus, bus_count), GFP_KERNEL);
	if (!ctx)
		return dev_err_probe(dev, -ENOMEM, "memory allocation failed\n");

	ctx->dev = dev;
	mutex_init(&ctx->lock);

	ctx->scl = devm_gpiod_get(dev, "scl", GPIOD_OUT_HIGH_OPEN_DRAIN);
	if (IS_ERR(ctx->scl))
		return dev_err_probe(dev, PTR_ERR(ctx->scl), "shared SCL node not found\n");

	device_for_each_child_node_scoped(dev, child) {
		struct gpio_shared_bus *bus = &ctx->bus[bus_num];
		struct i2c_adapter *adap = &bus->adap;
		struct i2c_algo_bit_data *bit_data = &bus->bit_data;

		bus->sda = devm_fwnode_gpiod_get(dev, child, "sda", GPIOD_OUT_HIGH_OPEN_DRAIN,
						 fwnode_get_name(child));
		if (IS_ERR(bus->sda))
			return dev_err_probe(dev, PTR_ERR(bus->sda),
					     "SDA node for bus %d not found\n", bus_num);
		bus->ctx = ctx;
		bit_data->data = bus;
		bit_data->setsda = gpio_shared_setsda;
		bit_data->setscl = gpio_shared_setscl;
		bit_data->pre_xfer = gpio_shared_pre_xfer;
		bit_data->post_xfer = gpio_shared_post_xfer;

		if (fwnode_property_read_u32(child, "i2c-gpio-shared,delay-us", &bit_data->udelay))
			bit_data->udelay = 5;
		if (!fwnode_property_read_bool(child, "i2c-gpio-shared,sda-output-only"))
			bit_data->getsda = gpio_shared_getsda;

		if (!device_property_read_bool(dev, "i2c-gpio-shared,scl-output-only"))
			bit_data->getscl = gpio_shared_getscl;
		if (!device_property_read_u32(dev, "i2c-gpio-shared,timeout-ms", &msecs))
			bit_data->timeout = msecs_to_jiffies(msecs);
		else
			bit_data->timeout = HZ / 10; /* 100ms */

		if (gpiod_cansleep(bus->sda) || gpiod_cansleep(ctx->scl))
			dev_warn(dev, "Slow GPIO pins might wreak havoc into I2C/SMBus bus timing\n");

		adap->dev.parent = dev;
		adap->owner = THIS_MODULE;
		adap->algo_data = &bus->bit_data;
		device_set_node(&adap->dev, child);
		snprintf(adap->name, sizeof(adap->name),
			 "i2c-gpio-shared:%s", fwnode_get_name(child));

		ret = i2c_bit_add_bus(adap);
		if (ret)
			return dev_err_probe(dev, ret, "failed to register bus %d\n", bus_num);

		ret = devm_add_action_or_reset(dev, gpio_shared_del_adapter, adap);
		if (ret)
			return dev_err_probe(dev, ret,
					     "bus %d cleanup registration failed\n", bus_num);

		dev_info(dev, "shared I2C bus %u using lines %u (SDA) and %u (SCL) delay=%d\n",
			 bus_num, desc_to_gpio(bus->sda), desc_to_gpio(ctx->scl),
			 bit_data->udelay);

		bus_num++;
	}

	return 0;
}

static const struct of_device_id gpio_shared_of_match[] = {
	{ .compatible = "i2c-gpio-shared" },
	{}
};
MODULE_DEVICE_TABLE(of, gpio_shared_of_match);

static struct platform_driver gpio_shared_driver = {
	.probe = gpio_shared_probe,
	.driver = {
		.name = "i2c-gpio-shared",
		.of_match_table = gpio_shared_of_match,
	},
};

module_platform_driver(gpio_shared_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Markus Stockhausen <markus.stockhausen at gmx.de>");
MODULE_DESCRIPTION("bitbanging multi I2C driver for shared SCL");
