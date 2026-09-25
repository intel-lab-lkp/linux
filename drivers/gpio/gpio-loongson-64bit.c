// SPDX-License-Identifier: GPL-2.0+
/*
 * Loongson GPIO Support
 *
 * Copyright (C) 2022-2023 Loongson Technology Corporation Limited
 */

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/cleanup.h>
#include <linux/irq.h>
#include <linux/irqdesc.h>
#include <linux/module.h>
#include <linux/spinlock.h>
#include <linux/err.h>
#include <linux/gpio/driver.h>
#include <linux/gpio/generic.h>
#include <linux/platform_device.h>
#include <linux/bitops.h>
#include <linux/reset.h>
#include <linux/interrupt.h>
#include <asm/types.h>

enum loongson_gpio_mode {
	BIT_CTRL_MODE,
	BYTE_CTRL_MODE,
};

struct loongson_gpio_chip;

struct loongson_gpio_chip_data {
	const char		*label;
	enum loongson_gpio_mode	mode;
	unsigned int		conf_offset;
	unsigned int		out_offset;
	unsigned int		in_offset;
	unsigned int		inten_offset;
	unsigned int		intpol_offset;
	unsigned int		intedge_offset;
	unsigned int		intclr_offset;
	unsigned int		intsts_offset;
	unsigned int		intdual_offset;
	unsigned int		intr_num;
	irq_flow_handler_t	irq_handler;
	const struct irq_chip	*girqchip;
	unsigned int		(*irq_mapping)(struct loongson_gpio_chip *lgpio,
					       unsigned int pin);
	int			(*irq_init)(struct platform_device *pdev,
					    struct loongson_gpio_chip *lgpio);
};

struct loongson_gpio_shared_irq_data;

struct loongson_gpio_chip {
	struct gpio_generic_chip chip;
	spinlock_t		lock;
	void __iomem		*reg_base;
	struct loongson_gpio_shared_irq_data *shared_irq_data;
	const struct loongson_gpio_chip_data *chip_data;
};

static inline struct loongson_gpio_chip *to_loongson_gpio_chip(struct gpio_chip *chip)
{
	return container_of(to_gpio_generic_chip(chip),
			    struct loongson_gpio_chip, chip);
}

static inline void loongson_commit_direction(struct loongson_gpio_chip *lgpio, unsigned int pin,
					     int input)
{
	u8 bval = input ? 1 : 0;

	writeb(bval, lgpio->reg_base + lgpio->chip_data->conf_offset + pin);
}

static void loongson_commit_level(struct loongson_gpio_chip *lgpio, unsigned int pin, int high)
{
	u8 bval = high ? 1 : 0;

	writeb(bval, lgpio->reg_base + lgpio->chip_data->out_offset + pin);
}

static int loongson_gpio_direction_input(struct gpio_chip *chip, unsigned int pin)
{
	unsigned long flags;
	struct loongson_gpio_chip *lgpio = to_loongson_gpio_chip(chip);

	spin_lock_irqsave(&lgpio->lock, flags);
	loongson_commit_direction(lgpio, pin, 1);
	spin_unlock_irqrestore(&lgpio->lock, flags);

	return 0;
}

static int loongson_gpio_direction_output(struct gpio_chip *chip, unsigned int pin, int value)
{
	unsigned long flags;
	struct loongson_gpio_chip *lgpio = to_loongson_gpio_chip(chip);

	spin_lock_irqsave(&lgpio->lock, flags);
	loongson_commit_level(lgpio, pin, value);
	loongson_commit_direction(lgpio, pin, 0);
	spin_unlock_irqrestore(&lgpio->lock, flags);

	return 0;
}

static int loongson_gpio_get(struct gpio_chip *chip, unsigned int pin)
{
	u8  bval;
	int val;
	struct loongson_gpio_chip *lgpio = to_loongson_gpio_chip(chip);

	bval = readb(lgpio->reg_base + lgpio->chip_data->in_offset + pin);
	val = bval & 1;

	return val;
}

static int loongson_gpio_get_direction(struct gpio_chip *chip, unsigned int pin)
{
	u8  bval;
	struct loongson_gpio_chip *lgpio = to_loongson_gpio_chip(chip);

	bval = readb(lgpio->reg_base + lgpio->chip_data->conf_offset + pin);
	if (bval & 1)
		return GPIO_LINE_DIRECTION_IN;

	return GPIO_LINE_DIRECTION_OUT;
}

static int loongson_gpio_set(struct gpio_chip *chip, unsigned int pin, int value)
{
	unsigned long flags;
	struct loongson_gpio_chip *lgpio = to_loongson_gpio_chip(chip);

	spin_lock_irqsave(&lgpio->lock, flags);
	loongson_commit_level(lgpio, pin, value);
	spin_unlock_irqrestore(&lgpio->lock, flags);

	return 0;
}

static void loongson_gpio_irq_ack(struct irq_data *data)
{
	struct gpio_chip *chip = irq_data_get_irq_chip_data(data);
	struct loongson_gpio_chip *lgpio = to_loongson_gpio_chip(chip);
	irq_hw_number_t hwirq = irqd_to_hwirq(data);

	writeb(0x1, lgpio->reg_base + lgpio->chip_data->intclr_offset + hwirq);
}

static void loongson_gpio_irq_mask(struct irq_data *data)
{
	struct gpio_chip *chip = irq_data_get_irq_chip_data(data);
	struct loongson_gpio_chip *lgpio = to_loongson_gpio_chip(chip);
	irq_hw_number_t hwirq = irqd_to_hwirq(data);

	writeb(0x0, lgpio->reg_base + lgpio->chip_data->inten_offset + hwirq);
}

static void loongson_gpio_irq_unmask(struct irq_data *data)
{
	struct gpio_chip *chip = irq_data_get_irq_chip_data(data);
	struct loongson_gpio_chip *lgpio = to_loongson_gpio_chip(chip);
	irq_hw_number_t hwirq = irqd_to_hwirq(data);

	writeb(0x1, lgpio->reg_base + lgpio->chip_data->inten_offset + hwirq);
}

