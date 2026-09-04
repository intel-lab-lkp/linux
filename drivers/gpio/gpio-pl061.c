// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2008, 2009 Provigent Ltd.
 *
 * Author: Baruch Siach <baruch@tkos.co.il>
 *
 * Driver for the ARM PrimeCell(tm) General Purpose Input/Output (PL061)
 *
 * Data sheet: ARM DDI 0190B, September 2000
 */
#include <linux/amba/bus.h>
#include <linux/bitops.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/gpio/driver.h>
#include <linux/gpio/regmap.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/irq.h>
#include <linux/irqchip/chained_irq.h>
#include <linux/module.h>
#include <linux/pinctrl/consumer.h>
#include <linux/pm.h>
#include <linux/regmap.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/spinlock.h>

#define PL061_REG_NONE		((unsigned int)-1)

#define PL061_GPIO_NR		8
#define AMBARELLA_GPIO_NR	32

struct pl061_variant_data {
	unsigned int data;
	unsigned int dir;
	unsigned int is;
	unsigned int ibe;
	unsigned int iev;
	unsigned int ie;
	unsigned int ris;
	unsigned int mis;
	unsigned int ic;
	unsigned int mask;
	unsigned int enable;
	unsigned int ngpio;
	bool access_32bit;
	bool masked_data_address;
	bool write_data_after_dir;
	bool clear_irq_on_type;
	bool pm_save_restore;
	const struct regmap_config *regmap_config;
};

struct pl061_context_save_regs {
	u8 gpio_data;
	u8 gpio_dir;
	u8 gpio_is;
	u8 gpio_ibe;
	u8 gpio_iev;
	u8 gpio_ie;
};

struct pl061 {
	raw_spinlock_t		lock;
	void __iomem		*base;
	const struct pl061_variant_data *variant;
	struct gpio_irq_chip	girq;
	int			parent_irq;

	struct pl061_context_save_regs csave_regs;
};

static struct pl061 *pl061_from_gpio_chip(struct gpio_chip *gc)
{
	return gpio_regmap_get_drvdata(gpiochip_get_data(gc));
}

static u32 pl061_read(struct pl061 *pl061, unsigned int reg)
{
	if (pl061->variant->access_32bit)
		return readl(pl061->base + reg);

	return readb(pl061->base + reg);
}

static void pl061_write(struct pl061 *pl061, u32 value, unsigned int reg)
{
	if (pl061->variant->access_32bit)
		writel(value, pl061->base + reg);
	else
		writeb(value, pl061->base + reg);
}

static int pl061_reg_mask_xlate(struct gpio_regmap *gpio, unsigned int base,
				unsigned int offset, unsigned int *reg,
				unsigned int *mask)
{
	struct pl061 *pl061 = gpio_regmap_get_drvdata(gpio);

	/* ARM PL061 data bits use masked addresses: bits [9:2] select pins. */
	if (base == pl061->variant->data)
		*reg = BIT(offset + 2);
	else
		*reg = base;
	*mask = BIT(offset);

	return 0;
}

