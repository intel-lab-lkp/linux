// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2024 Bootlin
 *
 * Author: Kamel BOUHARA <kamel.bouhara@bootlin.com>
 * Author: Mathieu Dubois-Briand <mathieu.dubois-briand@bootlin.com>
 */

#include <linux/gpio/consumer.h>
#include <linux/gpio/driver.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/mfd/max7360.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/slab.h>

#define MAX7360_GPIO_PORT	1
#define MAX7360_GPIO_COL	2

struct max7360_gpio {
	struct gpio_chip chip;
	struct device *dev;
	struct regmap *regmap;
	unsigned long gpio_function;

	/*
	 * Interrupts handling data: only used when gpio_function is
	 * MAX7360_GPIO_PORT.
	 */
	u8 masked_interrupts;
	u8 in_values;
	unsigned int irq_types[MAX7360_MAX_GPIO];
};

static void max7360_gpio_set_value(struct gpio_chip *gc,
				   unsigned int pin, int state)
{
	struct max7360_gpio *max7360_gpio = gpiochip_get_data(gc);
	int ret;

	if (max7360_gpio->gpio_function == MAX7360_GPIO_COL) {
		int off = MAX7360_MAX_GPIO - (gc->ngpio - pin);

		ret = regmap_write_bits(max7360_gpio->regmap, MAX7360_REG_PORTS,
					BIT(off), state ? BIT(off) : 0);
	} else {
		ret = regmap_write(max7360_gpio->regmap,
				   MAX7360_REG_PWMBASE + pin, state ? 0xFF : 0);
	}

	if (ret)
		dev_err(max7360_gpio->dev,
			"failed to set value %d on gpio-%d", state, pin);
}

static int max7360_gpio_get_value(struct gpio_chip *gc, unsigned int pin)
{
	struct max7360_gpio *max7360_gpio = gpiochip_get_data(gc);
	unsigned int val;
	int off;
	int ret;

	if (max7360_gpio->gpio_function == MAX7360_GPIO_COL) {
		off = MAX7360_MAX_GPIO - (gc->ngpio - pin);

		ret = regmap_read(max7360_gpio->regmap, MAX7360_REG_PORTS, &val);
	} else {
		off = pin;
		ret = regmap_read(max7360_gpio->regmap, MAX7360_REG_GPIOIN, &val);
	}

	if (ret) {
		dev_err(max7360_gpio->dev, "failed to read gpio-%d", pin);
		return ret;
	}

	return !!(val & BIT(off));
}

static int max7360_gpio_get_direction(struct gpio_chip *gc, unsigned int pin)
{
	struct max7360_gpio *max7360_gpio = gpiochip_get_data(gc);
	unsigned int val;
	int ret;

	if (max7360_gpio->gpio_function == MAX7360_GPIO_COL)
		return GPIO_LINE_DIRECTION_OUT;

	ret = regmap_read(max7360_gpio->regmap, MAX7360_REG_GPIOCTRL, &val);
	if (ret) {
		dev_err(max7360_gpio->dev, "failed to read gpio-%d direction",
			pin);
		return ret;
	}

	if (val & BIT(pin))
		return GPIO_LINE_DIRECTION_OUT;

	return GPIO_LINE_DIRECTION_IN;
}

static int max7360_gpio_direction_input(struct gpio_chip *gc, unsigned int pin)
{
	struct max7360_gpio *max7360_gpio = gpiochip_get_data(gc);
	int ret;

	if (max7360_gpio->gpio_function == MAX7360_GPIO_COL)
		return -EIO;

	ret = regmap_write_bits(max7360_gpio->regmap, MAX7360_REG_GPIOCTRL,
				BIT(pin), 0);
	if (ret) {
		dev_err(max7360_gpio->dev, "failed to set gpio-%d direction",
			pin);
		return ret;
	}

	return 0;
}

