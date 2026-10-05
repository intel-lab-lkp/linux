// SPDX-License-Identifier: GPL-2.0-only
/*
 * gpio-amdptpci.c - AMD Promontory PCI GPIO Controller Driver
 *
 * Copyright (C) 2026 ASMedia Technology Inc.
 *
 * Author:	Chloe Chen <chloe_chen@asmedia.com.tw>
 *		YD Tseng <yd_tseng@asmedia.com.tw>
 *
 */

#include <linux/bits.h>
#include <linux/compiler.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/gpio/driver.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pci.h>
#include <linux/pm.h>
#include <linux/pm_runtime.h>
#include <linux/pm_wakeup.h>
#include <linux/spinlock.h>
#include <linux/types.h>

#define PT27_NUM_GPIOS          24
#define PT27_WAKE_GPIOS         12
#define PT27_GPIO_MASK          GENMASK(PT27_NUM_GPIOS - 1, 0)
#define PT27_WAKE_MASK          GENMASK(PT27_WAKE_GPIOS - 1, 0)

#define PT27_REG_DIR            0x0000
#define PT27_REG_IN             0x0004
#define PT27_REG_OUT            0x0008
#define PT27_REG_INT_EN         0x000C
#define PT27_REG_INT_LVL        0x0010
#define PT27_REG_INT_MODE0      0x0014
#define PT27_REG_INT_MODE1      0x0018
#define PT27_REG_INT_STAT       0x001C
#define PT27_REG_INT_MASK       0x0020
#define PT27_REG_INT_CTL        0x0024

#define PT27_INT_CTL_MODE       BIT(0)
#define PT27_INT_CTL_LVL        BIT(1)
#define PT27_INT_CTL_PAD_EN     BIT(2)

#define PT27_REG_VENDOR0        0x0028
#define PT27_REG_VENDOR1        0x002C

#define PT27_REG_WAKE_EN        0x0034
#define PT27_REG_WAKE_LVL       0x0036
#define PT27_REG_WAKE_MODE      0x0038
#define PT27_REG_WAKE_STAT      0x003A

#define PT27_INT_MODE_MASK	0x3U
#define PT27_MODE_LEVEL         0x0U
#define PT27_MODE_RISING        0x1U
#define PT27_MODE_FALLING       0x2U
#define PT27_MODE_BOTH          0x3U

#define PT27_AUTOSUSPEND_DELAY_MS   2000

struct pt27_saved_regs {
	u32 dir;
	u32 out;
	u32 int_en;
	u32 int_lvl;
	u32 int_mode0;
	u16 int_mode1;
	u32 int_mask;
	u8 int_ctl;
	u32 vendor0;
	u32 vendor1;
	u16 wake_en;
	u16 wake_lvl;
	u16 wake_mode;
};

struct pt27_gpio {
	struct device          *dev;
	struct gpio_chip        gc;
	void __iomem           *base;
	struct pt27_saved_regs  saved;
	bool                    saved_valid;
	raw_spinlock_t          lock;
	struct mutex            irq_bus_lock;
	bool                    irq_bus_active;
	u32                     enabled_irq_mask;
	bool                    irq_pm_held;
	u16                     wake_mask;
	bool                    use_msi;
	int                     irq;
};

static inline u32  pt27_rd32(struct pt27_gpio *c, u32 off)
{
	return ioread32(c->base + off);
}

static inline void pt27_wr32(struct pt27_gpio *c, u32 off, u32 v)
{
	iowrite32(v, c->base + off);
}

static inline u16  pt27_rd16(struct pt27_gpio *c, u32 off)
{
	return ioread16(c->base + off);
}

static inline void pt27_wr16(struct pt27_gpio *c, u32 off, u16 v)
{
	iowrite16(v, c->base + off);
}

static inline u8   pt27_rd8(struct pt27_gpio *c, u32 off)
{
	return ioread8(c->base + off);
}

static inline void pt27_wr8(struct pt27_gpio *c, u32 off, u8 v)
{
	iowrite8(v, c->base + off);
}

static void pt27_runtime_put_autosuspend(struct pt27_gpio *chip)
{
	pm_runtime_mark_last_busy(chip->dev);
	pm_runtime_put_autosuspend(chip->dev);

}