static int pl061_irq_type(struct irq_data *d, unsigned trigger)
{
	struct gpio_chip *gc = irq_data_get_irq_chip_data(d);
	struct pl061 *pl061 = pl061_from_gpio_chip(gc);
	const struct pl061_variant_data *variant = pl061->variant;
	int offset = irqd_to_hwirq(d);
	unsigned long flags;
	u32 gpiois, gpioibe, gpioiev;
	u32 bit = BIT(offset);

	if (offset < 0 || offset >= (int)gc->ngpio)
		return -EINVAL;

	if ((trigger & IRQ_TYPE_LEVEL_MASK) && (trigger & IRQ_TYPE_EDGE_BOTH)) {
		dev_err(gc->parent,
			"trying to configure line %d for both level and edge "
			"detection, choose one!\n",
			offset);
		return -EINVAL;
	}

	raw_spin_lock_irqsave(&pl061->lock, flags);

	gpioiev = pl061_read(pl061, variant->iev);
	gpiois = pl061_read(pl061, variant->is);
	gpioibe = pl061_read(pl061, variant->ibe);

	if (trigger & IRQ_TYPE_LEVEL_MASK) {
		bool polarity = trigger & IRQ_TYPE_LEVEL_HIGH;

		/* Disable edge detection */
		gpioibe &= ~bit;
		/* Enable level detection */
		gpiois |= bit;
		/* Select polarity */
		if (polarity)
			gpioiev |= bit;
		else
			gpioiev &= ~bit;
		irq_set_handler_locked(d, handle_level_irq);
		dev_dbg(gc->parent, "line %d: IRQ on %s level\n",
			offset,
			polarity ? "HIGH" : "LOW");
	} else if ((trigger & IRQ_TYPE_EDGE_BOTH) == IRQ_TYPE_EDGE_BOTH) {
		/* Disable level detection */
		gpiois &= ~bit;
		/* Select both edges, setting this makes GPIOEV be ignored */
		gpioibe |= bit;
		irq_set_handler_locked(d, handle_edge_irq);
		dev_dbg(gc->parent, "line %d: IRQ on both edges\n", offset);
	} else if ((trigger & IRQ_TYPE_EDGE_RISING) ||
		   (trigger & IRQ_TYPE_EDGE_FALLING)) {
		bool rising = trigger & IRQ_TYPE_EDGE_RISING;

		/* Disable level detection */
		gpiois &= ~bit;
		/* Clear detection on both edges */
		gpioibe &= ~bit;
		/* Select edge */
		if (rising)
			gpioiev |= bit;
		else
			gpioiev &= ~bit;
		irq_set_handler_locked(d, handle_edge_irq);
		dev_dbg(gc->parent, "line %d: IRQ on %s edge\n",
			offset,
			rising ? "RISING" : "FALLING");
	} else {
		/* No trigger: disable everything */
		gpiois &= ~bit;
		gpioibe &= ~bit;
		gpioiev &= ~bit;
		irq_set_handler_locked(d, handle_bad_irq);
		dev_warn(gc->parent, "no trigger selected for line %d\n",
			 offset);
	}

	pl061_write(pl061, gpiois, variant->is);
	pl061_write(pl061, gpioibe, variant->ibe);
	pl061_write(pl061, gpioiev, variant->iev);
	if (variant->clear_irq_on_type)
		pl061_write(pl061, bit, variant->ic);

	raw_spin_unlock_irqrestore(&pl061->lock, flags);

	return 0;
}

static void pl061_irq_handler(struct irq_desc *desc)
{
	unsigned long pending;
	int offset;
	struct gpio_chip *gc = irq_desc_get_handler_data(desc);
	struct pl061 *pl061 = pl061_from_gpio_chip(gc);
	struct irq_chip *irqchip = irq_desc_get_chip(desc);

	chained_irq_enter(irqchip, desc);

	pending = pl061_read(pl061, pl061->variant->mis);
	if (pending) {
		for_each_set_bit(offset, &pending, gc->ngpio)
			generic_handle_domain_irq(gc->irq.domain, offset);
	}

	chained_irq_exit(irqchip, desc);
}

static void pl061_irq_mask(struct irq_data *d)
{
	struct gpio_chip *gc = irq_data_get_irq_chip_data(d);
	struct pl061 *pl061 = pl061_from_gpio_chip(gc);
	u32 mask = BIT(irqd_to_hwirq(d) % gc->ngpio);
	u32 gpioie;

	raw_spin_lock(&pl061->lock);
	gpioie = pl061_read(pl061, pl061->variant->ie) & ~mask;
	pl061_write(pl061, gpioie, pl061->variant->ie);
	raw_spin_unlock(&pl061->lock);

	gpiochip_disable_irq(gc, d->hwirq);
}

static void pl061_irq_unmask(struct irq_data *d)
{
	struct gpio_chip *gc = irq_data_get_irq_chip_data(d);
	struct pl061 *pl061 = pl061_from_gpio_chip(gc);
	u32 mask = BIT(irqd_to_hwirq(d) % gc->ngpio);
	u32 gpioie;

	gpiochip_enable_irq(gc, d->hwirq);

	raw_spin_lock(&pl061->lock);
	gpioie = pl061_read(pl061, pl061->variant->ie) | mask;
	pl061_write(pl061, gpioie, pl061->variant->ie);
	raw_spin_unlock(&pl061->lock);
}

/**
 * pl061_irq_ack() - ACK an edge IRQ
 * @d: IRQ data for this IRQ
 *
 * This gets called from the edge IRQ handler to ACK the edge IRQ
 * in the GPIOIC (interrupt-clear) register. For level IRQs this is
 * not needed: these go away when the level signal goes away.
 */
static void pl061_irq_ack(struct irq_data *d)
{
	struct gpio_chip *gc = irq_data_get_irq_chip_data(d);
	struct pl061 *pl061 = pl061_from_gpio_chip(gc);
	u32 mask = BIT(irqd_to_hwirq(d) % gc->ngpio);

	raw_spin_lock(&pl061->lock);
	pl061_write(pl061, mask, pl061->variant->ic);
	raw_spin_unlock(&pl061->lock);
}

