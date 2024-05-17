// SPDX-License-Identifier: GPL-2.0-only
/*
 * AD24xx GPIO driver
 *
 * Copyright (c) 2023-2024 Alvin Šipraga <alsi@bang-olufsen.dk>
 */

#include <linux/a2b/a2b.h>
#include <linux/a2b/ad24xx.h>
#include <linux/gpio/driver.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/of_irq.h>
#include <linux/regmap.h>

struct ad24xx_gpio {
	struct device *dev;
	struct a2b_func *func;
	struct a2b_node *node;
	struct regmap *regmap;
	int irqs[AD24XX_MAX_GPIOS];
	struct gpio_chip gpio_chip;
	struct irq_chip irq_chip;
	struct mutex mutex;
	unsigned int irq_invert : AD24XX_MAX_GPIOS;
	unsigned int irq_enable : AD24XX_MAX_GPIOS;
};

static int ad24xx_gpio_get_direction(struct gpio_chip *gc, unsigned int offset)
{
	struct ad24xx_gpio *adg = gpiochip_get_data(gc);
	unsigned int val;
	int ret;

	ret = regmap_read(adg->regmap, A2B_GPIOOEN, &val);
	if (ret)
		return ret;

	if (val & BIT(offset))
		return 0; /* output */

	return 1; /* input */
}

static int ad24xx_gpio_get(struct gpio_chip *gc, unsigned int offset)
{
	struct ad24xx_gpio *adg = gpiochip_get_data(gc);
	unsigned int val;
	int ret;

	ret = regmap_read(adg->regmap, A2B_GPIOIN, &val);
	if (ret)
		return ret;

	if (val & BIT(offset))
		return 1; /* high */

	return 0; /* low */
}

static void ad24xx_gpio_set(struct gpio_chip *gc, unsigned int offset,
			    int value)
{
	struct ad24xx_gpio *adg = gpiochip_get_data(gc);
	unsigned int reg = value ? A2B_GPIODATSET : A2B_GPIODATCLR;

	regmap_write(adg->regmap, reg, BIT(offset));
}

static int ad24xx_gpio_set_direction(struct ad24xx_gpio *adg,
				     unsigned int offset,
				     unsigned int direction)
{
	unsigned int mask = BIT(offset);
	unsigned int ival = direction ? BIT(offset) : 0;
	int ret;

	ret = regmap_update_bits(adg->regmap, A2B_GPIOIEN, mask, ival);
	if (ret)
		return ret;

	ret = regmap_update_bits(adg->regmap, A2B_GPIOOEN, mask, ~ival);
	if (ret)
		return ret;

	return 0;
}

static int ad24xx_gpio_direction_input(struct gpio_chip *gc,
				       unsigned int offset)
{
	struct ad24xx_gpio *adg = gpiochip_get_data(gc);

	return ad24xx_gpio_set_direction(adg, offset, 1);
}

static int ad24xx_gpio_direction_output(struct gpio_chip *gc,
					unsigned int offset, int value)
{
	struct ad24xx_gpio *adg = gpiochip_get_data(gc);

	/* For atomicity, write the output value before setting the direction */
	ad24xx_gpio_set(gc, offset, value);

	return ad24xx_gpio_set_direction(adg, offset, 0);
}

static int ad24xx_gpio_child_to_parent_hwirq(struct gpio_chip *gc,
					     unsigned int child,
					     unsigned int child_type,
					     unsigned int *parent,
					     unsigned int *parent_type)
{
	*parent = child;
	return 0;
}

static void ad24xx_gpio_irq_mask(struct irq_data *d)
{
	struct gpio_chip *gpio_chip = irq_data_get_irq_chip_data(d);
	struct ad24xx_gpio *adg = gpiochip_get_data(gpio_chip);
	irq_hw_number_t hwirq = irqd_to_hwirq(d);

	adg->irq_enable &= ~BIT(hwirq);
	gpiochip_disable_irq(gpio_chip, hwirq);
}

static void ad24xx_gpio_irq_unmask(struct irq_data *d)
{
	struct gpio_chip *gpio_chip = irq_data_get_irq_chip_data(d);
	struct ad24xx_gpio *adg = gpiochip_get_data(gpio_chip);
	irq_hw_number_t hwirq = irqd_to_hwirq(d);

	gpiochip_disable_irq(gpio_chip, hwirq);
	adg->irq_enable |= BIT(hwirq);
}

