// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Realtek DHC RTD1625 gpio driver
 *
 * Copyright (c) 2023-2026 Realtek Semiconductor Corp.
 */

#include <linux/bitfield.h>
#include <linux/bitops.h>
#include <linux/gpio/driver.h>
#include <linux/gpio/regmap.h>
#include <linux/interrupt.h>
#include <linux/irqchip.h>
#include <linux/irqdomain.h>
#include <linux/irqchip/chained_irq.h>
#include <linux/irqdomain.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/regmap.h>
#include <linux/spinlock.h>
#include <linux/types.h>

#define RTD1625_GPIO_DIR BIT(0)
#define RTD1625_GPIO_OUT BIT(2)
#define RTD1625_GPIO_IN BIT(4)
#define RTD1625_GPIO_EDGE_INT_DP BIT(6)
#define RTD1625_GPIO_EDGE_INT_EN BIT(8)
#define RTD1625_GPIO_LEVEL_INT_EN BIT(16)
#define RTD1625_GPIO_LEVEL_INT_DP BIT(18)
#define RTD1625_GPIO_DEBOUNCE GENMASK(30, 28)
#define RTD1625_GPIO_DEBOUNCE_WREN BIT(31)

#define RTD1625_GPIO_WREN(x) ((x) << 1)

/* Write-enable masks for all GPIO configs and reserved hardware bits */
#define RTD1625_ISO_GPIO_WREN_ALL 0x8000aa8a
#define RTD1625_ISOM_GPIO_WREN_ALL 0x800aaa8a

#define RTD1625_GPIO_DEBOUNCE_1US 0
#define RTD1625_GPIO_DEBOUNCE_10US 1
#define RTD1625_GPIO_DEBOUNCE_100US 2
#define RTD1625_GPIO_DEBOUNCE_1MS 3
#define RTD1625_GPIO_DEBOUNCE_10MS 4
#define RTD1625_GPIO_DEBOUNCE_20MS 5
#define RTD1625_GPIO_DEBOUNCE_30MS 6
#define RTD1625_GPIO_DEBOUNCE_50MS 7

#define GPIO_CONTROL(gpio) ((gpio) * 4)

/**
 * struct rtd1625_gpio_info - Specific GPIO register information
 * @num_gpios: The number of GPIOs
 * @irq_type_support: Supported IRQ types
 * @gpa_offset: Offset for GPIO assert interrupt status registers
 * @gpda_offset: Offset for GPIO deassert interrupt status registers
 * @level_offset: Offset of level interrupt status register
 * @write_en_all: Write-enable mask for all configurable bits
 */
struct rtd1625_gpio_info {
	unsigned int num_gpios;
	unsigned int irq_type_support;
	unsigned int base_offset;
	unsigned int gpa_offset;
	unsigned int gpda_offset;
	unsigned int level_offset;
	unsigned int write_en_all;
};

struct rtd1625_gpio {
	struct gpio_chip *gpio_chip;
	const struct rtd1625_gpio_info *info;
	void __iomem *base;
	struct regmap *regmap;
	unsigned int irqs[3];
	raw_spinlock_t lock;
	struct irq_domain *domain;
	unsigned int *save_regs;
};

static unsigned int rtd1625_gpio_gpa_offset(struct rtd1625_gpio *data, unsigned int offset)
{
	return data->info->gpa_offset + ((offset / 32) * 4);
}

static unsigned int rtd1625_gpio_gpda_offset(struct rtd1625_gpio *data, unsigned int offset)
{
	return data->info->gpda_offset + ((offset / 32) * 4);
}

static unsigned int rtd1625_gpio_level_offset(struct rtd1625_gpio *data, unsigned int offset)
{
	return data->info->level_offset + ((offset / 32) * 4);
}

