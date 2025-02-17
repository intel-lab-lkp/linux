// SPDX-License-Identifier: GPL-2.0 OR MIT
/*
 * Copyright (C) 2023-2025 SpacemiT (Hangzhou) Technology Co. Ltd
 * Copyright (C) 2025 Yixun Lan <dlan@gentoo.org>
 */

#include <linux/io.h>
#include <linux/init.h>
#include <linux/irq.h>
#include <linux/interrupt.h>
#include <linux/gpio/driver.h>
#include <linux/platform_device.h>
#include <linux/pinctrl/pinctrl.h>
#include <linux/property.h>
#include <linux/seq_file.h>
#include <linux/module.h>

#include "gpiolib.h"

/* register offset */
/* GPIO port level register */
#define GPLR		0x00
/* GPIO port direction register - R/W */
#define GPDR		0x0c
/* GPIO port set register - W */
#define GPSR		0x18
/* GPIO port clear register - W */
#define GPCR		0x24
/* GPIO port rising edge register R/W */
#define GRER		0x30
/* GPIO port falling edge register R/W */
#define GFER		0x3c
/* GPIO edge detect status register - R/W1C */
#define GEDR		0x48
/*  GPIO (set) direction register - W */
#define GSDR		0x54
/* GPIO (clear) direction register - W */
#define GCDR		0x60
/* GPIO (set) rising edge detect enable register - W */
#define GSRER		0x6c
/* GPIO (clear) rising edge detect enable register - W */
#define GCRER		0x78
/* GPIO (set) falling edge detect enable register - W */
#define GSFER		0x84
/* GPIO (clear) falling edge detect enable register - W */
#define GCFER		0x90
/* GPIO interrupt mask register, 0 disable, 1 enable - R/W */
#define GAPMASK		0x9c

#define NR_BANKS		4
#define NR_GPIOS_PER_BANK	32

#define to_spacemit_gpio_bank(x) container_of((x), struct spacemit_gpio_bank, gc)

struct spacemit_gpio;

struct spacemit_gpio_bank {
	struct gpio_chip		gc;
	struct spacemit_gpio		*sg;
	void __iomem			*base;
	u32				index;
	u32				irq_mask;
	u32				irq_rising_edge;
	u32				irq_falling_edge;
};

struct spacemit_gpio {
	struct	device			*dev;
	struct	spacemit_gpio_bank	sgb[NR_BANKS];
};

static irqreturn_t spacemit_gpio_irq_handler(int irq, void *dev_id)
{
	struct spacemit_gpio_bank *gb = dev_id;
	unsigned long pending;
	u32 n, gedr;

	gedr = readl(gb->base + GEDR);
	if (!gedr)
		return IRQ_NONE;
	writel(gedr, gb->base + GEDR);

	gedr = gedr & gb->irq_mask;
	if (!gedr)
		return IRQ_NONE;

	pending = gedr;
	for_each_set_bit(n, &pending, BITS_PER_LONG)
		handle_nested_irq(irq_find_mapping(gb->gc.irq.domain, n));

	return IRQ_HANDLED;
}

static void spacemit_gpio_irq_ack(struct irq_data *d)
{
	struct spacemit_gpio_bank *gb = irq_data_get_irq_chip_data(d);

	writel(BIT(irqd_to_hwirq(d)), gb->base + GEDR);
}

static void spacemit_gpio_irq_mask(struct irq_data *d)
{
	struct spacemit_gpio_bank *gb = irq_data_get_irq_chip_data(d);
	u32 bit = BIT(irqd_to_hwirq(d));

	gb->irq_mask &= ~bit;

	if (bit & gb->irq_rising_edge)
		writel(bit, gb->base + GCRER);

	if (bit & gb->irq_falling_edge)
		writel(bit, gb->base + GCFER);
}

static void spacemit_gpio_irq_unmask(struct irq_data *d)
{
	struct spacemit_gpio_bank *gb = irq_data_get_irq_chip_data(d);
	u32 bit = BIT(irqd_to_hwirq(d));

	gb->irq_mask |= bit;

	if (bit & gb->irq_rising_edge)
		writel(bit,  gb->base + GSRER);

	if (bit & gb->irq_falling_edge)
		writel(bit, gb->base + GSFER);
}

