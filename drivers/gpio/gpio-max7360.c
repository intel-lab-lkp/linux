// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2025 Bootlin
 *
 * Author: Kamel BOUHARA <kamel.bouhara@bootlin.com>
 * Author: Mathieu Dubois-Briand <mathieu.dubois-briand@bootlin.com>
 */

#include <linux/bitmap.h>
#include <linux/gpio/driver.h>
#include <linux/gpio/regmap.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/mfd/max7360.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/regmap.h>
#include <linux/slab.h>

#define MAX7360_GPIO_PORT	1
#define MAX7360_GPIO_COL	2

static int max7360_get_available_gpos(struct device *dev, unsigned int *available_gpios)
{
	u32 columns;
	int ret;

	ret = device_property_read_u32(dev->parent, "keypad,num-columns", &columns);
	if (ret < 0) {
		dev_err(dev, "Failed to read columns count\n");
		return ret;
	}

	*available_gpios = min(MAX7360_MAX_GPO, MAX7360_MAX_KEY_COLS - columns);

	return 0;
}

static int max7360_gpo_init_valid_mask(struct gpio_chip *gc,
				       unsigned long *valid_mask,
				       unsigned int ngpios)
{
	unsigned int available_gpios;
	int ret;

	ret = max7360_get_available_gpos(gc->parent, &available_gpios);
	if (ret)
		return ret;

	bitmap_clear(valid_mask, 0, MAX7360_MAX_KEY_COLS - ngpios);

	return 0;
}

static int max7360_set_gpos_count(struct device *dev, struct regmap *regmap)
{
	/*
	 * MAX7360 COL0 to COL7 pins can be used either as keypad columns,
	 * general purpose output or a mix of both.
	 * By default, all pins are used as keypad, here we update this
	 * configuration to allow to use some of them as GPIOs.
	 */
	unsigned int available_gpios;
	unsigned int val;
	int ret;

	ret = max7360_get_available_gpos(dev, &available_gpios);
	if (ret)
		return ret;

	/*
	 * Configure which GPIOs will be used for keypad.
	 * MAX7360_REG_DEBOUNCE contains configuration both for keypad debounce
	 * timings and gpos/keypad columns repartition. Only the later is
	 * modified here.
	 */
	val = FIELD_PREP(MAX7360_PORTS, available_gpios);
	ret = regmap_write_bits(regmap, MAX7360_REG_DEBOUNCE, MAX7360_PORTS, val);
	if (ret) {
		dev_err(dev, "Failed to write max7360 columns/gpos configuration");
		return ret;
	}

	return 0;
}

static int max7360_gpio_reg_mask_xlate(struct gpio_regmap *gpio,
				       unsigned int base, unsigned int offset,
				       unsigned int *reg, unsigned int *mask)
{
	if (base == MAX7360_REG_PWMBASE) {
		/*
		 * GPIO output is using PWM duty cycle registers: one register
		 * per line, with value being either 0 or 255.
		 */
		*reg = base + offset;
		*mask = 0xFF;
	} else {
		*reg = base;
		*mask = BIT(offset);
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
		val = (mask_buf & BIT(i)) ? MAX7360_PORT_CFG_INTERRUPT_MASK : 0;
		regmap_write_bits(regmap, MAX7360_REG_PWMCFG(i),
				  MAX7360_PORT_CFG_INTERRUPT_MASK, val);
	}

	return 0;
}

static int max7360_gpio_probe(struct platform_device *pdev)
{
	struct regmap_irq_chip *irq_chip;
	struct gpio_regmap_config gpio_config = { };
	struct device *dev = &pdev->dev;
	unsigned long gpio_function;
	struct regmap *regmap;
	unsigned int outconf;
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
		gpio_config.regmap_irq_flags = IRQF_TRIGGER_LOW | IRQF_ONESHOT | IRQF_SHARED;
		gpio_config.regmap_irq_irqno = fwnode_irq_get_byname(dev_fwnode(dev->parent),
								     "inti");
		if (gpio_config.regmap_irq_irqno < 0)
			return dev_err_probe(dev, gpio_config.regmap_irq_irqno,
					     "Failed to get IRQ\n");

		irq_chip = devm_kzalloc(dev, sizeof(*irq_chip), GFP_KERNEL);
		gpio_config.regmap_irq_chip = irq_chip;
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
	}

	if (gpio_function == MAX7360_GPIO_PORT) {
		/*
		 * Port GPIOs: set output mode configuration (constant-current or not).
		 * This property is optional.
		 */
		outconf = 0;
		ret = device_property_read_u32(dev, "maxim,constant-current-disable", &outconf);
		if (ret && (ret != -EINVAL))
			return dev_err_probe(dev, ret, "Failed to read %s device property\n",
					     "maxim,constant-current-disable");

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
		gpio_config.reg_mask_xlate = max7360_gpio_reg_mask_xlate;
	} else {
		ret = max7360_set_gpos_count(dev, regmap);
		if (ret)
			return dev_err_probe(dev, ret, "Failed to set GPOS pin count\n");

		gpio_config.reg_set_base = GPIO_REGMAP_ADDR(MAX7360_REG_PORTS);
		gpio_config.ngpio = MAX7360_MAX_KEY_COLS;
		gpio_config.init_valid_mask = max7360_gpo_init_valid_mask;
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