static int rtd1625_reg_mask_xlate(struct gpio_regmap *gpio, enum gpio_regmap_operation op,
				  unsigned int base, unsigned int offset, unsigned int *reg,
				  unsigned int *mask)
{
	/* Each GPIO has its own dedicated 32-bit register */
	*reg = base + offset * 4;

	switch (op) {
	case GPIO_REGMAP_IN:
		*mask = RTD1625_GPIO_IN;
		break;
	case GPIO_REGMAP_OUT:
		*mask = RTD1625_GPIO_OUT;
		break;
	case GPIO_REGMAP_SET_WREN_OP:
		*mask = RTD1625_GPIO_WREN(RTD1625_GPIO_OUT);
		break;
	case GPIO_REGMAP_SET_WITH_CLEAR_OP:
	case GPIO_REGMAP_SET_OP:
		*mask = RTD1625_GPIO_OUT;
		break;
	case GPIO_REGMAP_SET_DIR_WREN_OP:
		*mask = RTD1625_GPIO_WREN(RTD1625_GPIO_DIR);
		break;
	case GPIO_REGMAP_GET_OP:
	case GPIO_REGMAP_GET_DIR_OP:
		*mask = RTD1625_GPIO_DIR;
		break;
	default:
		*mask = 0;
		break;
	}

	return 0;
}

static unsigned int rtd1625_gpio_set_debounce(struct gpio_chip *chip, unsigned int offset,
					      unsigned int debounce)
{
	struct rtd1625_gpio *data = gpiochip_get_data(chip);
	u8 deb_val;
	u32 val;

	switch (debounce) {
	case 1:
		deb_val = RTD1625_GPIO_DEBOUNCE_1US;
		break;
	case 10:
		deb_val = RTD1625_GPIO_DEBOUNCE_10US;
		break;
	case 100:
		deb_val = RTD1625_GPIO_DEBOUNCE_100US;
		break;
	case 1000:
		deb_val = RTD1625_GPIO_DEBOUNCE_1MS;
		break;
	case 10000:
		deb_val = RTD1625_GPIO_DEBOUNCE_10MS;
		break;
	case 20000:
		deb_val = RTD1625_GPIO_DEBOUNCE_20MS;
		break;
	case 30000:
		deb_val = RTD1625_GPIO_DEBOUNCE_30MS;
		break;
	case 50000:
		deb_val = RTD1625_GPIO_DEBOUNCE_50MS;
		break;
	default:
		return -ENOTSUPP;
	}

	val = FIELD_PREP(RTD1625_GPIO_DEBOUNCE, deb_val) | RTD1625_GPIO_DEBOUNCE_WREN;
	regmap_write(data->regmap, GPIO_CONTROL(offset), val);

	return 0;
}

static int rtd1625_gpio_set_config(struct gpio_chip *chip, unsigned int offset,
				   unsigned long config)
{
	int debounce;

	if (pinconf_to_config_param(config) == PIN_CONFIG_INPUT_DEBOUNCE) {
		debounce = pinconf_to_config_argument(config);
		return rtd1625_gpio_set_debounce(chip, offset, debounce);
	}

	return gpiochip_generic_config(chip, offset, config);
}

static void rtd1625_gpio_irq_handle(struct irq_desc *desc)
{
	unsigned int (*get_reg_offset)(struct rtd1625_gpio *gpio, unsigned int offset);
	struct rtd1625_gpio *data = irq_desc_get_handler_data(desc);
	struct irq_chip *chip = irq_desc_get_chip(desc);
	unsigned int irq = irq_desc_get_irq(desc);
	struct irq_domain *domain = data->domain;
	unsigned int reg_offset, i, j, val;
	irq_hw_number_t hwirq;
	unsigned long status;
	unsigned int girq;
	u32 irq_type;

	if (irq == data->irqs[0])
		get_reg_offset = &rtd1625_gpio_gpa_offset;
	else if (irq == data->irqs[1])
		get_reg_offset = &rtd1625_gpio_gpda_offset;
	else if (irq == data->irqs[2])
		get_reg_offset = &rtd1625_gpio_level_offset;
	else
		return;

	chained_irq_enter(chip, desc);

	for (i = 0; i < data->info->num_gpios; i += 32) {
		reg_offset = get_reg_offset(data, i);
		regmap_read(data->regmap, reg_offset, &val);

		status = val;

		/* Clear edge interrupts; level interrupts are cleared in ->irq_ack() */
		if (irq != data->irqs[2])
			regmap_write(data->regmap, reg_offset, status);

		for_each_set_bit(j, &status, 32) {
			hwirq = i + j;
			girq = irq_find_mapping(domain, hwirq);
			irq_type = irq_get_trigger_type(girq);

			if (irq == data->irqs[1] && irq_type != IRQ_TYPE_EDGE_BOTH)
				continue;

			generic_handle_domain_irq(domain, hwirq);
		}
	}

	chained_irq_exit(chip, desc);
}