static int spacemit_gpio_irq_set_type(struct irq_data *d, unsigned int type)
{
	struct spacemit_gpio_bank *gb = irq_data_get_irq_chip_data(d);
	u32 bit = BIT(irqd_to_hwirq(d));

	if (type & IRQ_TYPE_EDGE_RISING) {
		gb->irq_rising_edge |= bit;
		writel(bit, gb->base + GSRER);
	} else {
		gb->irq_rising_edge &= ~bit;
		writel(bit, gb->base + GCRER);
	}

	if (type & IRQ_TYPE_EDGE_FALLING) {
		gb->irq_falling_edge |= bit;
		writel(bit, gb->base + GSFER);
	} else {
		gb->irq_falling_edge &= ~bit;
		writel(bit, gb->base + GCFER);
	}

	return 0;
}

static void spacemit_gpio_irq_print_chip(struct irq_data *data, struct seq_file *p)
{
	struct spacemit_gpio_bank *gb = irq_data_get_irq_chip_data(data);

	seq_printf(p, "%s-%d", dev_name(gb->gc.parent), gb->index);
}

static struct irq_chip spacemit_gpio_chip = {
	.name		= "k1-gpio-irqchip",
	.irq_ack	= spacemit_gpio_irq_ack,
	.irq_mask	= spacemit_gpio_irq_mask,
	.irq_unmask	= spacemit_gpio_irq_unmask,
	.irq_set_type	= spacemit_gpio_irq_set_type,
	.irq_print_chip	= spacemit_gpio_irq_print_chip,
	.flags		= IRQCHIP_IMMUTABLE,
	GPIOCHIP_IRQ_RESOURCE_HELPERS,
};

static int spacemit_gpio_xlate(struct gpio_chip *gc,
			       const struct of_phandle_args *gpiospec, u32 *flags)
{
	struct spacemit_gpio_bank *gb = gpiochip_get_data(gc);
	struct spacemit_gpio *sg = gb->sg;

	int i;

	if (gc->of_gpio_n_cells != 3)
		return -EINVAL;

	if (gpiospec->args_count < gc->of_gpio_n_cells)
		return -EINVAL;

	i = gpiospec->args[0];
	if (i >= NR_BANKS)
		return -EINVAL;

	if (gc != &sg->sgb[i].gc)
		return -EINVAL;

	if (gpiospec->args[1] >= gc->ngpio)
		return -EINVAL;

	if (flags)
		*flags = gpiospec->args[2];

	return gpiospec->args[1];
}

static int spacemit_add_pin_range(struct gpio_chip *gc)
{
	struct spacemit_gpio_bank *gb;
	struct of_phandle_args pinspec;
	struct pinctrl_dev *pctldev;
	struct device_node *np;
	int ret, trim;

	np = dev_of_node(&gc->gpiodev->dev);
	if (!np)
		return 0;

	gb = to_spacemit_gpio_bank(gc);

	ret = of_parse_phandle_with_fixed_args(np, "gpio-ranges", 3,
					       gb->index, &pinspec);
	if (ret)
		return ret;

	pctldev = of_pinctrl_get(pinspec.np);
	of_node_put(pinspec.np);
	if (!pctldev)
		return -EPROBE_DEFER;

	/* Ignore ranges outside of this GPIO chip */
	if (pinspec.args[0] >= (gc->offset + gc->ngpio))
		return -EINVAL;

	if (pinspec.args[0] + pinspec.args[2] <= gc->offset)
		return -EINVAL;

	if (!pinspec.args[2])
		return -EINVAL;

	/* Trim the range to fit this GPIO chip */
	if (gc->offset > pinspec.args[0]) {
		trim = gc->offset - pinspec.args[0];
		pinspec.args[2] -= trim;
		pinspec.args[1] += trim;
		pinspec.args[0] = 0;
	} else {
		pinspec.args[0] -= gc->offset;
	}
	if ((pinspec.args[0] + pinspec.args[2]) > gc->ngpio)
		pinspec.args[2] = gc->ngpio - pinspec.args[0];

	ret = gpiochip_add_pin_range(gc,
				     pinctrl_dev_get_devname(pctldev),
				     pinspec.args[0],
				     pinspec.args[1],
				     pinspec.args[2]);
	if (ret)
		return ret;

	return 0;
}