static void pt27_save_regs(struct pt27_gpio *chip)
{
	chip->saved.dir       = pt27_rd32(chip, PT27_REG_DIR);
	chip->saved.out       = pt27_rd32(chip, PT27_REG_OUT);
	chip->saved.int_en    = pt27_rd32(chip, PT27_REG_INT_EN);
	chip->saved.int_lvl   = pt27_rd32(chip, PT27_REG_INT_LVL);
	chip->saved.int_mode0 = pt27_rd32(chip, PT27_REG_INT_MODE0);
	chip->saved.int_mode1 = pt27_rd16(chip, PT27_REG_INT_MODE1);
	chip->saved.int_mask  = pt27_rd32(chip, PT27_REG_INT_MASK);
	chip->saved.int_ctl   = pt27_rd8(chip, PT27_REG_INT_CTL);
	chip->saved.vendor0   = pt27_rd32(chip, PT27_REG_VENDOR0);
	chip->saved.vendor1   = pt27_rd32(chip, PT27_REG_VENDOR1);
	chip->saved.wake_en   = pt27_rd16(chip, PT27_REG_WAKE_EN);
	chip->saved.wake_lvl  = pt27_rd16(chip, PT27_REG_WAKE_LVL);
	chip->saved.wake_mode = pt27_rd16(chip, PT27_REG_WAKE_MODE);
	chip->saved_valid = true;

}

static void pt27_restore_regs(struct pt27_gpio *chip)
{
	pt27_wr32(chip, PT27_REG_INT_EN, 0);
	pt27_wr32(chip, PT27_REG_INT_MASK, PT27_GPIO_MASK);

	pt27_wr32(chip, PT27_REG_DIR,       chip->saved.dir);
	pt27_wr32(chip, PT27_REG_OUT,       chip->saved.out);
	pt27_wr32(chip, PT27_REG_INT_LVL,   chip->saved.int_lvl);
	pt27_wr32(chip, PT27_REG_INT_MODE0, chip->saved.int_mode0);
	pt27_wr16(chip, PT27_REG_INT_MODE1, chip->saved.int_mode1);
	pt27_wr8(chip, PT27_REG_INT_CTL,    chip->saved.int_ctl);
	pt27_wr32(chip, PT27_REG_VENDOR0,   chip->saved.vendor0);
	pt27_wr32(chip, PT27_REG_VENDOR1,   chip->saved.vendor1);

	pt27_wr16(chip, PT27_REG_WAKE_LVL,  chip->saved.wake_lvl);
	pt27_wr16(chip, PT27_REG_WAKE_MODE, chip->saved.wake_mode);
	pt27_wr16(chip, PT27_REG_WAKE_EN,   chip->saved.wake_en);

	pt27_wr32(chip, PT27_REG_INT_STAT,  PT27_GPIO_MASK);
	pt27_wr16(chip, PT27_REG_WAKE_STAT, PT27_WAKE_MASK);

	pt27_wr32(chip, PT27_REG_INT_EN,   chip->saved.int_en);
	pt27_wr32(chip, PT27_REG_INT_MASK, chip->saved.int_mask);

}

static int pt27_gpio_direction_input(struct gpio_chip *gc, unsigned int offset)
{
	struct pt27_gpio *chip = gpiochip_get_data(gc);
	unsigned long flags;
	u32 val;
	int ret;

	ret = pm_runtime_resume_and_get(chip->dev);
	if (ret < 0)
		return ret;

	raw_spin_lock_irqsave(&chip->lock, flags);
	val  = pt27_rd32(chip, PT27_REG_DIR);
	val &= ~BIT(offset);   /* 0 = input */
	pt27_wr32(chip, PT27_REG_DIR, val);
	raw_spin_unlock_irqrestore(&chip->lock, flags);

	pt27_runtime_put_autosuspend(chip);
	return 0;
}

static int pt27_gpio_direction_output(struct gpio_chip *gc, unsigned int offset,
				      int value)
{
	struct pt27_gpio *chip = gpiochip_get_data(gc);
	unsigned long flags;
	u32 val;
	int ret;

	ret = pm_runtime_resume_and_get(chip->dev);
	if (ret < 0)
		return ret;

	raw_spin_lock_irqsave(&chip->lock, flags);

	val = pt27_rd32(chip, PT27_REG_OUT);
	if (value)
		val |= BIT(offset);
	else
		val &= ~BIT(offset);
	pt27_wr32(chip, PT27_REG_OUT, val);

	val  = pt27_rd32(chip, PT27_REG_DIR);
	val |= BIT(offset);
	pt27_wr32(chip, PT27_REG_DIR, val);

	raw_spin_unlock_irqrestore(&chip->lock, flags);

	pt27_runtime_put_autosuspend(chip);
	return 0;
}