static int loongson_gpio_irq_set_type(struct irq_data *data, unsigned int type)
{
	struct gpio_chip *chip = irq_data_get_irq_chip_data(data);
	struct loongson_gpio_chip *lgpio = to_loongson_gpio_chip(chip);
	irq_hw_number_t hwirq = irqd_to_hwirq(data);
	u8 pol = 0, edge = 0, dual = 0;

	if ((type & IRQ_TYPE_SENSE_MASK) == IRQ_TYPE_EDGE_BOTH) {
		if (!lgpio->chip_data->intdual_offset)
			return -EINVAL;
		edge = 1;
		dual = 1;
		irq_set_handler_locked(data, handle_edge_irq);
	} else {
		switch (type) {
		case IRQ_TYPE_LEVEL_HIGH:
			pol = 1;
			fallthrough;
		case IRQ_TYPE_LEVEL_LOW:
			irq_set_handler_locked(data, handle_level_irq);
			break;

		case IRQ_TYPE_EDGE_RISING:
			pol = 1;
			fallthrough;
		case IRQ_TYPE_EDGE_FALLING:
			edge = 1;
			irq_set_handler_locked(data, handle_edge_irq);
			break;

		default:
			return -EINVAL;
		}
	}

	writeb(pol, lgpio->reg_base + lgpio->chip_data->intpol_offset + hwirq);
	writeb(edge, lgpio->reg_base + lgpio->chip_data->intedge_offset + hwirq);
	if (lgpio->chip_data->intdual_offset)
		writeb(dual, lgpio->reg_base + lgpio->chip_data->intdual_offset + hwirq);

	return 0;
}

static void loongson_gpio_ls2k0300_irq_handler(struct irq_desc *desc)
{
	struct loongson_gpio_chip *lgpio = irq_desc_get_handler_data(desc);
	struct irq_chip *girqchip = irq_desc_get_chip(desc);
	int i;

	chained_irq_enter(girqchip, desc);

	for (i = 0; i < lgpio->chip.gc.ngpio; i++) {
		/*
		 * For the GPIO controller of LS2K0300, interrupts status bits
		 * may be wrongly set even if the corresponding interrupt is
		 * disabled. Thus interrupt enable bits are checked along with
		 * status bits to detect interrupts reliably.
		 */
		if (readb(lgpio->reg_base + lgpio->chip_data->intsts_offset + i) &&
		    readb(lgpio->reg_base + lgpio->chip_data->inten_offset + i))
			generic_handle_domain_irq(lgpio->chip.gc.irq.domain, i);
	}

	chained_irq_exit(girqchip, desc);
}

static const struct irq_chip loongson_gpio_ls2k0300_irqchip = {
	.irq_ack	= loongson_gpio_irq_ack,
	.irq_mask	= loongson_gpio_irq_mask,
	.irq_unmask	= loongson_gpio_irq_unmask,
	.irq_set_type	= loongson_gpio_irq_set_type,
	.flags		= IRQCHIP_IMMUTABLE | IRQCHIP_SKIP_SET_WAKE,
	GPIOCHIP_IRQ_RESOURCE_HELPERS,
};

static int loongson_gpio_init_irqchip(struct platform_device *pdev,
				      struct loongson_gpio_chip *lgpio)
{
	const struct loongson_gpio_chip_data *data = lgpio->chip_data;
	struct gpio_chip *chip = &lgpio->chip.gc;
	int i;

	chip->irq.default_type = IRQ_TYPE_NONE;
	chip->irq.handler = handle_bad_irq;
	chip->irq.parent_handler = data->irq_handler;
	chip->irq.parent_handler_data = lgpio;
	gpio_irq_chip_set_chip(&chip->irq, data->girqchip);

	chip->irq.num_parents = 0;
	chip->irq.parents = devm_kcalloc(&pdev->dev, data->intr_num,
					 sizeof(*chip->irq.parents), GFP_KERNEL);
	if (!chip->irq.parents)
		return -ENOMEM;

	for (i = 0; i < data->intr_num; i++) {
		int ret;

		ret = platform_get_irq_optional(pdev, i);
		if (ret == -ENXIO)
			continue;
		if (ret < 0)
			return dev_err_probe(&pdev->dev, ret,
					     "failed to get IRQ %d\n", i);
		chip->irq.parents[chip->irq.num_parents++] = ret;
	}

	for (i = 0; i < data->intr_num; i++) {
		writeb(0x0, lgpio->reg_base + data->inten_offset + i);
		writeb(0x1, lgpio->reg_base + data->intclr_offset + i);
	}

	return 0;
}

/*
 * About shared IRQs for GPIO pins:
 *
 * This kind of GPIO controller lacks interrupt status register to identify
 * edge-triggered interrupts, and the handling of such interrupts must be
 * deligated to the parent IRQ controller.
 * This kind of GPIO controller should typically have a inten register to enable
 * or disable interrupts for individual GPIO pins, intpol register to configure
 * the interrupt polarity. The signal sent to the parent interrupt controller
 * is calculated by bit-wise comparing the GPIO line and the intpol register and
 * then masking with inten.
 * However, The interrput input lines of the parent IRQ controller is not enough
 * to 1:1 map to the GPIO pins, necessitating the use of shared IRQs. A mapping
 * is statically establised to map multiple GPIO pins to a single parent IRQ
 * line. The level of the parent IRQ is determined by or-ing the levels of all
 * GPIO pins mapped to it. Suppose gpio pin 0 and gpio pin 8 are mapped to the
 * same parent IRQ, the level of the parent IRQ is determined by the following
 * expression:
 *     (~(gpio[0] ^ intpol[0]) & inten[0]) |
 *     (~(gpio[8] ^ intpol[8]) & inten[8])
 * When the intpol register is not available, its value is assumed to be 1, and
 * the expression becomes:
 *     (gpio[0] & inten[0]) | (gpio[8] & inten[8])
 *
 * According to the hardware design, IRQ can be requested on only one of the
 * GPIO pins sharing the same parent IRQ, unless the intpol register is
 * available and all the IRQs are level-triggered.
 *
 * Convention on the usage of shared IRQs for GPIO pins:
 * - When a parent IRQ is exclusively used, the parent IRQ should be masked or
 *   unmasked accordingly when the irq pin using it is masked or unmasked.
 * - When a parent IRQ is shared among multiple GPIO pins, the parent IRQ
 *   should always be unmasked and masking or unmasking individual GPIO pins
 *   should be done through the GPIO controller's inten register.
 */

