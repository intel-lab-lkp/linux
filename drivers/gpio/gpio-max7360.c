// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2024 Bootlin
 *
 * Author: Kamel BOUHARA <kamel.bouhara@bootlin.com>
 * Author: Mathieu Dubois-Briand <mathieu.dubois-briand@bootlin.com>
 */

#include <linux/gpio/driver.h>
#include <linux/gpio/regmap.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/mfd/max7360.h>
#include <linux/module.h>
#include <linux/property.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/slab.h>

#define MAX7360_GPIO_PORT	1
#define MAX7360_GPIO_COL	2

static int max7360_gpo_reg_mask_xlate(struct gpio_regmap *gpio,
				      unsigned int base, unsigned int offset,
				      unsigned int *reg, unsigned int *mask)
{
	u16 ngpios = gpio_regmap_get_ngpio(gpio);

	*reg = base;
	*mask = BIT(MAX7360_MAX_KEY_COLS - (ngpios - offset));

	return 0;
}

static int max7360_gpio_request(struct gpio_chip *gc, unsigned int pin)
{
	/*
	 * GPIOs on PORT pins are shared with the PWM and rotary encoder
	 * drivers: they have to be requested from the MFD driver.
	 */
	return max7360_port_pin_request(gc->parent->parent, pin, true);
}

static void max7360_gpio_free(struct gpio_chip *gc, unsigned int pin)
{
	max7360_port_pin_request(gc->parent->parent, pin, false);
}

static int max7360_set_gpos_count(struct device *dev, struct regmap *regmap)
{
	/*
	 * MAX7360 COL0 to COL7 pins can be used either as keypad columns,
	 * general purpose output or a mix of both.
	 */
	unsigned int val;
	u32 columns;
	u32 ngpios;
	int ret;

	ret = device_property_read_u32(dev, "ngpios", &ngpios);
	if (ret < 0) {
		dev_err(dev, "Missing ngpios OF property\n");
		return ret;
	}

	/*
	 * Get the number of pins requested by the keypad and ensure our own pin
	 * count is compatible with it.
	 */
	ret = device_property_read_u32(dev->parent, "keypad,num-columns", &columns);
	if (ret < 0) {
		dev_err(dev, "Failed to read columns count\n");
		return ret;
	}

	if (ngpios > MAX7360_MAX_GPO ||
	    (ngpios + columns > MAX7360_MAX_KEY_COLS)) {
		dev_err(dev, "Incompatible gpos and columns count (%u, %u)\n",
			ngpios, columns);
		return -EINVAL;
	}

	/*
	 * MAX7360_REG_DEBOUNCE contains configuration both for keypad debounce
	 * timings and gpos/keypad columns repartition. Only the later is
	 * modified here.
	 */
	val = FIELD_PREP(MAX7360_PORTS, ngpios);
	ret = regmap_write_bits(regmap, MAX7360_REG_DEBOUNCE, MAX7360_PORTS, val);
	if (ret) {
		dev_err(dev, "Failed to write max7360 columns/gpos configuration");
		return ret;
	}

	return 0;
}

static const struct regmap_irq max7360_regmap_irqs[MAX7360_MAX_GPIO] = {
	REGMAP_IRQ_REG(0, 0, BIT(0)),
	REGMAP_IRQ_REG(1, 0, BIT(1)),
	REGMAP_IRQ_REG(2, 0, BIT(2)),
	REGMAP_IRQ_REG(3, 0, BIT(3)),
	REGMAP_IRQ_REG(4, 0, BIT(4)),
	REGMAP_IRQ_REG(5, 0, BIT(5)),
	REGMAP_IRQ_REG(6, 0, BIT(6)),
	REGMAP_IRQ_REG(7, 0, BIT(7)),
};

static int max7360_handle_mask_sync(const int index,
				    const unsigned int mask_buf_def,
				    const unsigned int mask_buf,
				    void *const irq_drv_data)
{
	struct regmap *regmap = irq_drv_data;
	unsigned int val;

	for (unsigned int i = 0; i < MAX7360_MAX_GPIO; ++i) {
		val = (mask_buf & 1 << i) ? MAX7360_PORT_CFG_INTERRUPT_MASK : 0;
		regmap_write_bits(regmap, MAX7360_REG_PWMCFG(i),
				  MAX7360_PORT_CFG_INTERRUPT_MASK, val);
	}

	return 0;
}