static int ad24xx_gpio_irq_set_type(struct irq_data *d, unsigned int type)
{
	struct gpio_chip *gpio_chip = irq_data_get_irq_chip_data(d);
	struct ad24xx_gpio *adg = gpiochip_get_data(gpio_chip);
	irq_hw_number_t hwirq = irqd_to_hwirq(d);

	switch (type) {
	case IRQ_TYPE_EDGE_RISING:
		adg->irq_invert &= ~BIT(hwirq);
		break;
	case IRQ_TYPE_EDGE_FALLING:
		adg->irq_invert |= BIT(hwirq);
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static void ad24xx_gpio_irq_bus_lock(struct irq_data *d)
{
	struct gpio_chip *gpio_chip = irq_data_get_irq_chip_data(d);
	struct ad24xx_gpio *adg = gpiochip_get_data(gpio_chip);

	mutex_lock(&adg->mutex);
}

static void ad24xx_gpio_irq_bus_sync_unlock(struct irq_data *d)
{
	struct gpio_chip *gpio_chip = irq_data_get_irq_chip_data(d);
	struct ad24xx_gpio *adg = gpiochip_get_data(gpio_chip);
	int ret;

	ret = regmap_write(adg->regmap, A2B_PINTINV, adg->irq_invert);
	if (ret)
		goto out;

	ret = regmap_write(adg->regmap, A2B_PINTEN, adg->irq_enable);
	if (ret)
		goto out;

out:
	mutex_unlock(&adg->mutex);

	if (ret)
		dev_err(adg->dev,
			"failed to update interrupt configuration: %d\n", ret);
}

static const struct irq_chip ad24xx_gpio_irq_chip = {
	.name = "ad24xx-gpio",
	.flags = IRQCHIP_IMMUTABLE,
	.irq_mask = ad24xx_gpio_irq_mask,
	.irq_unmask = ad24xx_gpio_irq_unmask,
	.irq_set_type = ad24xx_gpio_irq_set_type,
	.irq_bus_lock = ad24xx_gpio_irq_bus_lock,
	.irq_bus_sync_unlock = ad24xx_gpio_irq_bus_sync_unlock,
	GPIOCHIP_IRQ_RESOURCE_HELPERS,
};

static const struct regmap_config ad24xx_gpio_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
};

static int ad24xx_gpio_probe(struct device *dev)
{
	struct a2b_func *func = to_a2b_func(dev);
	struct a2b_node *node = func->node;
	struct fwnode_handle *fwnode = of_node_to_fwnode(dev->of_node);
	struct gpio_chip *gpio_chip;
	struct gpio_irq_chip *irq_chip;
	struct irq_domain *parent_domain;
	struct ad24xx_gpio *adg;
	struct device_node *np;
	int ret;

	adg = devm_kzalloc(dev, sizeof(*adg), GFP_KERNEL);
	if (!adg)
		return -ENOMEM;

	adg->regmap =
		devm_regmap_init_a2b_func(func, &ad24xx_gpio_regmap_config);
	if (IS_ERR(adg->regmap))
		return PTR_ERR(adg->regmap);

	adg->dev = dev;
	adg->func = func;
	adg->node = node;
	mutex_init(&adg->mutex);

	np = of_irq_find_parent(dev->of_node);
	if (!np)
		return -ENOENT;

	parent_domain = irq_find_host(np);
	of_node_put(np);
	if (!parent_domain)
		return -ENOENT;

	gpio_chip = &adg->gpio_chip;
	gpio_chip->label = dev_name(dev);
	gpio_chip->parent = dev;
	gpio_chip->fwnode = fwnode;
	gpio_chip->owner = THIS_MODULE;
	gpio_chip->get_direction = ad24xx_gpio_get_direction;
	gpio_chip->direction_input = ad24xx_gpio_direction_input;
	gpio_chip->direction_output = ad24xx_gpio_direction_output;
	gpio_chip->get = ad24xx_gpio_get;
	gpio_chip->set = ad24xx_gpio_set;
	gpio_chip->base = -1;
	gpio_chip->ngpio = node->chip_info->max_gpios;
	gpio_chip->can_sleep = true;

	irq_chip = &gpio_chip->irq;
	gpio_irq_chip_set_chip(irq_chip, &ad24xx_gpio_irq_chip);
	irq_chip->fwnode = fwnode;
	irq_chip->parent_domain = parent_domain;
	irq_chip->child_to_parent_hwirq = ad24xx_gpio_child_to_parent_hwirq;
	irq_chip->handler = handle_bad_irq;
	irq_chip->default_type = IRQ_TYPE_NONE;

	/* Initialize all GPIOs as inputs for high impedance state */
	ret = regmap_write(adg->regmap, A2B_GPIOIEN, 0xFF);
	if (ret)
		return ret;

	ret = devm_gpiochip_add_data(dev, gpio_chip, adg);
	if (ret)
		return ret;

	return 0;
}

static const struct of_device_id ad24xx_gpio_of_match_table[] = {
	{ .compatible = "adi,ad2401-gpio" },
	{ .compatible = "adi,ad2402-gpio" },
	{ .compatible = "adi,ad2403-gpio" },
	{ .compatible = "adi,ad2410-gpio" },
	{ .compatible = "adi,ad2420-gpio" },
	{ .compatible = "adi,ad2421-gpio" },
	{ .compatible = "adi,ad2422-gpio" },
	{ .compatible = "adi,ad2425-gpio" },
	{ .compatible = "adi,ad2426-gpio" },
	{ .compatible = "adi,ad2427-gpio" },
	{ .compatible = "adi,ad2428-gpio" },
	{ .compatible = "adi,ad2429-gpio" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, ad24xx_gpio_of_match_table);

static struct a2b_driver ad24xx_gpio_driver = {
	.driver = {
		.name = "ad24xx-gpio",
		.of_match_table = ad24xx_gpio_of_match_table,
		.probe_type = PROBE_PREFER_ASYNCHRONOUS,
	},
	.probe = ad24xx_gpio_probe,
};
module_a2b_driver(ad24xx_gpio_driver);

MODULE_AUTHOR("Alvin Šipraga <alsi@bang-olufsen.dk>");
MODULE_DESCRIPTION("AD24xx GPIO driver");
MODULE_LICENSE("GPL");