static int pt27_gpio_get_direction(struct gpio_chip *gc, unsigned int offset)
{
	struct pt27_gpio *chip = gpiochip_get_data(gc);
	unsigned long flags;
	u32 val;
	int ret;

	ret = pm_runtime_resume_and_get(chip->dev);
	if (ret < 0)
		return ret;

	raw_spin_lock_irqsave(&chip->lock, flags);
	val = pt27_rd32(chip, PT27_REG_DIR);
	raw_spin_unlock_irqrestore(&chip->lock, flags);

	pt27_runtime_put_autosuspend(chip);

	return (val & BIT(offset)) ? GPIO_LINE_DIRECTION_OUT
				   : GPIO_LINE_DIRECTION_IN;
}

static int pt27_gpio_get(struct gpio_chip *gc, unsigned int offset)
{
	struct pt27_gpio *chip = gpiochip_get_data(gc);
	unsigned long flags;
	u32 dir, val;
	int ret;

	ret = pm_runtime_resume_and_get(chip->dev);
	if (ret < 0)
		return ret;

	raw_spin_lock_irqsave(&chip->lock, flags);
	dir = pt27_rd32(chip, PT27_REG_DIR);
	val = pt27_rd32(chip, (dir & BIT(offset)) ? PT27_REG_OUT : PT27_REG_IN);
	raw_spin_unlock_irqrestore(&chip->lock, flags);

	pt27_runtime_put_autosuspend(chip);

	return !!(val & BIT(offset));
}

static int pt27_gpio_set(struct gpio_chip *gc, unsigned int offset, int value)
{
	struct pt27_gpio *chip = gpiochip_get_data(gc);
	unsigned long flags;
	u32 val;
	int ret;

	ret = pm_runtime_resume_and_get(chip->dev);
	if (ret < 0) {
		dev_err_ratelimited(chip->dev,
				    "%s: cannot resume to set GPIO%u: %d\n",
				     __func__, offset, ret);
		return ret;
	}

	raw_spin_lock_irqsave(&chip->lock, flags);
	val = pt27_rd32(chip, PT27_REG_OUT);
	if (value)
		val |= BIT(offset);
	else
		val &= ~BIT(offset);
	pt27_wr32(chip, PT27_REG_OUT, val);
	raw_spin_unlock_irqrestore(&chip->lock, flags);

	pt27_runtime_put_autosuspend(chip);
	return 0;
}

static u32 pt27_pending_irqs_locked(struct pt27_gpio *chip)
{
	u32 stat = pt27_rd32(chip, PT27_REG_INT_STAT);
	u32 en   = pt27_rd32(chip, PT27_REG_INT_EN);
	u32 mask = pt27_rd32(chip, PT27_REG_INT_MASK);

	return stat & en & ~mask & PT27_GPIO_MASK;
}

static bool pt27_irq_regs_accessible(struct pt27_gpio *chip)
{

	return READ_ONCE(chip->irq_bus_active) ||
	       READ_ONCE(chip->irq_pm_held);
}

static void pt27_irq_take_pm_hold(struct pt27_gpio *chip)
{
	unsigned long flags;
	bool take = false;

	raw_spin_lock_irqsave(&chip->lock, flags);
	if (!chip->irq_pm_held) {
		chip->irq_pm_held = true;
		take = true;
	}
	raw_spin_unlock_irqrestore(&chip->lock, flags);

	if (take)
		pm_runtime_get_noresume(chip->dev);
}