/*
 * We assume a maximum of 128 GPIOs for the bitmap, which should be
 * sufficient for all the actual hardware controllers defined in this
 * driver.
 */
enum { MAX_GPIO_WITH_IRQ = 128 };

struct loongson_gpio_shared_per_parent_irq_data {
	unsigned int parent_index;
	unsigned int parent_irq;
	/*
	 * The lock of the irq_desc of the parent irq also protects
	 * exclusive/usedby below. It must be taken before
	 * lgpio->shared_irq_data->lock.
	 */
	struct irq_data *parent_irq_data;
	struct loongson_gpio_chip *lgpio;

	/*
	 * exclusive: exclusive usage, only one gpio pin can use this parent irq.
	 *   It will happen in two cases:
	 *     - The parent irq is used by a gpio pin which is edge triggered, and
	 *       thus cannot be used by other gpio pins which are mapped to the same
	 *       parent irq.
	 *     - The parent irq is used by a gpio pin which is level triggered, and
	 *       the gpio chip lacks the polarity control register, and thus the
	 *       interrupt polarity is controlled by the parent irq controller, and
	 *       thus cannot be used by other gpio pins which are mapped to the
	 *       same parent irq.
	 *   When set, the request for other gpio pins which are mapped to this
	 *   parent irq will be rejected.
	 */
	unsigned int exclusive : 1;
	/*
	 * usedby: When exclusive is set, these bits indicate the gpio pin which
	 *   is using this parent irq. When exclusive is cleared, these bits
	 *   indicate the number of gpio pins which are using this parent irq.
	 * When both exclusive and usedby are 0, it means that the parent irq is not
	 * used by any gpio pin.
	 */
	unsigned int usedby : 31;
};

struct loongson_gpio_shared_irq_data {
	/*
	 * Protects the read/write operations on the irq related registers
	 */
	raw_spinlock_t lock;
	int *irq_in_use;
	struct loongson_gpio_shared_per_parent_irq_data per_parent_irq_data[];
};

static void loongson_gpio_write_register(struct loongson_gpio_chip *lgpio,
					 unsigned int reg_offset,
					 unsigned int line, u8 value)
{
	void __iomem *reg = lgpio->reg_base + reg_offset;

	if (lgpio->chip_data->mode == BIT_CTRL_MODE) {
		u32 bval;

		reg += (line / 32) * 4;
		bval = readl(reg);

		if (value)
			bval |= BIT(line % 32);
		else
			bval &= ~BIT(line % 32);

		writel(bval, reg);
	} else {
		writeb(value, reg + line);
	}
}

static u8 loongson_gpio_read_register(struct loongson_gpio_chip *lgpio,
				      unsigned int reg_offset,
				      unsigned int line)
{
	void __iomem *reg = lgpio->reg_base + reg_offset;

	if (lgpio->chip_data->mode == BIT_CTRL_MODE) {
		u32 bval;

		reg += (line / 32) * 4;
		bval = readl(reg);

		return (bval >> (line % 32)) & 1;
	}

	return readb(reg + line) & 1;
}

static void loongson_gpio_shared_mask_locked(struct loongson_gpio_chip *lgpio,
					  irq_hw_number_t hwirq,
					  struct loongson_gpio_shared_per_parent_irq_data *ppid)
{
	struct irq_data *parent_irq_data = ppid->parent_irq_data;

	if (ppid->exclusive)
		parent_irq_data->chip->irq_mask(parent_irq_data);

	scoped_guard(raw_spinlock_irqsave, &lgpio->shared_irq_data->lock)
		loongson_gpio_write_register(lgpio, lgpio->chip_data->inten_offset,
					     hwirq, 0);
}

static void loongson_gpio_shared_irq_mask(struct irq_data *data)
{
	struct gpio_chip *chip = irq_data_get_irq_chip_data(data);
	struct loongson_gpio_chip *lgpio = to_loongson_gpio_chip(chip);
	irq_hw_number_t hwirq = irqd_to_hwirq(data);
	unsigned int parent_irq_index = lgpio->chip_data->irq_mapping(lgpio, hwirq);
	struct loongson_gpio_shared_per_parent_irq_data *ppid =
		&lgpio->shared_irq_data->per_parent_irq_data[parent_irq_index];

	/*
	 * If irq_in_use is not set, then set_type handler must has not been called
	 * and no trigger type has been set for this gpio pin, so we should not
	 * proceed and do nothing.
	 */
	if (!lgpio->shared_irq_data->irq_in_use[hwirq])
		return;

	guard(raw_spinlock_irqsave)(&irq_data_to_desc(ppid->parent_irq_data)->lock);

	loongson_gpio_shared_mask_locked(lgpio, hwirq, ppid);
}

static void loongson_gpio_shared_irq_unmask(struct irq_data *data)
{
	struct gpio_chip *chip = irq_data_get_irq_chip_data(data);
	struct loongson_gpio_chip *lgpio = to_loongson_gpio_chip(chip);
	irq_hw_number_t hwirq = irqd_to_hwirq(data);
	unsigned int parent_irq_index = lgpio->chip_data->irq_mapping(lgpio, hwirq);
	struct loongson_gpio_shared_per_parent_irq_data *ppid =
		&lgpio->shared_irq_data->per_parent_irq_data[parent_irq_index];
	struct irq_data *parent_irq_data = ppid->parent_irq_data;

	if (!lgpio->shared_irq_data->irq_in_use[hwirq])
		return;

	guard(raw_spinlock_irqsave)(&irq_data_to_desc(ppid->parent_irq_data)->lock);

	if (ppid->exclusive)
		parent_irq_data->chip->irq_mask(parent_irq_data);

	scoped_guard(raw_spinlock_irqsave, &lgpio->shared_irq_data->lock)
		loongson_gpio_write_register(lgpio, lgpio->chip_data->inten_offset,
					     hwirq, 1);

	if (ppid->exclusive)
		parent_irq_data->chip->irq_unmask(parent_irq_data);
}