static int max7360_gpio_direction_output(struct gpio_chip *gc, unsigned int pin,
					 int state)
{
	struct max7360_gpio *max7360_gpio = gpiochip_get_data(gc);
	int ret;

	if (max7360_gpio->gpio_function == MAX7360_GPIO_PORT) {
		ret = regmap_write_bits(max7360_gpio->regmap,
					MAX7360_REG_GPIOCTRL, BIT(pin),
					BIT(pin));
		if (ret) {
			dev_err(max7360_gpio->dev,
				"failed to set gpio-%d direction", pin);
			return ret;
		}
	}

	max7360_gpio_set_value(gc, pin, state);

	return 0;
}

static int max7360_gpio_request(struct gpio_chip *gc, unsigned int pin)
{
	struct max7360_gpio *max7360_gpio = gpiochip_get_data(gc);

	/*
	 * GPOs on COL pins (keypad columns) can always be requested: this
	 * driver has full access to them, up to the number set in chip.ngpio.
	 * GPIOs on PORT pins are shared with the PWM and rotary encoder
	 * drivers: they have to be requested from the MFD driver.
	 */
	if (max7360_gpio->gpio_function == MAX7360_GPIO_COL)
		return 0;

	return max7360_port_pin_request(max7360_gpio->dev->parent, pin, true);
}

static void max7360_gpio_free(struct gpio_chip *gc, unsigned int pin)
{
	struct max7360_gpio *max7360_gpio = gpiochip_get_data(gc);

	if (max7360_gpio->gpio_function == MAX7360_GPIO_COL)
		return;

	max7360_port_pin_request(max7360_gpio->dev->parent, pin, false);
}

static struct gpio_chip max7360_gpio_chip = {
	.label                  = "max7360",
	.request		= max7360_gpio_request,
	.free			= max7360_gpio_free,
	.get_direction		= max7360_gpio_get_direction,
	.direction_input	= max7360_gpio_direction_input,
	.direction_output	= max7360_gpio_direction_output,
	.get                    = max7360_gpio_get_value,
	.set                    = max7360_gpio_set_value,
	.base                   = -1,
	.can_sleep              = true,
};

static irqreturn_t max7360_gpio_irq(int irq, void *data)
{
	struct max7360_gpio *max7360_gpio = data;
	unsigned long pending;
	unsigned int gpio_irq;
	unsigned int type;
	unsigned int count = 0;
	int val;
	int irqn;
	int ret;

	ret = regmap_read(max7360_gpio->regmap, MAX7360_REG_GPIOIN, &val);
	if (ret) {
		dev_err(max7360_gpio->dev, "Failed to read gpio values: %d\n",
			ret);
		return IRQ_NONE;
	}

	/* MAX7360 generates interrupts but does not report which pins changed:
	 * compare the pin value with the value they had in previous interrupt
	 * and report interrupt if the change match the set IRQ type.
	 */
	pending = val ^ max7360_gpio->in_values;
	for_each_set_bit(irqn, &pending, max7360_gpio->chip.ngpio) {
		type = max7360_gpio->irq_types[irqn];
		if (max7360_gpio->masked_interrupts & BIT(irqn))
			continue;
		if ((val & BIT(irqn)) && type == IRQ_TYPE_EDGE_FALLING)
			continue;
		if (!(val & BIT(irqn)) && type == IRQ_TYPE_EDGE_RISING)
			continue;
		gpio_irq = irq_find_mapping(max7360_gpio->chip.irq.domain, irqn);
		handle_nested_irq(gpio_irq);
		count++;
	}

	max7360_gpio->in_values = val;

	if (count == 0)
		return IRQ_NONE;

	return IRQ_HANDLED;
}