static void rtd1625_gpio_ack_irq(struct irq_data *d)
{
	struct gpio_chip *gc = irq_data_get_irq_chip_data(d);
	irq_hw_number_t hwirq = irqd_to_hwirq(d);
	u32 irq_type = irqd_get_trigger_type(d);
	u32 bit_mask = BIT(hwirq % 32);
	struct rtd1625_gpio *data;
	struct gpio_regmap *gpio;
	int reg_offset;

	gpio = gpiochip_get_data(gc);
	data = gpio_regmap_get_drvdata(gpio);

	if (irq_type & IRQ_TYPE_LEVEL_MASK) {
		reg_offset = rtd1625_gpio_level_offset(data, hwirq);
		regmap_write(data->regmap, reg_offset, bit_mask);
	}
}

static void rtd1625_gpio_enable_edge_irq(struct rtd1625_gpio *data, irq_hw_number_t hwirq)
{
	int gpda_reg_offset = rtd1625_gpio_gpda_offset(data, hwirq);
	int gpa_reg_offset = rtd1625_gpio_gpa_offset(data, hwirq);
	u32 clr_mask = BIT(hwirq % 32);
	u32 val;

	guard(raw_spinlock_irqsave)(&data->lock);
	regmap_write(data->regmap, gpa_reg_offset, clr_mask);
	regmap_write(data->regmap, gpda_reg_offset, clr_mask);
	val = RTD1625_GPIO_EDGE_INT_EN | RTD1625_GPIO_WREN(RTD1625_GPIO_EDGE_INT_EN);
	regmap_write(data->regmap, data->info->base_offset + GPIO_CONTROL(hwirq), val);
}

static void rtd1625_gpio_disable_edge_irq(struct rtd1625_gpio *data, irq_hw_number_t hwirq)
{
	u32 val;

	guard(raw_spinlock_irqsave)(&data->lock);
	val = RTD1625_GPIO_WREN(RTD1625_GPIO_EDGE_INT_EN);
	regmap_write(data->regmap, data->info->base_offset + GPIO_CONTROL(hwirq), val);
}

static void rtd1625_gpio_enable_level_irq(struct rtd1625_gpio *data, irq_hw_number_t hwirq)
{
	int level_reg_offset = rtd1625_gpio_level_offset(data, hwirq);
	u32 clr_mask = BIT(hwirq % 32);
	u32 val;

	guard(raw_spinlock_irqsave)(&data->lock);
	regmap_write(data->regmap, level_reg_offset, clr_mask);
	val = RTD1625_GPIO_LEVEL_INT_EN | RTD1625_GPIO_WREN(RTD1625_GPIO_LEVEL_INT_EN);
	regmap_write(data->regmap, data->info->base_offset + GPIO_CONTROL(hwirq), val);
}

static void rtd1625_gpio_disable_level_irq(struct rtd1625_gpio *data, irq_hw_number_t hwirq)
{
	u32 val;

	guard(raw_spinlock_irqsave)(&data->lock);
	val = RTD1625_GPIO_WREN(RTD1625_GPIO_LEVEL_INT_EN);
	regmap_write(data->regmap, data->info->base_offset + GPIO_CONTROL(hwirq), val);
}

