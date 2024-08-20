// SPDX-License-Identifier: GPL-2.0
/*
 * Driver for Raspberry Pi RP1 GPIO unit
 *
 * Copyright (C) 2023 Raspberry Pi Ltd.
 *
 * This driver is inspired by:
 * pinctrl-bcm2835.c, please see original file for copyright information
 */

#include <linux/bitmap.h>
#include <linux/bitops.h>
#include <linux/bug.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/gpio/driver.h>
#include <linux/io.h>
#include <linux/irq.h>
#include <linux/irqdesc.h>
#include <linux/init.h>
#include <linux/of_address.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>
#include <linux/seq_file.h>
#include <linux/spinlock.h>
#include <linux/types.h>

#define MODULE_NAME "pinctrl-rp1"
#define RP1_NUM_GPIOS	54
#define RP1_NUM_BANKS	3

#define RP1_RW_OFFSET			0x0000
#define RP1_XOR_OFFSET			(RP1_RW_OFFSET + 0x1000)
#define RP1_SET_OFFSET			(RP1_RW_OFFSET + 0x2000)
#define RP1_CLR_OFFSET			(RP1_RW_OFFSET + 0x3000)

#define RP1_GPIO_REG_OFFSET		0x0000
#define RP1_GPIO_STATUS			(RP1_GPIO_REG_OFFSET + 0x0000)
#define RP1_GPIO_CTRL			(RP1_GPIO_REG_OFFSET + 0x0004)

#define RP1_GPIO_EVENTS_SHIFT_RAW	20

#define RP1_GPIO_CTRL_FUNCSEL_LSB	0
#define RP1_GPIO_CTRL_FUNCSEL_MASK	GENMASK(RP1_GPIO_CTRL_FUNCSEL_LSB + 4, \
						RP1_GPIO_CTRL_FUNCSEL_LSB)
#define RP1_GPIO_CTRL_OUTOVER_LSB	12
#define RP1_GPIO_CTRL_OUTOVER_MASK	GENMASK(RP1_GPIO_CTRL_OUTOVER_LSB + 1, \
						RP1_GPIO_CTRL_OUTOVER_LSB)
#define RP1_GPIO_CTRL_OEOVER_LSB	14
#define RP1_GPIO_CTRL_OEOVER_MASK	GENMASK(RP1_GPIO_CTRL_OEOVER_LSB + 1, \
						RP1_GPIO_CTRL_OEOVER_LSB)
#define RP1_GPIO_CTRL_INOVER_LSB	16
#define RP1_GPIO_CTRL_INOVER_MASK	GENMASK(RP1_GPIO_CTRL_INOVER_LSB + 1, \
						RP1_GPIO_CTRL_INOVER_LSB)
#define RP1_GPIO_CTRL_IRQEN_FALLING	BIT(20)
#define RP1_GPIO_CTRL_IRQEN_RISING	BIT(21)
#define RP1_GPIO_CTRL_IRQEN_LOW		BIT(22)
#define RP1_GPIO_CTRL_IRQEN_HIGH	BIT(23)
#define RP1_GPIO_CTRL_IRQEN_F_FALLING	BIT(24)
#define RP1_GPIO_CTRL_IRQEN_F_RISING	BIT(25)
#define RP1_GPIO_CTRL_IRQEN_F_LOW	BIT(26)
#define RP1_GPIO_CTRL_IRQEN_F_HIGH	BIT(27)
#define RP1_GPIO_CTRL_IRQRESET		BIT(28)
#define RP1_GPIO_CTRL_IRQOVER_LSB	30
#define RP1_GPIO_CTRL_IRQOVER_MASK	GENMASK(RP1_GPIO_CTRL_IRQOVER_LSB + 1, \
						RP1_GPIO_CTRL_IRQOVER_LSB)

#define RP1_INT_EDGE_FALLING		BIT(0)
#define RP1_INT_EDGE_RISING		BIT(1)
#define RP1_INT_LEVEL_LOW		BIT(2)
#define RP1_INT_LEVEL_HIGH		BIT(3)
#define RP1_INT_MASK			GENMASK(3, 0)