static void pt27_irq_mask(struct irq_data *d)
{
	struct gpio_chip *gc   = irq_data_get_irq_chip_data(d);
	struct pt27_gpio *chip = gpiochip_get_data(gc);
	irq_hw_number_t   hwirq = irqd_to_hwirq(d);
	unsigned long flags;
	u32 val;
	bool drop_pm = false;

	gpiochip_disable_irq(gc, hwirq);

	if (!pt27_irq_regs_accessible(chip)) {
		dev_err_ratelimited(chip->dev,
				    "%s: GPIO%lu remains masked; PT27 is not in D0\n",
				     __func__, hwirq);
		return;
	}

	raw_spin_lock_irqsave(&chip->lock, flags);

	val  = pt27_rd32(chip, PT27_REG_INT_MASK);
	val |= BIT(hwirq);
	pt27_wr32(chip, PT27_REG_INT_MASK, val);

	val  = pt27_rd32(chip, PT27_REG_INT_EN);
	val &= ~BIT(hwirq);
	pt27_wr32(chip, PT27_REG_INT_EN, val);

	chip->enabled_irq_mask &= ~BIT(hwirq);
	if (!chip->enabled_irq_mask && chip->irq_pm_held) {
		chip->irq_pm_held = false;
		drop_pm = true;
	}

	raw_spin_unlock_irqrestore(&chip->lock, flags);

	if (drop_pm)
		pt27_runtime_put_autosuspend(chip);

}

static void pt27_irq_unmask(struct irq_data *d)
{
	struct gpio_chip *gc   = irq_data_get_irq_chip_data(d);
	struct pt27_gpio *chip = gpiochip_get_data(gc);
	irq_hw_number_t   hwirq = irqd_to_hwirq(d);
	unsigned long flags;
	u32 val;

	if (!pt27_irq_regs_accessible(chip)) {
		dev_err_ratelimited(chip->dev,
				    "%s: cannot unmask GPIO%lu; PT27 is not in D0\n",
				     __func__, hwirq);
		return;
	}

	pt27_irq_take_pm_hold(chip);

	gpiochip_enable_irq(gc, hwirq);

	raw_spin_lock_irqsave(&chip->lock, flags);

	pt27_wr32(chip, PT27_REG_INT_STAT, BIT(hwirq));

	val  = pt27_rd32(chip, PT27_REG_INT_EN);
	val |= BIT(hwirq);
	pt27_wr32(chip, PT27_REG_INT_EN, val);

	val  = pt27_rd32(chip, PT27_REG_INT_MASK);
	val &= ~BIT(hwirq);
	pt27_wr32(chip, PT27_REG_INT_MASK, val);

	chip->enabled_irq_mask |= BIT(hwirq);

	raw_spin_unlock_irqrestore(&chip->lock, flags);

}

static void pt27_irq_ack(struct irq_data *d)
{
	struct gpio_chip *gc   = irq_data_get_irq_chip_data(d);
	struct pt27_gpio *chip = gpiochip_get_data(gc);
	irq_hw_number_t   hwirq = irqd_to_hwirq(d);

	pt27_wr32(chip, PT27_REG_INT_STAT, BIT(hwirq));
}

static int pt27_irq_set_type(struct irq_data *d, unsigned int type)
{
	struct gpio_chip *gc   = irq_data_get_irq_chip_data(d);
	struct pt27_gpio *chip = gpiochip_get_data(gc);
	irq_hw_number_t   hwirq = irqd_to_hwirq(d);
	unsigned long flags;
	u32 lvl_reg;
	u8  lvl_bit, mode_val;
	unsigned int shift;

	if (!pt27_irq_regs_accessible(chip))
		return -EIO;

	switch (type) {
	case IRQ_TYPE_LEVEL_HIGH:
		lvl_bit  = 1; mode_val = PT27_MODE_LEVEL;   break;
	case IRQ_TYPE_LEVEL_LOW:
		lvl_bit  = 0; mode_val = PT27_MODE_LEVEL;   break;
	case IRQ_TYPE_EDGE_RISING:
		lvl_bit  = 1; mode_val = PT27_MODE_RISING;  break;
	case IRQ_TYPE_EDGE_FALLING:
		lvl_bit  = 0; mode_val = PT27_MODE_FALLING; break;
	case IRQ_TYPE_EDGE_BOTH:
		lvl_bit  = 0; mode_val = PT27_MODE_BOTH;    break;
	default:
		return -EINVAL;
	}

	raw_spin_lock_irqsave(&chip->lock, flags);

	lvl_reg = pt27_rd32(chip, PT27_REG_INT_LVL);
	if (lvl_bit)
		lvl_reg |= BIT(hwirq);
	else
		lvl_reg &= ~BIT(hwirq);
	pt27_wr32(chip, PT27_REG_INT_LVL, lvl_reg);

	if (hwirq < 16) {
		u32 reg = pt27_rd32(chip, PT27_REG_INT_MODE0);

		shift  = hwirq * 2;
		reg   &= ~(PT27_INT_MODE_MASK << shift);
		reg   |= ((u32)mode_val << shift);
		pt27_wr32(chip, PT27_REG_INT_MODE0, reg);
	} else {
		u16 reg = pt27_rd16(chip, PT27_REG_INT_MODE1);

		shift  = (hwirq - 16) * 2;
		reg   &= ~((u16)(PT27_INT_MODE_MASK << shift));
		reg   |= (u16)(mode_val << shift);
		pt27_wr16(chip, PT27_REG_INT_MODE1, reg);
	}

	raw_spin_unlock_irqrestore(&chip->lock, flags);

	if (type & IRQ_TYPE_LEVEL_MASK)
		irq_set_handler_locked(d, handle_level_irq);
	else
		irq_set_handler_locked(d, handle_edge_irq);

	return 0;
}