static void loongson_gpio_shared_irq_handler(struct irq_desc *desc);

static int loongson_gpio_shared_irq_set_type(struct irq_data *data, unsigned int type)
{
	struct gpio_chip *chip = irq_data_get_irq_chip_data(data);
	struct loongson_gpio_chip *lgpio = to_loongson_gpio_chip(chip);
	irq_hw_number_t hwirq = irqd_to_hwirq(data);
	unsigned int parent_irq_index = lgpio->chip_data->irq_mapping(lgpio, hwirq);
	struct loongson_gpio_shared_per_parent_irq_data *ppid =
		&lgpio->shared_irq_data->per_parent_irq_data[parent_irq_index];
	unsigned int intpol_offset = lgpio->chip_data->intpol_offset;
	struct irq_data *parent_irq_data = ppid->parent_irq_data;
	struct irq_chip *parent_irq_chip = parent_irq_data->chip;
	int ret = 0;

	type &= IRQ_TYPE_SENSE_MASK;

	if (type != IRQ_TYPE_EDGE_FALLING && type != IRQ_TYPE_EDGE_RISING &&
	    type != IRQ_TYPE_LEVEL_LOW && type != IRQ_TYPE_LEVEL_HIGH)
		return -EINVAL;

	guard(raw_spinlock_irqsave)(&irq_data_to_desc(ppid->parent_irq_data)->lock);

	if (ppid->exclusive && hwirq != ppid->usedby)
		return -EBUSY;

	/*
	 * When the irq is not exclusively used and is currently shared by
	 * more than one gpio pin; or is currently used by single gpio pin and
	 * the gpio pin is not the one requesting to set the irq type, then the
	 * irq type can only be set to level triggered.
	 */
	if (!ppid->exclusive &&
	    (ppid->usedby > 1 ||
	     (ppid->usedby == 1 && !lgpio->shared_irq_data->irq_in_use[hwirq]))) {
		if (type != IRQ_TYPE_LEVEL_HIGH && type != IRQ_TYPE_LEVEL_LOW)
			return -EBUSY;

		/*
		 * When exclusive is not set and usedby at least one, the gpio chip must
		 * have intpol register to control the interrupt polarity. We will never
		 * reach here if the gpio chip lacks intpol register, because in that
		 * case, exclusive will be set no matter what trigger type is requested.
		 */
		BUG_ON(!intpol_offset);
		/*
		 * Since only level triggered irqs are allowed to be shared, we can
		 * safely set the intpol register without first disabling the pin in
		 * inten register.
		 */
		scoped_guard(raw_spinlock_irqsave, &lgpio->shared_irq_data->lock)
			loongson_gpio_write_register(lgpio, intpol_offset, hwirq,
						     type == IRQ_TYPE_LEVEL_HIGH);

		if (!lgpio->shared_irq_data->irq_in_use[hwirq])
			ppid->usedby++;

		irq_set_handler_locked(data, handle_level_irq);
	} else {
		parent_irq_chip->irq_mask(parent_irq_data);

		if (type == IRQ_TYPE_EDGE_FALLING || type == IRQ_TYPE_EDGE_RISING) {
			if (intpol_offset) {
				ret = parent_irq_chip->irq_set_type(parent_irq_data,
								    IRQ_TYPE_EDGE_RISING);
				if (ret < 0)
					goto out_restore_handler;

				scoped_guard(raw_spinlock_irqsave, &lgpio->shared_irq_data->lock)
					loongson_gpio_write_register(lgpio, intpol_offset,
								     hwirq,
								     type == IRQ_TYPE_EDGE_RISING);
			} else {
				ret = parent_irq_chip->irq_set_type(parent_irq_data, type);
				if (ret < 0)
					goto out_restore_handler;
			}

			ppid->exclusive = 1;
			ppid->usedby = hwirq;

			/*
			 * We deliberately set the handler to level_irq even for
			 * edge-triggered interrupts because we cannot provide .irq_ack()
			 * handler.
			 */
			irq_set_handler_locked(data, handle_level_irq);
		} else {
			if (intpol_offset) {
				ret = parent_irq_chip->irq_set_type(parent_irq_data,
								    IRQ_TYPE_LEVEL_HIGH);
				if (ret < 0)
					goto out_restore_handler;

				scoped_guard(raw_spinlock_irqsave, &lgpio->shared_irq_data->lock)
					loongson_gpio_write_register(lgpio, intpol_offset,
								     hwirq,
								     type == IRQ_TYPE_LEVEL_HIGH);
				ppid->exclusive = 0;
				ppid->usedby = 1;
			} else {
				ret = parent_irq_chip->irq_set_type(parent_irq_data, type);
				if (ret < 0)
					goto out_restore_handler;

				ppid->exclusive = 1;
				ppid->usedby = hwirq;
			}

			irq_set_handler_locked(data, handle_level_irq);
		}
out_restore_handler:
		/* ->irq_set_type() would override irq_handler, so restore here */
		irq_set_handler_locked(parent_irq_data, loongson_gpio_shared_irq_handler);
		if (ret < 0)
			goto out;
	}

	lgpio->shared_irq_data->irq_in_use[hwirq] = 1;

out:
	if (ppid->exclusive) {
		u8 inten;

		scoped_guard(raw_spinlock_irqsave, &lgpio->shared_irq_data->lock)
			inten = loongson_gpio_read_register(lgpio, lgpio->chip_data->inten_offset,
							    hwirq);
		if (inten)
			parent_irq_chip->irq_unmask(parent_irq_data);
	} else
		parent_irq_chip->irq_unmask(parent_irq_data);

	return ret;
}