static void rtd1625_gpio_enable_irq(struct irq_data *d)
{
	struct gpio_chip *gc = irq_data_get_irq_chip_data(d);
	irq_hw_number_t hwirq = irqd_to_hwirq(d);
	u32 irq_type = irqd_get_trigger_type(d);
	struct rtd1625_gpio *data;
	struct gpio_regmap *gpio;

	gpio = gpiochip_get_data(gc);
	data = gpio_regmap_get_drvdata(gpio);

	gpiochip_enable_irq(gc, hwirq);

	if (irq_type & IRQ_TYPE_EDGE_BOTH)
		rtd1625_gpio_enable_edge_irq(data, hwirq);
	else if (irq_type & IRQ_TYPE_LEVEL_MASK)
		rtd1625_gpio_enable_level_irq(data, hwirq);
}

static void rtd1625_gpio_disable_irq(struct irq_data *d)
{
	struct gpio_chip *gc = irq_data_get_irq_chip_data(d);
	irq_hw_number_t hwirq = irqd_to_hwirq(d);
	u32 irq_type = irqd_get_trigger_type(d);
	struct rtd1625_gpio *data;
	struct gpio_regmap *gpio;

	gpio = gpiochip_get_data(gc);
	data = gpio_regmap_get_drvdata(gpio);

	if (irq_type & IRQ_TYPE_EDGE_BOTH)
		rtd1625_gpio_disable_edge_irq(data, hwirq);
	else if (irq_type & IRQ_TYPE_LEVEL_MASK)
		rtd1625_gpio_disable_level_irq(data, hwirq);

	gpiochip_disable_irq(gc, hwirq);
}

static int rtd1625_gpio_irq_set_level_type(struct irq_data *d, bool level)
{
	u32 val = RTD1625_GPIO_WREN(RTD1625_GPIO_LEVEL_INT_DP);
	struct gpio_chip *gc = irq_data_get_irq_chip_data(d);
	irq_hw_number_t hwirq = irqd_to_hwirq(d);
	struct rtd1625_gpio *data;
	struct gpio_regmap *gpio;

	gpio = gpiochip_get_data(gc);
	data = gpio_regmap_get_drvdata(gpio);
	if (!(data->info->irq_type_support & IRQ_TYPE_LEVEL_MASK))
		return -EINVAL;

	scoped_guard(raw_spinlock_irqsave, &data->lock) {
		if (level)
			val |= RTD1625_GPIO_LEVEL_INT_DP;
		regmap_write(data->regmap, data->info->base_offset + GPIO_CONTROL(hwirq), val);
	}

	irq_set_handler_locked(d, handle_level_irq);

	return 0;
}

static int rtd1625_gpio_irq_set_edge_type(struct irq_data *d, bool polarity)
{
	u32 val = RTD1625_GPIO_WREN(RTD1625_GPIO_EDGE_INT_DP);
	struct gpio_chip *gc = irq_data_get_irq_chip_data(d);
	irq_hw_number_t hwirq = irqd_to_hwirq(d);
	struct rtd1625_gpio *data;
	struct gpio_regmap *gpio;

	gpio = gpiochip_get_data(gc);
	data = gpio_regmap_get_drvdata(gpio);
	if (!(data->info->irq_type_support & IRQ_TYPE_EDGE_BOTH))
		return -EINVAL;

	scoped_guard(raw_spinlock_irqsave, &data->lock) {
		if (polarity)
			val |= RTD1625_GPIO_EDGE_INT_DP;
		regmap_write(data->regmap, data->info->base_offset + GPIO_CONTROL(hwirq), val);
	}

	irq_set_handler_locked(d, handle_edge_irq);

	return 0;
}

