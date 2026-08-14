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
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/irq.h>
#include <linux/irqchip/chained_irq.h>
#include <linux/module.h>
#include <linux/pinctrl/consumer.h>
#include <linux/pm.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/spinlock.h>

#define PL061_GPIO_NR	8
#define PL061_REG_NONE	U32_MAX
#define PL061_AMBARELLA_PERIPH_ID	0x00000061

struct pl061_variant_data {
	u32 data;
	u32 dir;
	u32 is;
	u32 ibe;
	u32 iev;
	u32 ie;
	u32 ris;
	u32 mis;
	u32 ic;
	u32 afsel;
	u32 mask;
	u32 enable;
	unsigned int ngpio;
	bool access_32bit;
	bool masked_data_address;
	bool write_data_after_dir;
	bool clear_irq_on_type;
};

struct pl061_context_save_regs {
	u32 gpio_data;
	u32 gpio_dir;
	u32 gpio_is;
	u32 gpio_ibe;
	u32 gpio_iev;
	u32 gpio_ie;
	u32 gpio_afsel;
	u32 gpio_mask;
};

struct pl061 {
	raw_spinlock_t		lock;

	void __iomem		*base;
	const struct pl061_variant_data *variant;
	struct gpio_chip	gc;
	int			parent_irq;

	struct pl061_context_save_regs csave_regs;
};

static u32 pl061_read(struct pl061 *pl061, u32 offset)
{
	if (pl061->variant->access_32bit)
		return readl_relaxed(pl061->base + offset);

	return readb_relaxed(pl061->base + offset);
}

static void pl061_write(struct pl061 *pl061, u32 value, u32 offset)
{
	if (pl061->variant->access_32bit)
		writel_relaxed(value, pl061->base + offset);
	else
		writeb_relaxed(value, pl061->base + offset);
}

static int pl061_get_data(struct pl061 *pl061, unsigned int offset)
{
	if (pl061->variant->masked_data_address)
		return !!readb_relaxed(pl061->base + BIT(offset + 2));

	pl061_write(pl061, BIT(offset), pl061->variant->mask);
	return !!(pl061_read(pl061, pl061->variant->data) & BIT(offset));
}

static void pl061_set_data(struct pl061 *pl061, unsigned int offset, int value)
{
	if (pl061->variant->masked_data_address) {
		writeb_relaxed(!!value << offset,
			       pl061->base + BIT(offset + 2));
		return;
	}

	pl061_write(pl061, BIT(offset), pl061->variant->mask);
	pl061_write(pl061, value ? BIT(offset) : 0, pl061->variant->data);
}

static int pl061_get_direction(struct gpio_chip *gc, unsigned offset)
{
	struct pl061 *pl061 = gpiochip_get_data(gc);

	if (pl061_read(pl061, pl061->variant->dir) & BIT(offset))
		return GPIO_LINE_DIRECTION_OUT;

	return GPIO_LINE_DIRECTION_IN;
}

static int pl061_direction_input(struct gpio_chip *gc, unsigned offset)
{
	struct pl061 *pl061 = gpiochip_get_data(gc);
	unsigned long flags;
	u32 gpiodir;

	raw_spin_lock_irqsave(&pl061->lock, flags);
	gpiodir = pl061_read(pl061, pl061->variant->dir);
	gpiodir &= ~(BIT(offset));
	pl061_write(pl061, gpiodir, pl061->variant->dir);
	raw_spin_unlock_irqrestore(&pl061->lock, flags);

	return 0;
}

static int pl061_direction_output(struct gpio_chip *gc, unsigned offset,
		int value)
{
	struct pl061 *pl061 = gpiochip_get_data(gc);
	unsigned long flags;
	u32 gpiodir;

	raw_spin_lock_irqsave(&pl061->lock, flags);
	pl061_set_data(pl061, offset, value);
	gpiodir = pl061_read(pl061, pl061->variant->dir);
	gpiodir |= BIT(offset);
	pl061_write(pl061, gpiodir, pl061->variant->dir);

	/*
	 * gpio value is set again, because pl061 doesn't allow to set value of
	 * a gpio pin before configuring it in OUT mode.
	 */
	if (pl061->variant->write_data_after_dir)
		pl061_set_data(pl061, offset, value);
	raw_spin_unlock_irqrestore(&pl061->lock, flags);

	return 0;
}

static int pl061_get_value(struct gpio_chip *gc, unsigned offset)
{
	struct pl061 *pl061 = gpiochip_get_data(gc);
	unsigned long flags;
	int value;

	raw_spin_lock_irqsave(&pl061->lock, flags);
	value = pl061_get_data(pl061, offset);
	raw_spin_unlock_irqrestore(&pl061->lock, flags);

	return value;
}

static int pl061_set_value(struct gpio_chip *gc, unsigned int offset, int value)
{
	struct pl061 *pl061 = gpiochip_get_data(gc);
	unsigned long flags;

	raw_spin_lock_irqsave(&pl061->lock, flags);
	pl061_set_data(pl061, offset, value);
	raw_spin_unlock_irqrestore(&pl061->lock, flags);

	return 0;
}