static void loongson_gpio_shared_irq_shutdown(struct irq_data *data)
{
	struct gpio_chip *chip = irq_data_get_irq_chip_data(data);
	struct loongson_gpio_chip *lgpio = to_loongson_gpio_chip(chip);
	irq_hw_number_t hwirq = irqd_to_hwirq(data);
	unsigned int parent_irq_index = lgpio->chip_data->irq_mapping(lgpio, hwirq);
	struct loongson_gpio_shared_per_parent_irq_data *ppid =
		&lgpio->shared_irq_data->per_parent_irq_data[parent_irq_index];

	if (!lgpio->shared_irq_data->irq_in_use[hwirq])
		return;

	guard(raw_spinlock_irqsave)(&irq_data_to_desc(ppid->parent_irq_data)->lock);

	loongson_gpio_shared_mask_locked(lgpio, hwirq, ppid);

	if (!ppid->exclusive) {
		if (lgpio->shared_irq_data->irq_in_use[hwirq]) {
			if (!WARN_ON(ppid->usedby == 0))
				ppid->usedby--;
		}
	} else {
		if (ppid->usedby == hwirq) {
			ppid->usedby = 0;
			ppid->exclusive = 0;
		}
	}

	lgpio->shared_irq_data->irq_in_use[hwirq] = 0;
}

static void loongson_gpio_shared_irq_init_valid_mask(struct gpio_chip *gc,
						     unsigned long *valid_mask,
						     unsigned int ngpios)
{
	struct loongson_gpio_chip *lgpio = to_loongson_gpio_chip(gc);
	struct loongson_gpio_shared_irq_data *shared_irq_data = lgpio->shared_irq_data;
	const struct loongson_gpio_chip_data *data = lgpio->chip_data;
	unsigned int i;

	for (i = 0; i < ngpios; i++) {
		unsigned int parent_irq_index = data->irq_mapping(lgpio, i);

		/*
		 * A parent_irq of 0 means the corresponding interrupt line is
		 * absent from the device tree or ACPI namespace.
		 */
		if (i >= MAX_GPIO_WITH_IRQ ||
		    parent_irq_index >= data->intr_num ||
		    !shared_irq_data->per_parent_irq_data[parent_irq_index].parent_irq)
			clear_bit(i, valid_mask);
	}
}

static int loongson_gpio_shared_irq_init_hw(struct gpio_chip *gc)
{
	struct loongson_gpio_chip *lgpio = to_loongson_gpio_chip(gc);
	const struct loongson_gpio_chip_data *data = lgpio->chip_data;
	struct loongson_gpio_shared_irq_data *shared_irq_data = lgpio->shared_irq_data;
	int i;

	shared_irq_data->irq_in_use = devm_kcalloc(gc->parent, gc->ngpio,
						   sizeof(*shared_irq_data->irq_in_use),
						   GFP_KERNEL);

	if (!shared_irq_data->irq_in_use)
		return -ENOMEM;

	for (i = 0; i < gc->ngpio; i++)
		loongson_gpio_write_register(lgpio, data->inten_offset, i, 0);

	return 0;
}

static int loongson_gpio_init_shared_irqchip(struct platform_device *pdev,
					     struct loongson_gpio_chip *lgpio)
{
	const struct loongson_gpio_chip_data *data = lgpio->chip_data;
	struct gpio_chip *chip = &lgpio->chip.gc;
	struct loongson_gpio_shared_irq_data *shared_irq_data;
	int i;
	int available_parent_irqs;

	chip->irq.default_type = IRQ_TYPE_NONE;
	chip->irq.handler = handle_bad_irq;
	chip->irq.parent_handler = data->irq_handler;
	chip->irq.init_valid_mask = loongson_gpio_shared_irq_init_valid_mask;
	chip->irq.init_hw = loongson_gpio_shared_irq_init_hw;

	shared_irq_data = devm_kzalloc(&pdev->dev,
				       struct_size(shared_irq_data,
						   per_parent_irq_data,
						   data->intr_num),
				       GFP_KERNEL);
	if (!shared_irq_data)
		return -ENOMEM;

	lgpio->shared_irq_data = shared_irq_data;
	raw_spin_lock_init(&shared_irq_data->lock);
	for (i = 0; i < data->intr_num; i++) {
		shared_irq_data->per_parent_irq_data[i].parent_index = i;
		shared_irq_data->per_parent_irq_data[i].lgpio = lgpio;
	}

	gpio_irq_chip_set_chip(&chip->irq, data->girqchip);

	chip->irq.parents = devm_kcalloc(&pdev->dev, data->intr_num,
					 sizeof(*chip->irq.parents), GFP_KERNEL);
	if (!chip->irq.parents)
		return -ENOMEM;

	chip->irq.per_parent_data = true;
	chip->irq.parent_handler_data_array =
		devm_kcalloc(&pdev->dev, data->intr_num,
			     sizeof(*chip->irq.parent_handler_data_array),
			     GFP_KERNEL);
	if (!chip->irq.parent_handler_data_array)
		return -ENOMEM;

	for (i = 0, available_parent_irqs = 0; i < data->intr_num; i++) {
		int ret;
		struct irq_data *parent_irq_data;
		int dup = 0, j;

		ret = platform_get_irq_optional(pdev, i);

		if (ret == -ENXIO)
			continue;

		if (ret < 0)
			return dev_err_probe(&pdev->dev, ret,
					     "failed to get IRQ %d\n", i);

		for (j = 0; j < available_parent_irqs; j++)
			if (chip->irq.parents[j] == ret) {
				dup = 1;
				break;
			}
		if (dup) {
			dev_warn(&pdev->dev,
				 FW_BUG "IRQ for parent index %d is duplicated, skipping\n", i);
			continue;
		}

		parent_irq_data = irq_get_irq_data(ret);
		if (WARN_ON(!parent_irq_data))
			continue;
		/* Implementing ->irq_set_type() is required for a parent irqchip */
		if (parent_irq_data->chip->irq_set_type == NULL)
			continue;

		chip->irq.parents[available_parent_irqs] = ret;
		chip->irq.parent_handler_data_array[available_parent_irqs] =
			&shared_irq_data->per_parent_irq_data[i];

		shared_irq_data->per_parent_irq_data[i].parent_irq = ret;
		shared_irq_data->per_parent_irq_data[i].parent_irq_data =
			irq_get_irq_data(ret);

		available_parent_irqs++;
	}
	chip->irq.num_parents = available_parent_irqs;