#define RP1_INT_EDGE_BOTH		(RP1_INT_EDGE_FALLING |	\
					 RP1_INT_EDGE_RISING)

enum {
	RP1_PUD_OFF			= 0,
	RP1_PUD_DOWN			= 1,
	RP1_PUD_UP			= 2,
};

#define RP1_FSEL_COUNT			9

#define RP1_FSEL_ALT0			0x00
#define RP1_FSEL_GPIO			0x05
#define RP1_FSEL_NONE			0x09
#define RP1_FSEL_NONE_HW		0x1f

enum {
	RP1_DIR_OUTPUT			= 0,
	RP1_DIR_INPUT			= 1,
};

enum {
	RP1_OUTOVER_PERI		= 0,
	RP1_OUTOVER_INVPERI		= 1,
	RP1_OUTOVER_LOW			= 2,
	RP1_OUTOVER_HIGH		= 3,
};

enum {
	RP1_OEOVER_PERI			= 0,
	RP1_OEOVER_INVPERI		= 1,
	RP1_OEOVER_DISABLE		= 2,
	RP1_OEOVER_ENABLE		= 3,
};

enum {
	RP1_INOVER_PERI			= 0,
	RP1_INOVER_INVPERI		= 1,
	RP1_INOVER_LOW			= 2,
	RP1_INOVER_HIGH			= 3,
};

#define RP1_RIO_OUT			0x00
#define RP1_RIO_OE			(RP1_RIO_OUT + 0x04)
#define RP1_RIO_IN			(RP1_RIO_OUT + 0x08)

#define RP1_PAD_SLEWFAST_LSB		0
#define RP1_PAD_SLEWFAST_MASK		BIT(RP1_PAD_SLEWFAST_LSB)
#define RP1_PAD_SCHMITT_LSB		1
#define RP1_PAD_SCHMITT_MASK		BIT(RP1_PAD_SCHMITT_LSB)
#define RP1_PAD_PULL_LSB		2
#define RP1_PAD_PULL_MASK		GENMASK(RP1_PAD_PULL_LSB + 1, \
						RP1_PAD_PULL_LSB)
#define RP1_PAD_DRIVE_LSB		4
#define RP1_PAD_DRIVE_MASK		GENMASK(RP1_PAD_DRIVE_LSB + 1, \
						RP1_PAD_DRIVE_LSB)
#define RP1_PAD_IN_ENABLE_LSB		6
#define RP1_PAD_IN_ENABLE_MASK		BIT(RP1_PAD_IN_ENABLE_LSB)
#define RP1_PAD_OUT_DISABLE_LSB		7
#define RP1_PAD_OUT_DISABLE_MASK	BIT(RP1_PAD_OUT_DISABLE_LSB)

#define RP1_PAD_DRIVE_2MA		0x00000000
#define RP1_PAD_DRIVE_4MA		BIT(4)
#define RP1_PAD_DRIVE_8MA		BIT(5)
#define RP1_PAD_DRIVE_12MA		(RP1_PAD_DRIVE_4MA | \
					RP1_PAD_DRIVE_8MA)

#define FIELD_SET(_reg, _mask, _val)			\
	({						\
		_reg &= ~(_mask);				\
		_reg |= FIELD_PREP((_mask), (_val));	\
	})