static int spacemit_gpio_add_bank(struct spacemit_gpio *sg,
				  void __iomem *regs,
				  int index, int irq)
{
	struct spacemit_gpio_bank *gb = &sg->sgb[index];
	struct gpio_chip *gc = &gb->gc;
	struct device *dev = sg->dev;
	struct gpio_irq_chip *girq;
	void __iomem *dat, *set, *clr, *dirin, *dirout;
	int ret, bank_base[] = { 0x0, 0x4, 0x8, 0x100 };

	gb->index = index;
	gb->base = regs + bank_base[index];

	dat	= gb->base + GPLR;
	set	= gb->base + GPSR;
	clr	= gb->base + GPCR;
	dirin	= gb->base + GCDR;
	dirout	= gb->base + GSDR;

	/* This registers 32 GPIO lines per bank */
	ret = bgpio_init(gc, dev, 4, dat, set, clr, dirout, dirin,
			 BGPIOF_UNREADABLE_REG_SET | BGPIOF_UNREADABLE_REG_DIR);
	if (ret)
		return dev_err_probe(dev, ret, "failed to init gpio chip\n");

	gb->sg = sg;

	gc->label		= dev_name(dev);
	gc->request		= gpiochip_generic_request;
	gc->free		= gpiochip_generic_free;
	gc->ngpio		= NR_GPIOS_PER_BANK;
	gc->base		= -1;

#ifdef CONFIG_OF_GPIO
	gc->of_xlate		= spacemit_gpio_xlate;
	gc->of_add_pin_range	= spacemit_add_pin_range;
	gc->of_gpio_n_cells	= 3;
#endif

	girq			= &gc->irq;
	girq->threaded		= true;
	girq->handler		= handle_simple_irq;

	gpio_irq_chip_set_chip(girq, &spacemit_gpio_chip);

	/* Clear Edge Detection Settings */
	writel(0x0, gb->base + GRER);
	writel(0x0, gb->base + GFER);
	/* Clear and Disable Interrupt */
	writel(0xffffffff, gb->base + GCFER);
	writel(0xffffffff, gb->base + GCRER);
	writel(0, gb->base + GAPMASK);

	ret = devm_request_threaded_irq(dev, irq, NULL,
					spacemit_gpio_irq_handler,
					IRQF_ONESHOT | IRQF_SHARED,
					gb->gc.label, gb);
	if (ret < 0)
		return dev_err_probe(dev, ret, "failed to register IRQ\n");

	ret = devm_gpiochip_add_data(dev, gc, gb);
	if (ret < 0)
		return dev_err_probe(dev, ret, "failed to add gpio chip\n");

	/* Eable Interrupt */
	writel(0xffffffff, gb->base + GAPMASK);

	return 0;
}

static int spacemit_gpio_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct spacemit_gpio *sg;
	struct resource *res;
	void __iomem *regs;
	int i, irq, ret;

	sg = devm_kzalloc(dev, sizeof(*sg), GFP_KERNEL);
	if (!sg)
		return -ENOMEM;

	regs = devm_platform_get_and_ioremap_resource(pdev, 0, &res);
	if (IS_ERR(regs))
		return PTR_ERR(regs);

	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;

	sg->dev	= dev;

	for (i = 0; i < NR_BANKS; i++) {
		ret = spacemit_gpio_add_bank(sg, regs, i, irq);
		if (ret)
			return ret;
	}

	return 0;
}

static const struct of_device_id spacemit_gpio_dt_ids[] = {
	{ .compatible = "spacemit,k1-gpio" },
	{ /* sentinel */ }
};

static struct platform_driver spacemit_gpio_driver = {
	.probe		= spacemit_gpio_probe,
	.driver		= {
		.name	= "k1-gpio",
		.of_match_table = spacemit_gpio_dt_ids,
	},
};
module_platform_driver(spacemit_gpio_driver);

MODULE_AUTHOR("Yixun Lan <dlan@gentoo.org>");
MODULE_DESCRIPTION("GPIO driver for SpacemiT K1 SoC");
MODULE_LICENSE("GPL");