static void max7360_gpio_irq_unmask(struct irq_data *data)
{
	struct max7360_gpio *max7360_gpio;
	unsigned int pin = irqd_to_hwirq(data);
	unsigned int val;
	int ret;

	max7360_gpio = gpiochip_get_data(irq_data_get_irq_chip_data(data));

	/* Read current pin value, so we know if the pin changed in the next
	 * interrupt.
	 * No lock should be needed regarding the interrupt handler: as long as
	 * the corresponding bit has not been cleared in masked_interrupts, this
	 * gpio is ignored.
	 */
	ret = regmap_read(max7360_gpio->regmap, MAX7360_REG_GPIOIN, &val);
	if (ret)
		dev_err(max7360_gpio->dev, "Failed to read gpio values: %d\n",
			ret);

	max7360_gpio->in_values &= ~BIT(pin);
	max7360_gpio->in_values |= val & BIT(pin);

	ret = regmap_write_bits(max7360_gpio->regmap, MAX7360_REG_PWMCFG + pin,
				MAX7360_PORT_CFG_INTERRUPT_MASK, 0);

	if (ret)
		dev_err(max7360_gpio->dev, "failed to unmask gpio-%d", pin);

	max7360_gpio->masked_interrupts &= ~BIT(pin);
}

static void max7360_gpio_irq_mask(struct irq_data *data)
{
	struct max7360_gpio *max7360_gpio;
	unsigned int pin = irqd_to_hwirq(data);
	int ret;

	max7360_gpio = gpiochip_get_data(irq_data_get_irq_chip_data(data));

	max7360_gpio->masked_interrupts |= BIT(pin);

	ret = regmap_write_bits(max7360_gpio->regmap, MAX7360_REG_PWMCFG + pin,
				MAX7360_PORT_CFG_INTERRUPT_MASK,
				MAX7360_PORT_CFG_INTERRUPT_MASK);

	if (ret)
		dev_err(max7360_gpio->dev, "failed to mask gpio-%d", pin);
}

static void max7360_gpio_irq_enable(struct irq_data *data)
{
	max7360_gpio_irq_unmask(data);
}

static void max7360_gpio_irq_disable(struct irq_data *data)
{
	max7360_gpio_irq_mask(data);
}

static int max7360_gpio_irq_set_type(struct irq_data *data,
				     unsigned int flow_type)
{
	struct max7360_gpio *max7360_gpio;
	unsigned int pin;
	unsigned int val;
	int ret;

	pin = irqd_to_hwirq(data);
	max7360_gpio = gpiochip_get_data(irq_data_get_irq_chip_data(data));

	switch (flow_type) {
	case IRQ_TYPE_EDGE_RISING:
		val = 0;
		break;
	case IRQ_TYPE_EDGE_FALLING:
	case IRQ_TYPE_EDGE_BOTH:
		val = MAX7360_PORT_CFG_INTERRUPT_EDGES;
		break;
	default:
		return -EINVAL;
	}
	ret = regmap_write_bits(max7360_gpio->regmap, MAX7360_REG_PWMCFG + pin,
				MAX7360_PORT_CFG_INTERRUPT_EDGES, val);

	if (ret)
		dev_err(max7360_gpio->dev, "failed to unmask gpio-%d", pin);

	max7360_gpio->irq_types[pin] = flow_type;

	return 0;
}

static const struct irq_chip max7360_gpio_irqchip = {
	.name = "max7360",
	.irq_enable = max7360_gpio_irq_enable,
	.irq_disable = max7360_gpio_irq_disable,
	.irq_mask = max7360_gpio_irq_mask,
	.irq_unmask = max7360_gpio_irq_unmask,
	.irq_set_type = max7360_gpio_irq_set_type,
	.flags = IRQCHIP_IMMUTABLE,
	GPIOCHIP_IRQ_RESOURCE_HELPERS,
};