static int pt27_irq_set_wake(struct irq_data *d, unsigned int on)
{
	struct gpio_chip *gc   = irq_data_get_irq_chip_data(d);
	struct pt27_gpio *chip = gpiochip_get_data(gc);
	irq_hw_number_t   hwirq = irqd_to_hwirq(d);
	unsigned int trig = irqd_get_trigger_type(d);
	unsigned long flags;
	u16  wake_en, wake_lvl, wake_mode;

	if (hwirq >= PT27_WAKE_GPIOS) {
		dev_warn(chip->dev,
			 "%s: GPIO%lu does not support wake (max GPIO%u)\n",
			 __func__, hwirq, PT27_WAKE_GPIOS - 1);
		return -EINVAL;
	}
	if (!pt27_irq_regs_accessible(chip))
		return -EIO;
	if (on && trig == IRQ_TYPE_NONE)
		return -EINVAL;

	raw_spin_lock_irqsave(&chip->lock, flags);

	wake_en   = pt27_rd16(chip, PT27_REG_WAKE_EN);
	wake_lvl  = pt27_rd16(chip, PT27_REG_WAKE_LVL);
	wake_mode = pt27_rd16(chip, PT27_REG_WAKE_MODE);

	if (on) {
		wake_en |= BIT(hwirq);
		chip->wake_mask |= (u16)BIT(hwirq);

		if (trig & IRQ_TYPE_LEVEL_MASK) {
			wake_mode |= BIT(hwirq);
			if (trig == IRQ_TYPE_LEVEL_HIGH)
				wake_lvl |= BIT(hwirq);
			else
				wake_lvl &= ~BIT(hwirq);
		} else {
			wake_mode &= ~BIT(hwirq);
			if (trig == IRQ_TYPE_EDGE_RISING ||
			    trig == IRQ_TYPE_EDGE_BOTH)
				wake_lvl |= BIT(hwirq);
			else
				wake_lvl &= ~BIT(hwirq);
		}
	} else {
		wake_en  &= ~BIT(hwirq);
		chip->wake_mask &= ~(u16)BIT(hwirq);
	}

	pt27_wr16(chip, PT27_REG_WAKE_EN,   wake_en);
	pt27_wr16(chip, PT27_REG_WAKE_LVL,  wake_lvl);
	pt27_wr16(chip, PT27_REG_WAKE_MODE, wake_mode);

	raw_spin_unlock_irqrestore(&chip->lock, flags);

	return 0;
}

static void pt27_irq_bus_lock(struct irq_data *d)
{
	struct gpio_chip *gc   = irq_data_get_irq_chip_data(d);
	struct pt27_gpio *chip = gpiochip_get_data(gc);
	int ret;

	mutex_lock(&chip->irq_bus_lock);

	ret = pm_runtime_resume_and_get(chip->dev);
	if (ret < 0) {
		WRITE_ONCE(chip->irq_bus_active, false);
		dev_err_ratelimited(chip->dev,
				    "%s: cannot resume for IRQ configuration: %d\n",
				     __func__, ret);
		return;
	}

	WRITE_ONCE(chip->irq_bus_active, true);
}