	return 0;
}

/*
 * Determine the GPIO pins which are triggering the parent irq given by
 * ppid, by comparing each mapped pin's line level against its polarity.
 */
static void loongson_gpio_shared_irq_poll(struct loongson_gpio_chip *lgpio,
					  unsigned int parent_index,
					  unsigned long *irq_pending_map,
					  unsigned int ngpio)
{
	const struct loongson_gpio_chip_data *data = lgpio->chip_data;
	unsigned int i;

	for (i = 0; i < ngpio; i++) {
		u8 inten, intpol, level;

		if (data->irq_mapping(lgpio, i) != parent_index)
			continue;

		scoped_guard(raw_spinlock_irqsave, &lgpio->shared_irq_data->lock) {
			inten = !!loongson_gpio_read_register(lgpio, data->inten_offset, i);
			intpol = !!loongson_gpio_read_register(lgpio, data->intpol_offset, i);
			level = !!loongson_gpio_read_register(lgpio, data->in_offset, i);
		}

		if (inten && intpol == level)
			bitmap_set(irq_pending_map, i, 1);
	}
}

static void loongson_gpio_shared_irq_handler(struct irq_desc *desc)
{
	struct loongson_gpio_shared_per_parent_irq_data *ppid =
		irq_desc_get_handler_data(desc);
	struct loongson_gpio_chip *lgpio = ppid->lgpio;
	struct gpio_chip *chip = &lgpio->chip.gc;
	struct irq_chip *girqchip = irq_desc_get_chip(desc);
	unsigned int ngpio = chip->ngpio > MAX_GPIO_WITH_IRQ ? MAX_GPIO_WITH_IRQ : chip->ngpio;
	unsigned int exclusive, usedby, i;
	DECLARE_BITMAP(irq_pending_map, MAX_GPIO_WITH_IRQ);

	bitmap_clear(irq_pending_map, 0, ngpio);

	chained_irq_enter(girqchip, desc);

	scoped_guard(raw_spinlock_irqsave, &desc->lock) {
		exclusive = ppid->exclusive;
		usedby = ppid->usedby;
	}
	if (exclusive && usedby < ngpio)
		bitmap_set(irq_pending_map, usedby, 1);
	else if (usedby) {
		/*
		 * When a parent irq is shared by more than 1 gpio pin,
		 * these IRQs must be level-triggered, and the polarity
		 * register must be available in the GPIO controller to
		 * determine the triggering GPIO line.
		 */
		loongson_gpio_shared_irq_poll(lgpio,
					      ppid->parent_index,
					      irq_pending_map,
					      ngpio);
	}

	if (!bitmap_empty(irq_pending_map, ngpio)) {
		for (i = 0; i < ngpio; i++) {
			if (test_bit(i, irq_pending_map))
				generic_handle_domain_irq(chip->irq.domain, i);
		}
	}

	chained_irq_exit(girqchip, desc);
}

static const struct irq_chip loongson_gpio_shared_irqchip = {
	.irq_mask	= loongson_gpio_shared_irq_mask,
	.irq_unmask	= loongson_gpio_shared_irq_unmask,
	.irq_set_type	= loongson_gpio_shared_irq_set_type,
	.irq_shutdown	= loongson_gpio_shared_irq_shutdown,
	.flags		= IRQCHIP_IMMUTABLE | IRQCHIP_SKIP_SET_WAKE,
	GPIOCHIP_IRQ_RESOURCE_HELPERS,
};

static unsigned int loongson_gpio_irq_mapping_mod8(struct loongson_gpio_chip *lgpio,
						   unsigned int pin)
{
	return pin % 8;
}

static unsigned int loongson_gpio_irq_mapping_cap4(struct loongson_gpio_chip *lgpio,
						   unsigned int pin)
{
	return pin >= 4 ? 4 : pin;
}

static unsigned int loongson_gpio_irq_mapping_cap4_2(struct loongson_gpio_chip *lgpio,
						    unsigned int pin)
{
	if (pin < 4)
		return pin;
	else if (pin < 32)
		return 4;
	else
		return 5;
}

static unsigned int loongson_gpio_irq_mapping_div32(struct loongson_gpio_chip *lgpio,
						    unsigned int pin)
{
	return pin / 32;
}

static int loongson_gpio_init(struct platform_device *pdev, struct loongson_gpio_chip *lgpio,
			      void __iomem *reg_base)
{
	struct gpio_generic_chip_config config;
	int ret;

	lgpio->reg_base = reg_base;
	if (lgpio->chip_data->mode == BIT_CTRL_MODE) {
		config = (struct gpio_generic_chip_config) {
			.dev = &pdev->dev,
			.sz = 8,
			.dat = lgpio->reg_base + lgpio->chip_data->in_offset,
			.set = lgpio->reg_base + lgpio->chip_data->out_offset,
			.dirin = lgpio->reg_base + lgpio->chip_data->conf_offset,
		};

		ret = gpio_generic_chip_init(&lgpio->chip, &config);
		if (ret) {
			dev_err(&pdev->dev, "unable to init generic GPIO\n");
			return ret;
		}
	} else {
		lgpio->chip.gc.direction_input = loongson_gpio_direction_input;
		lgpio->chip.gc.get = loongson_gpio_get;
		lgpio->chip.gc.get_direction = loongson_gpio_get_direction;
		lgpio->chip.gc.direction_output = loongson_gpio_direction_output;
		lgpio->chip.gc.set = loongson_gpio_set;
		lgpio->chip.gc.parent = &pdev->dev;
		lgpio->chip.gc.base = -1;
		spin_lock_init(&lgpio->lock);
	}

	lgpio->chip.gc.label = lgpio->chip_data->label;
	lgpio->chip.gc.can_sleep = false;
	if (lgpio->chip_data->girqchip) {
		ret = lgpio->chip_data->irq_init(pdev, lgpio);
		if (ret)
			return dev_err_probe(&pdev->dev, ret, "failed to initialize irqchip\n");
		if (lgpio->chip.gc.irq.num_parents == 0) {
			dev_warn(&pdev->dev, "no parent IRQs ha been found, GPIO IRQs will not be available\n");
			gpio_irq_chip_set_chip(&lgpio->chip.gc.irq, NULL);
		}
	}

	return devm_gpiochip_add_data(&pdev->dev, &lgpio->chip.gc, lgpio);
}