static int pl061_irq_set_wake(struct irq_data *d, unsigned int state)
{
	struct gpio_chip *gc = irq_data_get_irq_chip_data(d);
	struct pl061 *pl061 = pl061_from_gpio_chip(gc);

	return irq_set_irq_wake(pl061->parent_irq, state);
}

static void pl061_irq_print_chip(struct irq_data *data, struct seq_file *p)
{
	struct gpio_chip *gc = irq_data_get_irq_chip_data(data);

	seq_puts(p, dev_name(gc->parent));
}

static const struct irq_chip pl061_irq_chip = {
	.irq_ack		= pl061_irq_ack,
	.irq_mask		= pl061_irq_mask,
	.irq_unmask		= pl061_irq_unmask,
	.irq_set_type		= pl061_irq_type,
	.irq_set_wake		= pl061_irq_set_wake,
	.irq_print_chip		= pl061_irq_print_chip,
	.flags			= IRQCHIP_IMMUTABLE,
	GPIOCHIP_IRQ_RESOURCE_HELPERS,
};

static int pl061_probe(struct amba_device *adev, const struct amba_id *id)
{
	struct device *dev = &adev->dev;
	struct gpio_regmap_config config = {};
	const struct pl061_variant_data *variant = id->data;
	struct pl061 *pl061;
	struct gpio_irq_chip *girq;
	struct regmap *regmap;
	int ret, irq;

	pl061 = devm_kzalloc(dev, sizeof(*pl061), GFP_KERNEL);
	if (pl061 == NULL)
		return -ENOMEM;

	pl061->variant = variant;

	pl061->base = devm_ioremap_resource(dev, &adev->res);
	if (IS_ERR(pl061->base))
		return PTR_ERR(pl061->base);

	raw_spin_lock_init(&pl061->lock);

	regmap = devm_regmap_init_mmio(dev, pl061->base, variant->regmap_config);
	if (IS_ERR(regmap))
		return PTR_ERR(regmap);

	pl061_write(pl061, 0, variant->ie); /* disable irqs */
	if (variant->mask != PL061_REG_NONE)
		pl061_write(pl061, GENMASK(variant->ngpio - 1, 0), variant->mask);
	if (variant->enable != PL061_REG_NONE)
		pl061_write(pl061, GENMASK(variant->ngpio - 1, 0), variant->enable);

	irq = adev->irq[0];
	if (!irq)
		dev_warn(&adev->dev, "IRQ support disabled\n");
	pl061->parent_irq = irq;

	girq = &pl061->girq;
	gpio_irq_chip_set_chip(girq, &pl061_irq_chip);
	girq->parent_handler = pl061_irq_handler;
	girq->num_parents = 1;
	girq->parents = devm_kcalloc(dev, 1, sizeof(*girq->parents),
				     GFP_KERNEL);
	if (!girq->parents)
		return -ENOMEM;
	girq->parents[0] = irq;
	girq->default_type = IRQ_TYPE_NONE;
	girq->handler = handle_bad_irq;

	config.parent = dev;
	config.regmap = regmap;
	config.ngpio = variant->ngpio;
	config.reg_dat_base = GPIO_REGMAP_ADDR(variant->data);
	config.reg_set_base = GPIO_REGMAP_ADDR(variant->data);
	config.reg_dir_out_base = variant->dir;
	if (variant->masked_data_address)
		config.reg_mask_xlate = pl061_reg_mask_xlate;
	config.write_data_after_dir = variant->write_data_after_dir;
	config.girq = girq;
	config.drvdata = pl061;

	ret = PTR_ERR_OR_ZERO(devm_gpio_regmap_register(dev, &config));
	if (ret)
		return ret;

	amba_set_drvdata(adev, pl061);
	dev_info(dev, "PL061 GPIO chip registered\n");

	return 0;
}