static int rtd1625_gpio_irq_set_type(struct irq_data *d, unsigned int type)
{
	int ret;

	switch (type & IRQ_TYPE_SENSE_MASK) {
	case IRQ_TYPE_EDGE_RISING:
		ret = rtd1625_gpio_irq_set_edge_type(d, 1);
		break;
	case IRQ_TYPE_EDGE_FALLING:
		ret = rtd1625_gpio_irq_set_edge_type(d, 0);
		break;
	case IRQ_TYPE_EDGE_BOTH:
		ret = rtd1625_gpio_irq_set_edge_type(d, 1);
		break;
	case IRQ_TYPE_LEVEL_HIGH:
		ret = rtd1625_gpio_irq_set_level_type(d, 0);
		break;
	case IRQ_TYPE_LEVEL_LOW:
		ret = rtd1625_gpio_irq_set_level_type(d, 1);
		break;
	default:
		ret = -EINVAL;
	}

	return ret;
}

static struct irq_chip rtd1625_iso_gpio_irq_chip = {
	.name = "rtd1625-gpio",
	.irq_ack = rtd1625_gpio_ack_irq,
	.irq_mask = rtd1625_gpio_disable_irq,
	.irq_unmask = rtd1625_gpio_enable_irq,
	.irq_set_type = rtd1625_gpio_irq_set_type,
	.flags = IRQCHIP_IMMUTABLE | IRQCHIP_SKIP_SET_WAKE,
	GPIOCHIP_IRQ_RESOURCE_HELPERS,
};

static int rtd1625_gpio_setup_irq(struct platform_device *pdev, struct rtd1625_gpio *data)
{
	int num_irqs, irq, i;

	irq = platform_get_irq_optional(pdev, 0);
	if (irq == -ENXIO)
		return 0;
	if (irq < 0)
		return irq;

	num_irqs = (data->info->irq_type_support & IRQ_TYPE_LEVEL_MASK) ? 3 : 2;

	for (i = 0; i < num_irqs; i++) {
		irq = platform_get_irq(pdev, i);
		if (irq < 0)
			return irq;

		data->irqs[i] = irq;
		irq_set_chained_handler_and_data(data->irqs[i], rtd1625_gpio_irq_handle, data);
	}

	return 0;
}

static int rtd1625_gpio_irq_map(struct irq_domain *domain, unsigned int irq,
				irq_hw_number_t hwirq)
{
	struct rtd1625_gpio *data = domain->host_data;

	irq_set_chip_data(irq, data->gpio_chip);

	irq_set_chip_and_handler(irq, &rtd1625_iso_gpio_irq_chip, handle_bad_irq);

	irq_set_noprobe(irq);

	return 0;
}

static const struct irq_domain_ops rtd1625_gpio_irq_domain_ops = {
	.map = rtd1625_gpio_irq_map,
	.xlate = irq_domain_xlate_twocell,
};

static const struct regmap_config rtd1625_gpio_regmap_config = {
	.reg_bits = 32,
	.reg_stride = 4,
	.val_bits = 32,
	.disable_locking = true,
};