static int loongson_gpio_probe(struct platform_device *pdev)
{
	void __iomem *reg_base;
	struct loongson_gpio_chip *lgpio;
	struct device *dev = &pdev->dev;
	struct reset_control *rst;

	lgpio = devm_kzalloc(dev, sizeof(*lgpio), GFP_KERNEL);
	if (!lgpio)
		return -ENOMEM;

	lgpio->chip_data = device_get_match_data(dev);

	reg_base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(reg_base))
		return PTR_ERR(reg_base);

	rst = devm_reset_control_get_optional_exclusive_deasserted(&pdev->dev, NULL);
	if (IS_ERR(rst))
		return dev_err_probe(&pdev->dev, PTR_ERR(rst), "failed to get reset control\n");

	return loongson_gpio_init(pdev, lgpio, reg_base);
}

static const struct loongson_gpio_chip_data loongson_gpio_ls2k_data = {
	.label = "ls2k_gpio",
	.mode = BIT_CTRL_MODE,
	.conf_offset = 0x0,
	.in_offset = 0x20,
	.out_offset = 0x10,
	.inten_offset = 0x30,
	.intr_num = 6,
	.irq_init = loongson_gpio_init_shared_irqchip,
	.irq_handler = loongson_gpio_shared_irq_handler,
	.girqchip = &loongson_gpio_shared_irqchip,
	.irq_mapping = loongson_gpio_irq_mapping_cap4_2,
};

static const struct loongson_gpio_chip_data loongson_gpio_ls2k0300_data = {
	.label = "ls2k0300_gpio",
	.mode = BYTE_CTRL_MODE,
	.conf_offset = 0x800,
	.in_offset = 0xa00,
	.out_offset = 0x900,
	.inten_offset = 0xb00,
	.intpol_offset = 0xc00,
	.intedge_offset = 0xd00,
	.intclr_offset = 0xe00,
	.intsts_offset = 0xf00,
	.intdual_offset = 0xf80,
	.intr_num = 7,
	.irq_handler = loongson_gpio_ls2k0300_irq_handler,
	.girqchip = &loongson_gpio_ls2k0300_irqchip,
	.irq_init = loongson_gpio_init_irqchip,
};

static const struct loongson_gpio_chip_data loongson_gpio_ls2k0500_data0 = {
	.label = "ls2k0500_gpio",
	.mode = BIT_CTRL_MODE,
	.conf_offset = 0x0,
	.in_offset = 0x8,
	.out_offset = 0x10,
	.inten_offset = 0xb0,
	.intr_num = 2,
	.irq_init = loongson_gpio_init_shared_irqchip,
	.irq_handler = loongson_gpio_shared_irq_handler,
	.girqchip = &loongson_gpio_shared_irqchip,
	.irq_mapping = loongson_gpio_irq_mapping_div32,
};

static const struct loongson_gpio_chip_data loongson_gpio_ls2k0500_data1 = {
	.label = "ls2k0500_gpio",
	.mode = BIT_CTRL_MODE,
	.conf_offset = 0x0,
	.in_offset = 0x8,
	.out_offset = 0x10,
	.inten_offset = 0x98,
	.intr_num = 2,
	.irq_init = loongson_gpio_init_shared_irqchip,
	.irq_handler = loongson_gpio_shared_irq_handler,
	.girqchip = &loongson_gpio_shared_irqchip,
	.irq_mapping = loongson_gpio_irq_mapping_div32,
};

static const struct loongson_gpio_chip_data loongson_gpio_ls2k2000_data0 = {
	.label = "ls2k2000_gpio",
	.mode = BIT_CTRL_MODE,
	.conf_offset = 0x0,
	.in_offset = 0xc,
	.out_offset = 0x8,
	.inten_offset = 0x14,
	.intpol_offset = 0x10,
	.intr_num = 8,
	.irq_init = loongson_gpio_init_shared_irqchip,
	.irq_handler = loongson_gpio_shared_irq_handler,
	.girqchip = &loongson_gpio_shared_irqchip,
	.irq_mapping = loongson_gpio_irq_mapping_mod8,
};

static const struct loongson_gpio_chip_data loongson_gpio_ls2k2000_data1 = {
	.label = "ls2k2000_gpio",
	.mode = BYTE_CTRL_MODE,
	.conf_offset = 0x800,
	.in_offset = 0xa00,
	.out_offset = 0x900,
	.inten_offset = 0xb00,
	.intpol_offset = 0xc00,
	.intedge_offset = 0xd00,
	.intclr_offset = 0xe00,
	.intsts_offset = 0xf00,
	.intr_num = 8,
	.irq_handler = loongson_gpio_ls2k0300_irq_handler,
	.girqchip = &loongson_gpio_ls2k0300_irqchip,
	.irq_init = loongson_gpio_init_irqchip,
};

static const struct loongson_gpio_chip_data loongson_gpio_ls2k2000_data2 = {
	.label = "ls2k2000_gpio",
	.mode = BIT_CTRL_MODE,
	.conf_offset = 0x4,
	.in_offset = 0x8,
	.out_offset = 0x0,
};

static const struct loongson_gpio_chip_data loongson_gpio_ls3a5000_data = {
	.label = "ls3a5000_gpio",
	.mode = BIT_CTRL_MODE,
	.conf_offset = 0x0,
	.in_offset = 0xc,
	.out_offset = 0x8,
	.inten_offset = 0x14,
	.intpol_offset = 0x10,
	.intr_num = 8,
	.irq_init = loongson_gpio_init_shared_irqchip,
	.irq_handler = loongson_gpio_shared_irq_handler,
	.girqchip = &loongson_gpio_shared_irqchip,
	.irq_mapping = loongson_gpio_irq_mapping_mod8,
};