static int max7360_gpio_probe(struct platform_device *pdev)
{
	struct max7360_gpio *max7360_gpio;
	struct platform_device *parent;
	unsigned int ngpios;
	unsigned int outconf;
	struct gpio_irq_chip *girq;
	unsigned long flags;
	int irq;
	int ret;

	if (!pdev->dev.parent) {
		dev_err(&pdev->dev, "no parent device\n");
		return -ENODEV;
	}
	parent = to_platform_device(pdev->dev.parent);

	max7360_gpio = devm_kzalloc(&pdev->dev, sizeof(struct max7360_gpio),
				    GFP_KERNEL);
	if (!max7360_gpio)
		return -ENOMEM;

	if (of_property_read_u32(pdev->dev.of_node, "ngpios", &ngpios)) {
		dev_err(&pdev->dev, "Missing ngpios OF property\n");
		return -ENODEV;
	}

	max7360_gpio->regmap = dev_get_regmap(pdev->dev.parent, NULL);
	if (!max7360_gpio->regmap) {
		dev_err(&pdev->dev, "could not get parent regmap\n");
		return -ENODEV;
	}

	max7360_gpio->dev = &pdev->dev;
	max7360_gpio->chip = max7360_gpio_chip;
	max7360_gpio->chip.ngpio = ngpios;
	max7360_gpio->chip.parent = &pdev->dev;
	max7360_gpio->chip.base = -1;
	max7360_gpio->gpio_function = (uintptr_t)device_get_match_data(&pdev->dev);

	dev_dbg(&pdev->dev, "gpio count: %d\n", max7360_gpio->chip.ngpio);

	if (max7360_gpio->gpio_function == MAX7360_GPIO_PORT) {
		/* Port GPIOs: set output mode configuration (constant-current
		 * or not).
		 * This property is optional.
		 */
		outconf = 0;
		ret = of_property_read_u32(pdev->dev.of_node,
					   "maxim,constant-current-disable",
					   &outconf);
		if (ret && (ret != -EINVAL)) {
			dev_err(&pdev->dev,
				"Failed to read maxim,constant-current-disable OF property\n");
			return -ENODEV;
		}

	    regmap_write(max7360_gpio->regmap, MAX7360_REG_GPIOOUTM, outconf);
	}

	if (max7360_gpio->gpio_function == MAX7360_GPIO_PORT &&
	    of_property_read_bool(pdev->dev.of_node, "interrupt-controller")) {
		/* Port GPIOs: declare IRQ chip, if configuration was provided.
		 */
		irq = platform_get_irq_byname(parent, "inti");
		if (irq < 0)
			return dev_err_probe(&pdev->dev, irq,
					     "Failed to get IRQ");

		flags = IRQF_TRIGGER_LOW | IRQF_ONESHOT | IRQF_SHARED;
		ret = devm_request_threaded_irq(&pdev->dev, irq, NULL,
						max7360_gpio_irq, flags,
						"max7360-gpio", max7360_gpio);
		if (ret)
			return dev_err_probe(&pdev->dev, ret,
					     "Failed to register interrupt: %d\n",
					     ret);

		girq = &max7360_gpio->chip.irq;
		gpio_irq_chip_set_chip(girq, &max7360_gpio_irqchip);
		girq->parent_handler = NULL;
		girq->num_parents = 0;
		girq->parents = NULL;
		girq->threaded = true;
		girq->default_type = IRQ_TYPE_NONE;
		girq->handler = handle_simple_irq;
	}

	ret = devm_gpiochip_add_data(&pdev->dev, &max7360_gpio->chip, max7360_gpio);
	if (ret) {
		dev_err(&pdev->dev, "unable to add gpiochip: %d\n", ret);
		return ret;
	}

	return 0;
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
		.name	= MAX7360_DRVNAME_GPIO,
		.of_match_table = of_match_ptr(max7360_gpio_of_match),
	},
	.probe		= max7360_gpio_probe,
};
module_platform_driver(max7360_gpio_driver);

MODULE_DESCRIPTION("MAX7360 GPIO driver");
MODULE_AUTHOR("Kamel BOUHARA <kamel.bouhara@bootlin.com>");
MODULE_AUTHOR("Mathieu Dubois-Briand <mathieu.dubois-briand@bootlin.com>");
MODULE_ALIAS("platform:" MAX7360_DRVNAME_GPIO);
MODULE_LICENSE("GPL");