static int pl061_suspend(struct device *dev)
{
	struct pl061 *pl061 = dev_get_drvdata(dev);
	const struct pl061_variant_data *variant = pl061->variant;
	int offset;

	if (!variant->pm_save_restore)
		return 0;

	pl061->csave_regs.gpio_data = 0;
	pl061->csave_regs.gpio_dir = pl061_read(pl061, variant->dir);
	pl061->csave_regs.gpio_is = pl061_read(pl061, variant->is);
	pl061->csave_regs.gpio_ibe = pl061_read(pl061, variant->ibe);
	pl061->csave_regs.gpio_iev = pl061_read(pl061, variant->iev);
	pl061->csave_regs.gpio_ie = pl061_read(pl061, variant->ie);

	for (offset = 0; offset < variant->ngpio; offset++) {
		if (pl061->csave_regs.gpio_dir & BIT(offset))
			pl061->csave_regs.gpio_data |=
				pl061_read(pl061, BIT(offset + 2));
	}

	return 0;
}

static int pl061_resume(struct device *dev)
{
	struct pl061 *pl061 = dev_get_drvdata(dev);
	const struct pl061_variant_data *variant = pl061->variant;
	int offset;

	if (!variant->pm_save_restore)
		return 0;

	for (offset = 0; offset < variant->ngpio; offset++) {
		u32 dir = pl061_read(pl061, variant->dir);

		if (pl061->csave_regs.gpio_dir & BIT(offset)) {
			u32 value = !!(pl061->csave_regs.gpio_data & BIT(offset)) << offset;

			pl061_write(pl061, value, BIT(offset + 2));
			pl061_write(pl061, dir | BIT(offset), variant->dir);
			/*
			 * gpio value is set again, because pl061 doesn't allow to set value of
			 * a gpio pin before configuring it in OUT mode.
			 */
			pl061_write(pl061, value, BIT(offset + 2));
		} else {
			pl061_write(pl061, dir & ~BIT(offset), variant->dir);
		}
	}

	pl061_write(pl061, pl061->csave_regs.gpio_is, variant->is);
	pl061_write(pl061, pl061->csave_regs.gpio_ibe, variant->ibe);
	pl061_write(pl061, pl061->csave_regs.gpio_iev, variant->iev);
	pl061_write(pl061, pl061->csave_regs.gpio_ie, variant->ie);

	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(pl061_dev_pm_ops, pl061_suspend, pl061_resume);

static const struct regmap_config pl061_arm_regmap_config = {
	.reg_bits = 16,
	.val_bits = 8,
	.reg_stride = 4,
	.max_register = 0x400,
	.fast_io = true,
};

static const struct regmap_config pl061_ambarella_regmap_config = {
	.reg_bits = 32,
	.val_bits = 32,
	.reg_stride = 4,
	.max_register = 0x2c,
	.fast_io = true,
};

static struct pl061_variant_data pl061_arm = {
	.data = 0x000,
	.dir = 0x400,
	.is = 0x404,
	.ibe = 0x408,
	.iev = 0x40c,
	.ie = 0x410,
	.ris = 0x414,
	.mis = 0x418,
	.ic = 0x41c,
	.mask = PL061_REG_NONE,
	.enable = PL061_REG_NONE,
	.ngpio = PL061_GPIO_NR,
	.masked_data_address = true,
	.write_data_after_dir = true,
	.pm_save_restore = true,
	.regmap_config = &pl061_arm_regmap_config,
};

static struct pl061_variant_data pl061_ambarella = {
	.data = 0x00,
	.dir = 0x04,
	.is = 0x08,
	.ibe = 0x0c,
	.iev = 0x10,
	.ie = 0x14,
	.ris = 0x1c,
	.mis = 0x20,
	.ic = 0x24,
	.mask = 0x28,
	.enable = 0x2c,
	.ngpio = AMBARELLA_GPIO_NR,
	.access_32bit = true,
	.clear_irq_on_type = true,
	.regmap_config = &pl061_ambarella_regmap_config,
};

static const struct amba_id pl061_ids[] = {
	{
		.id	= 0x00041061,
		.mask	= 0x000fffff,
		.data	= &pl061_arm,
	},
	{
		.id	= (AMBA_VENDOR_AMBARELLA << 12) | 0x061,
		.mask	= 0x000fffff,
		.data	= &pl061_ambarella,
	},
	{ 0, 0 },
};
MODULE_DEVICE_TABLE(amba, pl061_ids);

static struct amba_driver pl061_gpio_driver = {
	.drv = {
		.name	= "pl061_gpio",
		.pm	= pm_sleep_ptr(&pl061_dev_pm_ops),
	},
	.id_table	= pl061_ids,
	.probe		= pl061_probe,
};
module_amba_driver(pl061_gpio_driver);

MODULE_DESCRIPTION("Driver for the ARM PrimeCell(tm) General Purpose Input/Output (PL061)");
MODULE_LICENSE("GPL v2");