#define FUNC(f) \
	[func_##f] = #f

struct rp1_iobank_desc {
	int min_gpio;
	int num_gpios;
	int gpio_offset;
	int inte_offset;
	int ints_offset;
	int rio_offset;
	int pads_offset;
};

struct rp1_pin_info {
	u8 num;
	u8 bank;
	u8 offset;
	u8 fsel;
	u8 irq_type;

	void __iomem *gpio;
	void __iomem *rio;
	void __iomem *inte;
	void __iomem *ints;
	void __iomem *pad;
};

struct rp1_pinctrl {
	struct device *dev;
	void __iomem *gpio_base;
	void __iomem *rio_base;
	void __iomem *pads_base;
	int irq[RP1_NUM_BANKS];
	struct rp1_pin_info pins[RP1_NUM_GPIOS];

	struct pinctrl_dev *pctl_dev;
	struct gpio_chip gpio_chip;
	struct pinctrl_gpio_range gpio_range;

	raw_spinlock_t irq_lock[RP1_NUM_BANKS];
};

const struct rp1_iobank_desc rp1_iobanks[RP1_NUM_BANKS] = {
	/*         gpio   inte    ints     rio    pads */
	{  0, 28, 0x0000, 0x011c, 0x0124, 0x0000, 0x0004 },
	{ 28,  6, 0x4000, 0x411c, 0x4124, 0x4000, 0x4004 },
	{ 34, 20, 0x8000, 0x811c, 0x8124, 0x8000, 0x8004 },
};

static int rp1_pinconf_set(struct rp1_pin_info *pin,
			   unsigned int offset, unsigned long *configs,
			   unsigned int num_configs);

static struct rp1_pin_info *rp1_get_pin(struct gpio_chip *chip,
					unsigned int offset)
{
	struct rp1_pinctrl *pc = gpiochip_get_data(chip);

	if (pc && offset < RP1_NUM_GPIOS)
		return &pc->pins[offset];
	return NULL;
}

static void rp1_pad_update(struct rp1_pin_info *pin, u32 clr, u32 set)
{
	u32 padctrl = readl(pin->pad);

	padctrl &= ~clr;
	padctrl |= set;

	writel(padctrl, pin->pad);
}

static void rp1_input_enable(struct rp1_pin_info *pin, int value)
{
	rp1_pad_update(pin, RP1_PAD_IN_ENABLE_MASK,
		       value ? RP1_PAD_IN_ENABLE_MASK : 0);
}

static void rp1_output_enable(struct rp1_pin_info *pin, int value)
{
	rp1_pad_update(pin, RP1_PAD_OUT_DISABLE_MASK,
		       value ? 0 : RP1_PAD_OUT_DISABLE_MASK);
}

static u32 rp1_get_fsel(struct rp1_pin_info *pin)
{
	u32 ctrl = readl(pin->gpio + RP1_GPIO_CTRL);
	u32 oeover = FIELD_GET(RP1_GPIO_CTRL_OEOVER_MASK, ctrl);
	u32 fsel = FIELD_GET(RP1_GPIO_CTRL_FUNCSEL_MASK, ctrl);

	if (oeover != RP1_OEOVER_PERI || fsel >= RP1_FSEL_COUNT)
		fsel = RP1_FSEL_NONE;

	return fsel;
}

static void rp1_set_fsel(struct rp1_pin_info *pin, u32 fsel)
{
	u32 ctrl = readl(pin->gpio + RP1_GPIO_CTRL);

	if (fsel >= RP1_FSEL_COUNT)
		fsel = RP1_FSEL_NONE_HW;

	rp1_input_enable(pin, 1);
	rp1_output_enable(pin, 1);

	if (fsel == RP1_FSEL_NONE) {
		FIELD_SET(ctrl, RP1_GPIO_CTRL_OEOVER_MASK, RP1_OEOVER_DISABLE);
	} else {
		FIELD_SET(ctrl, RP1_GPIO_CTRL_OUTOVER_MASK, RP1_OUTOVER_PERI);
		FIELD_SET(ctrl, RP1_GPIO_CTRL_OEOVER_MASK, RP1_OEOVER_PERI);
	}

	FIELD_SET(ctrl, RP1_GPIO_CTRL_FUNCSEL_MASK, fsel);
	writel(ctrl, pin->gpio + RP1_GPIO_CTRL);
}

static int rp1_get_dir(struct rp1_pin_info *pin)
{
	return !(readl(pin->rio + RP1_RIO_OE) & (1 << pin->offset)) ?
		RP1_DIR_INPUT : RP1_DIR_OUTPUT;
}

static void rp1_set_dir(struct rp1_pin_info *pin, bool is_input)
{
	int offset = is_input ? RP1_CLR_OFFSET : RP1_SET_OFFSET;

	writel(1 << pin->offset, pin->rio + RP1_RIO_OE + offset);
}

static int rp1_get_value(struct rp1_pin_info *pin)
{
	return !!(readl(pin->rio + RP1_RIO_IN) & (1 << pin->offset));
}

static void rp1_set_value(struct rp1_pin_info *pin, int value)
{
	/* Assume the pin is already an output */
	writel(1 << pin->offset,
	       pin->rio + RP1_RIO_OUT + (value ? RP1_SET_OFFSET : RP1_CLR_OFFSET));
}

static int rp1_gpio_get(struct gpio_chip *chip, unsigned int offset)
{
	struct rp1_pin_info *pin = rp1_get_pin(chip, offset);
	int ret;

	if (!pin)
		return -EINVAL;

	ret = rp1_get_value(pin);

	return ret;
}

static void rp1_gpio_set(struct gpio_chip *chip, unsigned int offset, int value)
{
	struct rp1_pin_info *pin = rp1_get_pin(chip, offset);

	if (pin)
		rp1_set_value(pin, value);
}

static int rp1_gpio_get_direction(struct gpio_chip *chip, unsigned int offset)
{
	struct rp1_pin_info *pin = rp1_get_pin(chip, offset);
	u32 fsel;

	if (!pin)
		return -EINVAL;

	fsel = rp1_get_fsel(pin);
	if (fsel != RP1_FSEL_GPIO)
		return -EINVAL;

	return (rp1_get_dir(pin) == RP1_DIR_OUTPUT) ?
		GPIO_LINE_DIRECTION_OUT :
		GPIO_LINE_DIRECTION_IN;
}

static int rp1_gpio_direction_input(struct gpio_chip *chip, unsigned int offset)
{
	struct rp1_pin_info *pin = rp1_get_pin(chip, offset);

	if (!pin)
		return -EINVAL;
	rp1_set_dir(pin, RP1_DIR_INPUT);
	rp1_set_fsel(pin, RP1_FSEL_GPIO);

	return 0;
}

static int rp1_gpio_direction_output(struct gpio_chip *chip, unsigned int offset,
				     int value)
{
	struct rp1_pin_info *pin = rp1_get_pin(chip, offset);

	if (!pin)
		return -EINVAL;
	rp1_set_value(pin, value);
	rp1_set_dir(pin, RP1_DIR_OUTPUT);
	rp1_set_fsel(pin, RP1_FSEL_GPIO);

	return 0;
}

static int rp1_gpio_set_config(struct gpio_chip *chip, unsigned int offset,
			       unsigned long config)
{
	struct rp1_pin_info *pin = rp1_get_pin(chip, offset);
	unsigned long configs[] = { config };

	return rp1_pinconf_set(pin, offset, configs,
			       ARRAY_SIZE(configs));
}

static const struct gpio_chip rp1_gpio_chip = {
	.label = MODULE_NAME,
	.owner = THIS_MODULE,
	.request = gpiochip_generic_request,
	.free = gpiochip_generic_free,
	.direction_input = rp1_gpio_direction_input,
	.direction_output = rp1_gpio_direction_output,
	.get_direction = rp1_gpio_get_direction,
	.get = rp1_gpio_get,
	.set = rp1_gpio_set,
	.base = -1,
	.set_config = rp1_gpio_set_config,
	.ngpio = RP1_NUM_GPIOS,
	.can_sleep = false,
};

static void rp1_gpio_irq_handler(struct irq_desc *desc)
{
	struct gpio_chip *chip = irq_desc_get_handler_data(desc);
	struct irq_chip *host_chip = irq_desc_get_chip(desc);
	struct rp1_pinctrl *pc = gpiochip_get_data(chip);
	const struct rp1_iobank_desc *bank;
	int irq = irq_desc_get_irq(desc);
	unsigned long ints;
	int bit_pos;

	if (pc->irq[0] == irq)
		bank = &rp1_iobanks[0];
	else if (pc->irq[1] == irq)
		bank = &rp1_iobanks[1];
	else
		bank = &rp1_iobanks[2];

	chained_irq_enter(host_chip, desc);

	ints = readl(pc->gpio_base + bank->ints_offset);
	for_each_set_bit(bit_pos, &ints, 32) {
		struct rp1_pin_info *pin = rp1_get_pin(chip, bit_pos);

		writel(RP1_GPIO_CTRL_IRQRESET,
		       pin->gpio + RP1_SET_OFFSET + RP1_GPIO_CTRL);
		generic_handle_irq(irq_linear_revmap(pc->gpio_chip.irq.domain,
						     bank->gpio_offset + bit_pos));
	}

	chained_irq_exit(host_chip, desc);
}

static void rp1_gpio_irq_config(struct rp1_pin_info *pin, bool enable)
{
	writel(1 << pin->offset,
	       pin->inte + (enable ? RP1_SET_OFFSET : RP1_CLR_OFFSET));
	if (!enable)
		/* Clear any latched events */
		writel(RP1_GPIO_CTRL_IRQRESET,
		       pin->gpio + RP1_SET_OFFSET + RP1_GPIO_CTRL);
}

static void rp1_gpio_irq_enable(struct irq_data *data)
{
	struct gpio_chip *chip = irq_data_get_irq_chip_data(data);
	unsigned int gpio = irqd_to_hwirq(data);
	struct rp1_pin_info *pin = rp1_get_pin(chip, gpio);

	rp1_gpio_irq_config(pin, true);
}

static void rp1_gpio_irq_disable(struct irq_data *data)
{
	struct gpio_chip *chip = irq_data_get_irq_chip_data(data);
	unsigned int gpio = irqd_to_hwirq(data);
	struct rp1_pin_info *pin = rp1_get_pin(chip, gpio);

	rp1_gpio_irq_config(pin, false);
}

static int rp1_irq_set_type(struct rp1_pin_info *pin, unsigned int type)
{
	u32 irq_flags;

	switch (type) {
	case IRQ_TYPE_NONE:
		irq_flags = 0;
		break;
	case IRQ_TYPE_EDGE_RISING:
		irq_flags = RP1_INT_EDGE_RISING;
		break;
	case IRQ_TYPE_EDGE_FALLING:
		irq_flags = RP1_INT_EDGE_FALLING;
		break;
	case IRQ_TYPE_EDGE_BOTH:
		irq_flags = RP1_INT_EDGE_RISING | RP1_INT_EDGE_FALLING;
		break;
	case IRQ_TYPE_LEVEL_HIGH:
		irq_flags = RP1_INT_LEVEL_HIGH;
		break;
	case IRQ_TYPE_LEVEL_LOW:
		irq_flags = RP1_INT_LEVEL_LOW;
		break;

	default:
		return -EINVAL;
	}

	/* Clear them all */
	writel(RP1_INT_MASK << RP1_GPIO_EVENTS_SHIFT_RAW,
	       pin->gpio + RP1_CLR_OFFSET + RP1_GPIO_CTRL);
	/* Set those that are needed */
	writel(irq_flags << RP1_GPIO_EVENTS_SHIFT_RAW,
	       pin->gpio + RP1_SET_OFFSET + RP1_GPIO_CTRL);
	pin->irq_type = type;

	return 0;
}

static int rp1_gpio_irq_set_type(struct irq_data *data, unsigned int type)
{
	struct gpio_chip *chip = irq_data_get_irq_chip_data(data);
	unsigned int gpio = irqd_to_hwirq(data);
	struct rp1_pin_info *pin = rp1_get_pin(chip, gpio);
	struct rp1_pinctrl *pc = gpiochip_get_data(chip);
	int bank = pin->bank;
	unsigned long flags;
	int ret;

	raw_spin_lock_irqsave(&pc->irq_lock[bank], flags);

	ret = rp1_irq_set_type(pin, type);
	if (!ret) {
		if (type & IRQ_TYPE_EDGE_BOTH)
			irq_set_handler_locked(data, handle_edge_irq);
		else
			irq_set_handler_locked(data, handle_level_irq);
	}

	raw_spin_unlock_irqrestore(&pc->irq_lock[bank], flags);

	return ret;
}

static void rp1_gpio_irq_ack(struct irq_data *data)
{
	struct gpio_chip *chip = irq_data_get_irq_chip_data(data);
	unsigned int gpio = irqd_to_hwirq(data);
	struct rp1_pin_info *pin = rp1_get_pin(chip, gpio);

	/* Clear any latched events */
	writel(RP1_GPIO_CTRL_IRQRESET, pin->gpio + RP1_SET_OFFSET + RP1_GPIO_CTRL);
}

static struct irq_chip rp1_gpio_irq_chip = {
	.name = MODULE_NAME,
	.irq_enable = rp1_gpio_irq_enable,
	.irq_disable = rp1_gpio_irq_disable,
	.irq_set_type = rp1_gpio_irq_set_type,
	.irq_ack = rp1_gpio_irq_ack,
	.irq_mask = rp1_gpio_irq_disable,
	.irq_unmask = rp1_gpio_irq_enable,
	.flags = IRQCHIP_IMMUTABLE,
};

static void rp1_pull_config_set(struct rp1_pin_info *pin, unsigned int arg)
{
	u32 padctrl = readl(pin->pad);

	FIELD_SET(padctrl, RP1_PAD_PULL_MASK, arg & 0x3);
	writel(padctrl, pin->pad);
}

static int rp1_pinconf_set(struct rp1_pin_info *pin, unsigned int offset,
			   unsigned long *configs, unsigned int num_configs)
{
	u32 param, arg;
	int i;

	if (!pin)
		return -EINVAL;

	for (i = 0; i < num_configs; i++) {
		param = pinconf_to_config_param(configs[i]);
		arg = pinconf_to_config_argument(configs[i]);

		switch (param) {
		case PIN_CONFIG_BIAS_DISABLE:
			rp1_pull_config_set(pin, RP1_PUD_OFF);
			break;

		case PIN_CONFIG_BIAS_PULL_DOWN:
			rp1_pull_config_set(pin, RP1_PUD_DOWN);
			break;

		case PIN_CONFIG_BIAS_PULL_UP:
			rp1_pull_config_set(pin, RP1_PUD_UP);
			break;

		case PIN_CONFIG_INPUT_ENABLE:
			rp1_input_enable(pin, arg);
			break;

		case PIN_CONFIG_OUTPUT_ENABLE:
			rp1_output_enable(pin, arg);
			break;

		case PIN_CONFIG_OUTPUT:
			rp1_set_value(pin, arg);
			rp1_set_dir(pin, RP1_DIR_OUTPUT);
			rp1_set_fsel(pin, RP1_FSEL_GPIO);
			break;

		case PIN_CONFIG_INPUT_SCHMITT_ENABLE:
			rp1_pad_update(pin, RP1_PAD_SCHMITT_MASK,
				       arg ? RP1_PAD_SCHMITT_MASK : 0);
			break;

		case PIN_CONFIG_SLEW_RATE:
			rp1_pad_update(pin, RP1_PAD_SLEWFAST_MASK,
				       arg ? RP1_PAD_SLEWFAST_MASK : 0);
			break;

		case PIN_CONFIG_DRIVE_STRENGTH:
			switch (arg) {
			case 2:
				arg = RP1_PAD_DRIVE_2MA;
				break;
			case 4:
				arg = RP1_PAD_DRIVE_4MA;
				break;
			case 8:
				arg = RP1_PAD_DRIVE_8MA;
				break;
			case 12:
				arg = RP1_PAD_DRIVE_12MA;
				break;
			default:
				return -ENOTSUPP;
			}
			rp1_pad_update(pin, RP1_PAD_DRIVE_MASK, arg);
			break;

		default:
			return -ENOTSUPP;

		} /* switch param type */
	} /* for each config */

	return 0;
}

static const struct of_device_id rp1_pinctrl_match[] = {
	{ .compatible = "raspberrypi,rp1-gpio" },
	{},
};

static int rp1_pinctrl_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct gpio_irq_chip *girq;
	struct rp1_pinctrl *pc;
	int err, i;

	pc = devm_kzalloc(dev, sizeof(*pc), GFP_KERNEL);
	if (!pc)
		return -ENOMEM;

	pc->dev = dev;
	pc->gpio_chip = rp1_gpio_chip;
	pc->gpio_chip.parent = dev;

	pc->gpio_base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(pc->gpio_base)) {
		dev_err(dev, "could not get GPIO IO memory\n");
		return PTR_ERR(pc->gpio_base);
	}

	pc->rio_base = devm_platform_ioremap_resource(pdev, 1);
	if (IS_ERR(pc->rio_base)) {
		dev_err(dev, "could not get RIO IO memory\n");
		return PTR_ERR(pc->rio_base);
	}

	pc->pads_base = devm_platform_ioremap_resource(pdev, 2);
	if (IS_ERR(pc->pads_base)) {
		dev_err(dev, "could not get PADS IO memory\n");
		return PTR_ERR(pc->pads_base);
	}

	for (i = 0; i < RP1_NUM_BANKS; i++) {
		const struct rp1_iobank_desc *bank = &rp1_iobanks[i];
		int j;

		for (j = 0; j < bank->num_gpios; j++) {
			struct rp1_pin_info *pin =
				&pc->pins[bank->min_gpio + j];

			pin->num = bank->min_gpio + j;
			pin->bank = i;
			pin->offset = j;

			pin->gpio = pc->gpio_base + bank->gpio_offset +
				    j * sizeof(u32) * 2;
			pin->inte = pc->gpio_base + bank->inte_offset;
			pin->ints = pc->gpio_base + bank->ints_offset;
			pin->rio  = pc->rio_base + bank->rio_offset;
			pin->pad  = pc->pads_base + bank->pads_offset +
				    j * sizeof(u32);
		}

		raw_spin_lock_init(&pc->irq_lock[i]);
	}

	girq = &pc->gpio_chip.irq;
	girq->chip = &rp1_gpio_irq_chip;
	girq->parent_handler = rp1_gpio_irq_handler;
	girq->num_parents = RP1_NUM_BANKS;
	girq->parents = pc->irq;
	girq->default_type = IRQ_TYPE_NONE;
	girq->handler = handle_level_irq;

	/*
	 * Use the same handler for all groups: this is necessary
	 * since we use one gpiochip to cover all lines - the
	 * irq handler then needs to figure out which group and
	 * bank that was firing the IRQ and look up the per-group
	 * and bank data.
	 */
	for (i = 0; i < RP1_NUM_BANKS; i++) {
		pc->irq[i] = irq_of_parse_and_map(np, i);
		if (!pc->irq[i]) {
			girq->num_parents = i;
			break;
		}
	}

	platform_set_drvdata(pdev, pc);

	err = devm_gpiochip_add_data(dev, &pc->gpio_chip, pc);
	if (err) {
		dev_err(dev, "could not add GPIO chip\n");
		return err;
	}

	return 0;
}

static struct platform_driver rp1_pinctrl_driver = {
	.probe = rp1_pinctrl_probe,
	.driver = {
		.name = MODULE_NAME,
		.of_match_table = rp1_pinctrl_match,
		.suppress_bind_attrs = true,
	},
};
builtin_platform_driver(rp1_pinctrl_driver);