static void pt27_irq_bus_sync_unlock(struct irq_data *d)
{
	struct gpio_chip *gc   = irq_data_get_irq_chip_data(d);
	struct pt27_gpio *chip = gpiochip_get_data(gc);

	if (READ_ONCE(chip->irq_bus_active)) {
		WRITE_ONCE(chip->irq_bus_active, false);
		pt27_runtime_put_autosuspend(chip);
	}

	mutex_unlock(&chip->irq_bus_lock);
}

static const struct irq_chip pt27_gpio_irq_chip = {
	.name                = "pt27_gpio",
	.irq_ack             = pt27_irq_ack,
	.irq_mask            = pt27_irq_mask,
	.irq_unmask          = pt27_irq_unmask,
	.irq_set_type        = pt27_irq_set_type,
	.irq_set_wake        = pt27_irq_set_wake,
	.irq_bus_lock        = pt27_irq_bus_lock,
	.irq_bus_sync_unlock = pt27_irq_bus_sync_unlock,
	.flags               = IRQCHIP_IMMUTABLE,
	GPIOCHIP_IRQ_RESOURCE_HELPERS,
};

static irqreturn_t pt27_gpio_isr(int irq, void *data)
{
	struct pt27_gpio *chip = data;
	unsigned long pending;
	unsigned long flags;

	raw_spin_lock_irqsave(&chip->lock, flags);
	pending = pt27_pending_irqs_locked(chip);
	raw_spin_unlock_irqrestore(&chip->lock, flags);

	if (!pending)
		return IRQ_NONE;

	while (pending) {
		unsigned int hwirq = __ffs(pending);

		pending &= ~BIT(hwirq);
		generic_handle_domain_irq(chip->gc.irq.domain, hwirq);
	}

	return IRQ_HANDLED;
}

static void pt27_hw_init(struct pt27_gpio *chip)
{
	u8 int_ctl;

	int_ctl  = pt27_rd8(chip, PT27_REG_INT_CTL);
	int_ctl |=  PT27_INT_CTL_MODE;
	int_ctl &= ~PT27_INT_CTL_PAD_EN;
	pt27_wr8(chip, PT27_REG_INT_CTL, int_ctl);

	pt27_wr32(chip, PT27_REG_INT_EN,    0);
	pt27_wr32(chip, PT27_REG_INT_MASK,  PT27_GPIO_MASK);
	pt27_wr32(chip, PT27_REG_INT_STAT,  PT27_GPIO_MASK);
	pt27_wr16(chip, PT27_REG_WAKE_EN,   0);
	pt27_wr16(chip, PT27_REG_WAKE_STAT, PT27_WAKE_MASK);
}