static int max7360_gpio_probe(struct platform_device *pdev)
{
	struct regmap_irq_chip *irq_chip;
	struct regmap_irq_chip_data *irq_chip_data;
	struct gpio_regmap_config gpio_config = { };
	struct device *dev = &pdev->dev;
	unsigned long gpio_function;
	struct regmap *regmap;
	unsigned int outconf;
	unsigned long flags;
	int irq;
	int ret;

	regmap = dev_get_regmap(dev->parent, NULL);
	if (!regmap)
		return dev_err_probe(dev, -ENODEV, "could not get parent regmap\n");

	gpio_function = (uintptr_t)device_get_match_data(dev);

	if (gpio_function == MAX7360_GPIO_PORT &&
	    (device_property_read_bool(dev, "interrupt-controller"))) {
		/*
		 * Port GPIOs with interrupt-controller property: add IRQ
		 * controller.
		 */
		irq = fwnode_irq_get_byname(dev->parent->fwnode, "inti");
		if (irq < 0)
			return dev_err_probe(dev, irq, "Failed to get IRQ\n");

		irq_chip = devm_kzalloc(dev, sizeof(*irq_chip), GFP_KERNEL);
		if (!irq_chip)
			return -ENOMEM;

		irq_chip->name = dev_name(dev);
		irq_chip->status_base = MAX7360_REG_GPIOIN;
		irq_chip->num_regs = 1;
		irq_chip->num_irqs = MAX7360_MAX_GPIO;
		irq_chip->irqs = max7360_regmap_irqs;
		irq_chip->handle_mask_sync = max7360_handle_mask_sync;
		irq_chip->status_is_level = true;
		irq_chip->irq_drv_data = regmap;

		for (unsigned int i = 0; i < MAX7360_MAX_GPIO; i++) {
			regmap_write_bits(regmap, MAX7360_REG_PWMCFG(i),
					  MAX7360_PORT_CFG_INTERRUPT_EDGES,
					  MAX7360_PORT_CFG_INTERRUPT_EDGES);
		}

		flags = IRQF_TRIGGER_LOW | IRQF_ONESHOT | IRQF_SHARED;
		ret = devm_regmap_add_irq_chip_fwnode(dev, dev_fwnode(dev), regmap, irq, flags, 0,
						      irq_chip, &irq_chip_data);
		if (ret)
			return dev_err_probe(dev, ret, "IRQ registration failed\n");

		gpio_config.irq_domain = regmap_irq_get_domain(irq_chip_data);
	}

	if (gpio_function == MAX7360_GPIO_PORT) {
		/*
		 * Port GPIOs: set output mode configuration (constant-current or not).
		 * This property is optional.
		 */
		outconf = 0;
		ret = device_property_read_u32(dev, "maxim,constant-current-disable", &outconf);
		if (ret && (ret != -EINVAL))
			return dev_err_probe(dev, -ENODEV,
					     "Failed to read maxim,constant-current-disable OF property\n");

		regmap_write(regmap, MAX7360_REG_GPIOOUTM, outconf);
	}

	/* Add gpio device. */
	gpio_config.parent = dev;
	gpio_config.regmap = regmap;
	if (gpio_function == MAX7360_GPIO_PORT) {
		gpio_config.ngpio = MAX7360_MAX_GPIO;
		gpio_config.reg_dat_base = GPIO_REGMAP_ADDR(MAX7360_REG_GPIOIN);
		gpio_config.reg_set_base = GPIO_REGMAP_ADDR(MAX7360_REG_PWMBASE);
		gpio_config.reg_dir_out_base = GPIO_REGMAP_ADDR(MAX7360_REG_GPIOCTRL);
		gpio_config.ngpio_per_reg = MAX7360_MAX_GPIO;
		gpio_config.request = max7360_gpio_request;
		gpio_config.free = max7360_gpio_free;
	} else {
		u32 ngpios;

		ret = device_property_read_u32(dev, "ngpios", &ngpios);
		if (ret < 0) {
			dev_err(dev, "Missing ngpios OF property\n");
			return ret;
		}

		gpio_config.reg_set_base = GPIO_REGMAP_ADDR(MAX7360_REG_PORTS);
		gpio_config.reg_mask_xlate = max7360_gpo_reg_mask_xlate;
		gpio_config.ngpio = ngpios;

		ret = max7360_set_gpos_count(dev, regmap);
		if (ret)
			return dev_err_probe(dev, ret, "Failed to set GPOS pin count\n");
	}

	return PTR_ERR_OR_ZERO(devm_gpio_regmap_register(dev, &gpio_config));
}

static const struct of_device_id max7360_gpio_of_match[] = {
	{
		.compatible = "maxim,max7360-gpo",
		.data = (void *)MAX7360_GPIO_COL
	}, {
		.compatible = "maxim,max7360-gpio",
		.data = (void *)MAX7360_GPIO_PORT
	}, {
	}
};
MODULE_DEVICE_TABLE(of, max7360_gpio_of_match);

static struct platform_driver max7360_gpio_driver = {
	.driver = {
		.name	= "max7360-gpio",
		.of_match_table = max7360_gpio_of_match,
	},
	.probe		= max7360_gpio_probe,
};
module_platform_driver(max7360_gpio_driver);

MODULE_DESCRIPTION("MAX7360 GPIO driver");
MODULE_AUTHOR("Kamel BOUHARA <kamel.bouhara@bootlin.com>");
MODULE_AUTHOR("Mathieu Dubois-Briand <mathieu.dubois-briand@bootlin.com>");
MODULE_LICENSE("GPL");