static int pl061_irq_type(struct irq_data *d, unsigned trigger)
{
	struct gpio_chip *gc = irq_data_get_irq_chip_data(d);
	struct pl061 *pl061 = gpiochip_get_data(gc);
	int offset = irqd_to_hwirq(d);
	unsigned long flags;
	u32 gpiois, gpioibe, gpioiev;
	u32 bit = BIT(offset);

	if (offset < 0 || offset >= gc->ngpio)
		return -EINVAL;

	if ((trigger & (IRQ_TYPE_LEVEL_HIGH | IRQ_TYPE_LEVEL_LOW)) &&
	    (trigger & (IRQ_TYPE_EDGE_RISING | IRQ_TYPE_EDGE_FALLING))) {
		dev_err(gc->parent,
			"trying to configure line %d for both level and edge "
			"detection, choose one!\n",
			offset);
		return -EINVAL;
	}

	raw_spin_lock_irqsave(&pl061->lock, flags);

	gpioiev = pl061_read(pl061, pl061->variant->iev);
	gpiois = pl061_read(pl061, pl061->variant->is);
	gpioibe = pl061_read(pl061, pl061->variant->ibe);

	if (trigger & (IRQ_TYPE_LEVEL_HIGH | IRQ_TYPE_LEVEL_LOW)) {
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

	pl061_write(pl061, gpiois, pl061->variant->is);
	pl061_write(pl061, gpioibe, pl061->variant->ibe);
	pl061_write(pl061, gpioiev, pl061->variant->iev);
	if (pl061->variant->clear_irq_on_type)
		pl061_write(pl061, bit, pl061->variant->ic);

	raw_spin_unlock_irqrestore(&pl061->lock, flags);

	return 0;
}

static void pl061_irq_handler(struct irq_desc *desc)
{
	unsigned long pending;
	int offset;
	struct gpio_chip *gc = irq_desc_get_handler_data(desc);
	struct pl061 *pl061 = gpiochip_get_data(gc);
	struct irq_chip *irqchip = irq_desc_get_chip(desc);

	chained_irq_enter(irqchip, desc);

	pending = pl061_read(pl061, pl061->variant->mis);
	if (pending) {
		for_each_set_bit(offset, &pending, gc->ngpio)
			generic_handle_domain_irq(gc->irq.domain,
						  offset);
	}

	chained_irq_exit(irqchip, desc);
}

static void pl061_irq_mask(struct irq_data *d)
{
	struct gpio_chip *gc = irq_data_get_irq_chip_data(d);
	struct pl061 *pl061 = gpiochip_get_data(gc);
	u32 mask = BIT(irqd_to_hwirq(d));
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
	struct pl061 *pl061 = gpiochip_get_data(gc);
	u32 mask = BIT(irqd_to_hwirq(d));
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
	struct pl061 *pl061 = gpiochip_get_data(gc);
	u32 mask = BIT(irqd_to_hwirq(d));

	raw_spin_lock(&pl061->lock);
	pl061_write(pl061, mask, pl061->variant->ic);
	raw_spin_unlock(&pl061->lock);
}

static int pl061_irq_set_wake(struct irq_data *d, unsigned int state)
{
	struct gpio_chip *gc = irq_data_get_irq_chip_data(d);
	struct pl061 *pl061 = gpiochip_get_data(gc);

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
	struct pl061 *pl061;
	struct gpio_irq_chip *girq;
	int ret, irq;

	pl061 = devm_kzalloc(dev, sizeof(*pl061), GFP_KERNEL);
	if (pl061 == NULL)
		return -ENOMEM;

	pl061->variant = id->data;
	if (!pl061->variant)
		return dev_err_probe(dev, -EINVAL, "missing variant data\n");

	pl061->base = devm_ioremap_resource(dev, &adev->res);
	if (IS_ERR(pl061->base))
		return PTR_ERR(pl061->base);

	raw_spin_lock_init(&pl061->lock);
	pl061->gc.request = gpiochip_generic_request;
	pl061->gc.free = gpiochip_generic_free;
	pl061->gc.base = -1;
	pl061->gc.get_direction = pl061_get_direction;
	pl061->gc.direction_input = pl061_direction_input;
	pl061->gc.direction_output = pl061_direction_output;
	pl061->gc.get = pl061_get_value;
	pl061->gc.set = pl061_set_value;
	pl061->gc.ngpio = pl061->variant->ngpio;
	pl061->gc.label = dev_name(dev);
	pl061->gc.parent = dev;
	pl061->gc.owner = THIS_MODULE;

	/*
	 * irq_chip support
	 */
	pl061_write(pl061, 0, pl061->variant->ie); /* disable irqs */
	if (pl061->variant->enable != PL061_REG_NONE)
		pl061_write(pl061, GENMASK(pl061->gc.ngpio - 1, 0),
			    pl061->variant->enable);
	if (pl061->variant->afsel != PL061_REG_NONE)
		pl061_write(pl061, 0, pl061->variant->afsel);
	if (pl061->variant->mask != PL061_REG_NONE)
		pl061_write(pl061, 0, pl061->variant->mask);
	irq = adev->irq[0];
	if (!irq)
		dev_warn(&adev->dev, "IRQ support disabled\n");
	pl061->parent_irq = irq;

	girq = &pl061->gc.irq;
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

	ret = devm_gpiochip_add_data(dev, &pl061->gc, pl061);
	if (ret)
		return ret;

	amba_set_drvdata(adev, pl061);
	dev_info(dev, "PL061 GPIO chip registered\n");

	return 0;
}

static int pl061_suspend(struct device *dev)
{
	struct pl061 *pl061 = dev_get_drvdata(dev);
	unsigned long flags;
	int offset;

	raw_spin_lock_irqsave(&pl061->lock, flags);
	pl061->csave_regs.gpio_data = 0;
	pl061->csave_regs.gpio_dir =
		pl061_read(pl061, pl061->variant->dir);
	pl061->csave_regs.gpio_is =
		pl061_read(pl061, pl061->variant->is);
	pl061->csave_regs.gpio_ibe =
		pl061_read(pl061, pl061->variant->ibe);
	pl061->csave_regs.gpio_iev =
		pl061_read(pl061, pl061->variant->iev);
	pl061->csave_regs.gpio_ie =
		pl061_read(pl061, pl061->variant->ie);
	if (pl061->variant->afsel != PL061_REG_NONE)
		pl061->csave_regs.gpio_afsel =
			pl061_read(pl061, pl061->variant->afsel);
	if (pl061->variant->mask != PL061_REG_NONE)
		pl061->csave_regs.gpio_mask =
			pl061_read(pl061, pl061->variant->mask);

	for (offset = 0; offset < pl061->gc.ngpio; offset++) {
		if ((pl061->csave_regs.gpio_dir & BIT(offset)) &&
		    pl061_get_data(pl061, offset))
			pl061->csave_regs.gpio_data |= BIT(offset);
	}
	if (pl061->variant->mask != PL061_REG_NONE)
		pl061_write(pl061, pl061->csave_regs.gpio_mask,
			    pl061->variant->mask);
	raw_spin_unlock_irqrestore(&pl061->lock, flags);

	return 0;
}

static int pl061_resume(struct device *dev)
{
	struct pl061 *pl061 = dev_get_drvdata(dev);
	int offset;

	for (offset = 0; offset < pl061->gc.ngpio; offset++) {
		if (pl061->csave_regs.gpio_dir & (BIT(offset)))
			pl061_direction_output(&pl061->gc, offset,
					pl061->csave_regs.gpio_data &
					(BIT(offset)));
		else
			pl061_direction_input(&pl061->gc, offset);
	}

	pl061_write(pl061, pl061->csave_regs.gpio_is,
		    pl061->variant->is);
	pl061_write(pl061, pl061->csave_regs.gpio_ibe,
		    pl061->variant->ibe);
	pl061_write(pl061, pl061->csave_regs.gpio_iev,
		    pl061->variant->iev);
	pl061_write(pl061, pl061->csave_regs.gpio_ie,
		    pl061->variant->ie);
	if (pl061->variant->afsel != PL061_REG_NONE)
		pl061_write(pl061, pl061->csave_regs.gpio_afsel,
			    pl061->variant->afsel);
	if (pl061->variant->mask != PL061_REG_NONE)
		pl061_write(pl061, pl061->csave_regs.gpio_mask,
			    pl061->variant->mask);
	if (pl061->variant->enable != PL061_REG_NONE)
		pl061_write(pl061, GENMASK(pl061->gc.ngpio - 1, 0),
			    pl061->variant->enable);

	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(pl061_dev_pm_ops, pl061_suspend, pl061_resume);

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
	.afsel = PL061_REG_NONE,
	.mask = PL061_REG_NONE,
	.enable = PL061_REG_NONE,
	.ngpio = PL061_GPIO_NR,
	.masked_data_address = true,
	.write_data_after_dir = true,
};

static struct pl061_variant_data pl061_ambarella = {
	.data = 0x00,
	.dir = 0x04,
	.is = 0x08,
	.ibe = 0x0c,
	.iev = 0x10,
	.ie = 0x14,
	.afsel = 0x18,
	.ris = 0x1c,
	.mis = 0x20,
	.ic = 0x24,
	.mask = 0x28,
	.enable = 0x2c,
	.ngpio = 32,
	.access_32bit = true,
	.clear_irq_on_type = true,
};

static const struct amba_id pl061_ids[] = {
	{
		.id	= 0x00041061,
		.mask	= 0x000fffff,
		.data	= &pl061_arm,
	},
	{
		.id	= PL061_AMBARELLA_PERIPH_ID,
		.mask	= 0xffffffff,
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