static int pt27_gpio_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	struct device    *dev = &pdev->dev;
	struct pt27_gpio *chip;
	struct gpio_irq_chip *girq;
	unsigned long irq_flags;
	int ret;

	chip = devm_kzalloc(dev, sizeof(*chip), GFP_KERNEL);
	if (!chip)
		return -ENOMEM;

	chip->dev = dev;
	raw_spin_lock_init(&chip->lock);
	mutex_init(&chip->irq_bus_lock);
	pci_set_drvdata(pdev, chip);

	ret = pcim_enable_device(pdev);
	if (ret)
		return dev_err_probe(dev, ret, "failed to enable PCI device\n");

	chip->base = pcim_iomap_region(pdev, 0, "pt27_gpio");
	if (IS_ERR(chip->base))
		return dev_err_probe(dev, PTR_ERR(chip->base),
				     "failed to claim and map BAR0\n");

	pci_set_master(pdev);

	ret = pci_alloc_irq_vectors(pdev, 1, 1, PCI_IRQ_MSIX | PCI_IRQ_MSI);
	if (ret < 0) {
		ret = pci_alloc_irq_vectors(pdev, 1, 1, PCI_IRQ_INTX);
		if (ret < 0) {
			ret = dev_err_probe(dev, ret,
					    "failed to allocate IRQ vector\n");
			goto err_clear_master;
		}
		chip->use_msi = false;
		irq_flags     = IRQF_SHARED;
		dev_dbg(dev, "Using shared INTx interrupt\n");
	} else {
		chip->use_msi = true;
		irq_flags     = 0;
		dev_dbg(dev, "Using %s interrupt vector\n",
			pdev->msix_enabled ? "MSI-X" : "MSI");
	}
	chip->irq = pci_irq_vector(pdev, 0);

	pt27_hw_init(chip);

	pm_runtime_set_autosuspend_delay(dev, PT27_AUTOSUSPEND_DELAY_MS);
	pm_runtime_use_autosuspend(dev);

	chip->gc.label            = dev_name(dev);
	chip->gc.parent           = dev;
	chip->gc.owner            = THIS_MODULE;
	chip->gc.base             = -1;             /* Dynamic base assignment */
	chip->gc.ngpio            = PT27_NUM_GPIOS;
	chip->gc.direction_input  = pt27_gpio_direction_input;
	chip->gc.direction_output = pt27_gpio_direction_output;
	chip->gc.get_direction    = pt27_gpio_get_direction;
	chip->gc.get              = pt27_gpio_get;
	chip->gc.set              = pt27_gpio_set;
	chip->gc.can_sleep        = true;

	girq = &chip->gc.irq;
	gpio_irq_chip_set_chip(girq, &pt27_gpio_irq_chip);
	girq->parent_handler = NULL;
	girq->num_parents    = 0;
	girq->parents        = NULL;
	girq->default_type   = IRQ_TYPE_NONE;
	girq->handler        = handle_bad_irq;

	ret = device_init_wakeup(dev, true);
	if (ret) {
		ret = dev_err_probe(dev, ret, "failed to enable wake capability\n");
		goto err_free_vectors;
	}

	dev_pm_set_driver_flags(dev, DPM_FLAG_SMART_SUSPEND);

	ret = devm_request_irq(dev, chip->irq, pt27_gpio_isr,
			       irq_flags | IRQF_NO_THREAD, dev_name(dev), chip);
	if (ret) {
		ret = dev_err_probe(dev, ret,
				    "failed to request IRQ %d\n", chip->irq);
		goto err_wakeup;
	}

	ret = devm_gpiochip_add_data(dev, &chip->gc, chip);
	if (ret) {
		ret = dev_err_probe(dev, ret, "failed to register GPIO chip\n");
		goto err_free_irq;
	}

	dev_info(dev,
		 "PT27 GPIO controller: %u GPIOs (%u wake-capable), IRQ=%d (%s)\n",
		 PT27_NUM_GPIOS, PT27_WAKE_GPIOS, chip->irq,
		 chip->use_msi ? (pdev->msix_enabled ? "MSI-X" : "MSI") : "INTx");

	pm_runtime_put_autosuspend(dev);
	pm_runtime_allow(dev);

	return 0;

err_free_irq:
	devm_free_irq(dev, chip->irq, chip);
err_wakeup:
	device_init_wakeup(dev, false);
err_free_vectors:
	pci_free_irq_vectors(pdev);
err_clear_master:
	pci_clear_master(pdev);
	return ret;
}

static void pt27_gpio_remove(struct pci_dev *pdev)
{
	struct device     *dev  = &pdev->dev;
	struct pt27_gpio  *chip = pci_get_drvdata(pdev);
	unsigned long flags;
	bool drop_pm;

	/* Restore the canonical "unbound PCI device" Runtime-PM state. */
	pm_runtime_forbid(dev);
	pm_runtime_get_noresume(dev);
	pm_runtime_dont_use_autosuspend(dev);

	mutex_lock(&chip->irq_bus_lock);

	raw_spin_lock_irqsave(&chip->lock, flags);
	pt27_wr32(chip, PT27_REG_INT_EN,    0);
	pt27_wr32(chip, PT27_REG_INT_MASK,  PT27_GPIO_MASK);
	pt27_wr32(chip, PT27_REG_INT_STAT,  PT27_GPIO_MASK);
	pt27_wr16(chip, PT27_REG_WAKE_EN,   0);
	pt27_wr16(chip, PT27_REG_WAKE_STAT, PT27_WAKE_MASK);

	chip->enabled_irq_mask = 0;
	chip->wake_mask        = 0;
	drop_pm = chip->irq_pm_held;
	chip->irq_pm_held = false;
	raw_spin_unlock_irqrestore(&chip->lock, flags);

	if (drop_pm)
		pm_runtime_put_noidle(dev);

	device_init_wakeup(dev, false);

	devm_free_irq(dev, chip->irq, chip);
	pci_free_irq_vectors(pdev);
	pci_clear_master(pdev);

	mutex_unlock(&chip->irq_bus_lock);
}