static int rtd1625_gpio_probe(struct platform_device *pdev)
{
	struct gpio_regmap_config config = {0};
	struct device *dev = &pdev->dev;
	struct gpio_regmap *gpio_reg;
	struct rtd1625_gpio *data;
	void __iomem *irq_base;
	int ret;

	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->info = device_get_match_data(dev);
	if (!data->info)
		return -EINVAL;

	irq_base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(irq_base))
		return PTR_ERR(irq_base);

	data->regmap = devm_regmap_init_mmio(dev, irq_base,
					     &rtd1625_gpio_regmap_config);
	if (IS_ERR(data->regmap))
		return PTR_ERR(data->regmap);

	data->save_regs = devm_kzalloc(dev, data->info->num_gpios *
				       sizeof(*data->save_regs), GFP_KERNEL);
	if (!data->save_regs)
		return -ENOMEM;

	config.parent = dev;
	config.regmap = data->regmap;
	config.ngpio = data->info->num_gpios;
	config.reg_dat_base = data->info->base_offset;
	config.reg_set_base = data->info->base_offset;
	config.reg_mask_xlate = rtd1625_reg_mask_xlate;
	config.set_config = rtd1625_gpio_set_config;
	config.reg_dir_out_base = data->info->base_offset;

	data->domain = irq_domain_add_linear(dev->of_node,
					     data->info->num_gpios,
					     &rtd1625_gpio_irq_domain_ops,
					     data);
	if (!data->domain)
		return -ENOMEM;

	ret = rtd1625_gpio_setup_irq(pdev, data);
	if (ret) {
		irq_domain_remove(data->domain);
		return ret;
	}

	config.irq_domain = data->domain;
	config.drvdata = data;
	platform_set_drvdata(pdev, data);

	gpio_reg = devm_gpio_regmap_register(dev, &config);
	if (IS_ERR(gpio_reg)) {
		irq_domain_remove(data->domain);
		return PTR_ERR(gpio_reg);
	}

	data->gpio_chip = gpio_regmap_get_gpiochip(gpio_reg);

	return 0;
}

static const struct rtd1625_gpio_info rtd1625_iso_gpio_info = {
	.num_gpios		= 166,
	.irq_type_support	= IRQ_TYPE_EDGE_BOTH,
	.base_offset		= 0x100,
	.gpa_offset		= 0x0,
	.gpda_offset		= 0x20,
	.write_en_all		= RTD1625_ISO_GPIO_WREN_ALL,
};

static const struct rtd1625_gpio_info rtd1625_isom_gpio_info = {
	.num_gpios		= 4,
	.irq_type_support	= IRQ_TYPE_EDGE_BOTH | IRQ_TYPE_LEVEL_LOW |
				  IRQ_TYPE_LEVEL_HIGH,
	.base_offset		= 0x20,
	.gpa_offset		= 0x0,
	.gpda_offset		= 0x4,
	.level_offset		= 0x18,
	.write_en_all		= RTD1625_ISOM_GPIO_WREN_ALL,
};

static const struct of_device_id rtd1625_gpio_of_matches[] = {
	{ .compatible = "realtek,rtd1625-iso-gpio", .data = &rtd1625_iso_gpio_info },
	{ .compatible = "realtek,rtd1625-isom-gpio", .data = &rtd1625_isom_gpio_info },
	{ }
};
MODULE_DEVICE_TABLE(of, rtd1625_gpio_of_matches);

static int rtd1625_gpio_suspend(struct device *dev)
{
	struct rtd1625_gpio *data = dev_get_drvdata(dev);
	const struct rtd1625_gpio_info *info = data->info;
	int i;

	for (i = 0; i < info->num_gpios; i++)
		regmap_read(data->regmap, data->info->base_offset + GPIO_CONTROL(i),
			    &data->save_regs[i]);

	return 0;
}

static int rtd1625_gpio_resume(struct device *dev)
{
	struct rtd1625_gpio *data = dev_get_drvdata(dev);
	const struct rtd1625_gpio_info *info = data->info;
	int i;

	for (i = 0; i < info->num_gpios; i++)
		regmap_write(data->regmap, data->info->base_offset + GPIO_CONTROL(i),
			     data->save_regs[i] | info->write_en_all);

	return 0;
}

DEFINE_NOIRQ_DEV_PM_OPS(rtd1625_gpio_pm_ops, rtd1625_gpio_suspend, rtd1625_gpio_resume);

static struct platform_driver rtd1625_gpio_platform_driver = {
	.driver = {
		.name = "gpio-rtd1625",
		.of_match_table = rtd1625_gpio_of_matches,
		.pm = pm_sleep_ptr(&rtd1625_gpio_pm_ops),
	},
	.probe = rtd1625_gpio_probe,
};
module_platform_driver(rtd1625_gpio_platform_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Realtek Semiconductor Corporation");
MODULE_DESCRIPTION("Realtek DHC SoC RTD1625 gpio driver");