static const struct loongson_gpio_chip_data loongson_gpio_ls7a_data = {
	.label = "ls7a_gpio",
	.mode = BYTE_CTRL_MODE,
	.conf_offset = 0x800,
	.in_offset = 0xa00,
	.out_offset = 0x900,
	.inten_offset = 0xb00,
	.intr_num = 5,
	.irq_init = loongson_gpio_init_shared_irqchip,
	.irq_handler = loongson_gpio_shared_irq_handler,
	.girqchip = &loongson_gpio_shared_irqchip,
	.irq_mapping = loongson_gpio_irq_mapping_cap4,
};

/* LS7A2000 chipset GPIO */
static const struct loongson_gpio_chip_data loongson_gpio_ls7a2000_data0 = {
	.label = "ls7a2000_gpio",
	.mode = BYTE_CTRL_MODE,
	.conf_offset = 0x800,
	.in_offset = 0xa00,
	.out_offset = 0x900,
	.inten_offset = 0xb00,
	.intpol_offset = 0xc00,
	.intedge_offset = 0xd00,
	.intclr_offset = 0xe00,
	.intsts_offset = 0xf00,
	.intr_num = 5,
	.irq_handler = loongson_gpio_ls2k0300_irq_handler,
	.girqchip = &loongson_gpio_ls2k0300_irqchip,
	.irq_init = loongson_gpio_init_irqchip,
};

/* LS7A2000 ACPI GPIO */
static const struct loongson_gpio_chip_data loongson_gpio_ls7a2000_data1 = {
	.label = "ls7a2000_gpio",
	.mode = BIT_CTRL_MODE,
	.conf_offset = 0x4,
	.in_offset = 0x8,
	.out_offset = 0x0,
};

/* Loongson-3A6000 node GPIO */
static const struct loongson_gpio_chip_data loongson_gpio_ls3a6000_data = {
	.label = "ls3a6000_gpio",
	.mode = BIT_CTRL_MODE,
	.conf_offset = 0x0,
	.in_offset = 0xc,
	.out_offset = 0x8,
	.inten_offset = 0x14,
	.intpol_offset = 0x10,
	.intr_num = 8,
	.irq_init = loongson_gpio_init_shared_irqchip,
	.irq_handler = loongson_gpio_shared_irq_handler,
	.girqchip = &loongson_gpio_shared_irqchip,
	.irq_mapping = loongson_gpio_irq_mapping_mod8,
};

static const struct of_device_id loongson_gpio_of_match[] = {
	{
		.compatible = "loongson,ls2k-gpio",
		.data = &loongson_gpio_ls2k_data,
	},
	{
		.compatible = "loongson,ls2k0300-gpio",
		.data = &loongson_gpio_ls2k0300_data,
	},
	{
		.compatible = "loongson,ls2k0500-gpio0",
		.data = &loongson_gpio_ls2k0500_data0,
	},
	{
		.compatible = "loongson,ls2k0500-gpio1",
		.data = &loongson_gpio_ls2k0500_data1,
	},
	{
		.compatible = "loongson,ls2k2000-gpio0",
		.data = &loongson_gpio_ls2k2000_data0,
	},
	{
		.compatible = "loongson,ls2k2000-gpio1",
		.data = &loongson_gpio_ls2k2000_data1,
	},
	{
		.compatible = "loongson,ls2k2000-gpio2",
		.data = &loongson_gpio_ls2k2000_data2,
	},
	{
		.compatible = "loongson,ls3a5000-gpio",
		.data = &loongson_gpio_ls3a5000_data,
	},
	{
		.compatible = "loongson,ls7a-gpio",
		.data = &loongson_gpio_ls7a_data,
	},
	{
		.compatible = "loongson,ls7a2000-gpio1",
		.data = &loongson_gpio_ls7a2000_data0,
	},
	{
		.compatible = "loongson,ls7a2000-gpio2",
		.data = &loongson_gpio_ls7a2000_data1,
	},
	{
		.compatible = "loongson,ls3a6000-gpio",
		.data = &loongson_gpio_ls3a6000_data,
	},
	{}
};
MODULE_DEVICE_TABLE(of, loongson_gpio_of_match);

static const struct acpi_device_id loongson_gpio_acpi_match[] = {
	{
		.id = "LOON0002",
		.driver_data = (kernel_ulong_t)&loongson_gpio_ls7a_data,
	},
	{
		.id = "LOON0007",
		.driver_data = (kernel_ulong_t)&loongson_gpio_ls3a5000_data,
	},
	{
		.id = "LOON000A",
		.driver_data = (kernel_ulong_t)&loongson_gpio_ls2k2000_data0,
	},
	{
		.id = "LOON000B",
		.driver_data = (kernel_ulong_t)&loongson_gpio_ls2k2000_data1,
	},
	{
		.id = "LOON000C",
		.driver_data = (kernel_ulong_t)&loongson_gpio_ls2k2000_data2,
	},
	{
		.id = "LOON000D",
		.driver_data = (kernel_ulong_t)&loongson_gpio_ls7a2000_data0,
	},
	{
		.id = "LOON000E",
		.driver_data = (kernel_ulong_t)&loongson_gpio_ls7a2000_data1,
	},
	{
		.id = "LOON000F",
		.driver_data = (kernel_ulong_t)&loongson_gpio_ls3a6000_data,
	},
	{}
};
MODULE_DEVICE_TABLE(acpi, loongson_gpio_acpi_match);

static struct platform_driver loongson_gpio_driver = {
	.driver = {
		.name = "loongson-gpio",
		.of_match_table = loongson_gpio_of_match,
		.acpi_match_table = loongson_gpio_acpi_match,
	},
	.probe = loongson_gpio_probe,
};

static int __init loongson_gpio_setup(void)
{
	return platform_driver_register(&loongson_gpio_driver);
}
postcore_initcall(loongson_gpio_setup);

MODULE_DESCRIPTION("Loongson gpio driver");
MODULE_LICENSE("GPL");