static int __maybe_unused pt27_gpio_suspend(struct device *dev)
{
	struct pt27_gpio *chip = dev_get_drvdata(dev);
	unsigned long flags;
	u32 int_mask;

	if (pm_runtime_status_suspended(dev)) {
		dev_dbg(dev, "%s: already runtime-suspended, skipping save\n",
			__func__);
		return 0;
	}

	raw_spin_lock_irqsave(&chip->lock, flags);
	pt27_save_regs(chip);
	pt27_wr32(chip, PT27_REG_INT_EN,    0);
	int_mask = PT27_REG_INT_MASK & ~chip->wake_mask;
	pt27_wr32(chip, PT27_REG_INT_MASK,  int_mask);
	pt27_wr32(chip, PT27_REG_INT_STAT,  PT27_GPIO_MASK);
	pt27_wr16(chip, PT27_REG_WAKE_STAT, PT27_WAKE_MASK);
	raw_spin_unlock_irqrestore(&chip->lock, flags);

	return 0;
}

static int __maybe_unused pt27_gpio_resume(struct device *dev)
{
	struct pt27_gpio *chip = dev_get_drvdata(dev);
	unsigned long flags;
	u16 wake_status;

	if (!chip->saved_valid)
		return 0;

	raw_spin_lock_irqsave(&chip->lock, flags);
	wake_status = pt27_rd16(chip, PT27_REG_WAKE_STAT) & PT27_WAKE_MASK;
	pt27_restore_regs(chip);
	raw_spin_unlock_irqrestore(&chip->lock, flags);

	if (wake_status)
		pm_wakeup_event(dev, 0);

	return 0;
}

static int __maybe_unused pt27_gpio_runtime_suspend(struct device *dev)
{
	struct pt27_gpio *chip = dev_get_drvdata(dev);
	unsigned long flags;
	int ret = 0;

	raw_spin_lock_irqsave(&chip->lock, flags);

	if (chip->irq_pm_held || chip->enabled_irq_mask) {
		ret = -EBUSY;
		goto out_unlock;
	}

	if (chip->wake_mask && !device_may_wakeup(dev)) {
		ret = -EBUSY;
		goto out_unlock;
	}

	if (pt27_pending_irqs_locked(chip)) {
		ret = -EBUSY;
		goto out_unlock;
	}

	pt27_save_regs(chip);

	pt27_wr32(chip, PT27_REG_INT_EN,    0);
	pt27_wr32(chip, PT27_REG_INT_MASK,  PT27_GPIO_MASK);
	pt27_wr32(chip, PT27_REG_INT_STAT,  PT27_GPIO_MASK);
	pt27_wr16(chip, PT27_REG_WAKE_STAT, PT27_WAKE_MASK);

out_unlock:
	raw_spin_unlock_irqrestore(&chip->lock, flags);
	return ret;
}

static int __maybe_unused pt27_gpio_runtime_resume(struct device *dev)
{

	return pt27_gpio_resume(dev);
}

static const struct dev_pm_ops pt27_gpio_pm_ops __maybe_unused = {
	SYSTEM_SLEEP_PM_OPS(pt27_gpio_suspend, pt27_gpio_resume)
	RUNTIME_PM_OPS(pt27_gpio_runtime_suspend, pt27_gpio_runtime_resume, NULL)
};

#define PCI_DEVICE_ID_PT27	0x444b

static const struct pci_device_id pt27_gpio_pci_ids[] = {
	{ PCI_DEVICE(PCI_VENDOR_ID_AMD, PCI_DEVICE_ID_PT27) },
	{ }
};
MODULE_DEVICE_TABLE(pci, pt27_gpio_pci_ids);

static struct pci_driver pt27_gpio_pci_driver = {
	.name     = "ptpci-gpio",
	.id_table = pt27_gpio_pci_ids,
	.probe    = pt27_gpio_probe,
	.remove   = pt27_gpio_remove,
	.driver   = {
		.pm = pm_ptr(&pt27_gpio_pm_ops),
	},
};

module_pci_driver(pt27_gpio_pci_driver);

MODULE_DESCRIPTION("AMD PT PCI GPIO Controller");
MODULE_LICENSE("GPL");
