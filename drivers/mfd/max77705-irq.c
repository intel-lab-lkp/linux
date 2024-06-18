// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * max77705-irq.c - Interrupt controller support for MAX77705
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * Copyright (C) 2024 Dzmitry Sankouski <dsankouski@gmail.com>
 */

#include <linux/err.h>
#include <linux/irq.h>
#include <linux/interrupt.h>
#include <linux/gpio.h>
#include <linux/platform_device.h>
#include <linux/mfd/max77705.h>
#include <linux/mfd/max77705-private.h>
#include <linux/regmap.h>

static const u8 max77705_mask_reg[] = {
	[SYS_INT] = MAX77705_PMIC_REG_SYSTEM_INT_MASK,
	[CHG_INT] = MAX77705_CHG_REG_INT_MASK,
	[FUEL_INT] = MAX77705_REG_INVALID,
};

static struct regmap *get_i2c(struct max77705_dev *max77705,
				enum max77705_irq_source src)
{
	switch (src) {
	case SYS_INT:
		return max77705->regmap;
	case FUEL_INT:
		return max77705->regmap_fg;
	case CHG_INT:
		return max77705->regmap_charger;
	default:
		return ERR_PTR(-EINVAL);
	}
}

struct max77705_irq_data {
	int mask;
	enum max77705_irq_source group;
};

static const struct max77705_irq_data max77705_irqs[] = {
	[MAX77705_SYSTEM_IRQ_BSTEN_INT] = { .group = SYS_INT, .mask = BIT(3) },
	[MAX77705_SYSTEM_IRQ_SYSUVLO_INT] = { .group = SYS_INT, .mask = BIT(4) },
	[MAX77705_SYSTEM_IRQ_SYSOVLO_INT] = { .group = SYS_INT, .mask = BIT(5) },
	[MAX77705_SYSTEM_IRQ_TSHDN_INT] = { .group = SYS_INT, .mask = BIT(6) },
	[MAX77705_SYSTEM_IRQ_TM_INT] = { .group = SYS_INT, .mask = BIT(7) },

	[MAX77705_CHG_IRQ_BYP_I] = { .group = CHG_INT, .mask = BIT(0) },
	[MAX77705_CHG_IRQ_BAT_I] = { .group = CHG_INT, .mask = BIT(3) },
	[MAX77705_CHG_IRQ_CHG_I] = { .group = CHG_INT, .mask = BIT(4) },
	[MAX77705_CHG_IRQ_WCIN_I] = { .group = CHG_INT, .mask = BIT(5) },
	[MAX77705_CHG_IRQ_CHGIN_I] = { .group = CHG_INT, .mask = BIT(6) },
	[MAX77705_CHG_IRQ_AICL_I] = { .group = CHG_INT, .mask = BIT(7) },

	[MAX77705_FG_IRQ_ALERT] = { .group = FUEL_INT, .mask = BIT(1) },
};

static void max77705_irq_lock(struct irq_data *data)
{
	struct max77705_dev *max77705 = irq_get_chip_data(data->irq);

	mutex_lock(&max77705->irqlock);
}

static void max77705_irq_sync_unlock(struct irq_data *data)
{
	struct max77705_dev *max77705 = irq_get_chip_data(data->irq);
	int i;

	for (i = 0; i < MAX77705_IRQ_GROUP_NR; i++) {
		u8 mask_reg = max77705_mask_reg[i];
		struct regmap *i2c = get_i2c(max77705, i);

		if (mask_reg == MAX77705_REG_INVALID ||
				IS_ERR_OR_NULL(i2c))
			continue;
		max77705->irq_masks_cache[i] = max77705->irq_masks_cur[i];

		regmap_write(i2c, max77705_mask_reg[i],
				max77705->irq_masks_cur[i]);
	}

	mutex_unlock(&max77705->irqlock);
}

static inline void max77705_read_irq_reg(struct regmap *regmap, unsigned int pmic_rev,
					unsigned int reg, unsigned int *irq_src) {
	u8 dummy[2] = {0, }; /* for pass1 intr reg clear issue */

	switch (pmic_rev) {
	case MAX77705_PASS1:
		regmap_noinc_read(regmap, reg - 1,
				dummy, sizeof(dummy));
		*irq_src = (unsigned int) dummy[1];
		break;
	case MAX77705_PASS2:
	case MAX77705_PASS3:
		regmap_read(regmap, reg,
				irq_src);
		break;
	default:
		pr_err("%s: PMIC_REVISION(SRC_CHG) isn't valid\n", __func__);
		break;
	}
}

static inline const struct max77705_irq_data *
irq_to_max77705_irq(struct max77705_dev *max77705, int irq)
{
	return &max77705_irqs[irq - max77705->irq_base];
}

static void max77705_irq_mask(struct irq_data *data)
{
	struct max77705_dev *max77705 = irq_get_chip_data(data->irq);
	const struct max77705_irq_data *irq_data =
	    irq_to_max77705_irq(max77705, data->irq);

	if (irq_data->group >= MAX77705_IRQ_GROUP_NR)
		return;

	max77705->irq_masks_cur[irq_data->group] |= irq_data->mask;
}

static void max77705_irq_unmask(struct irq_data *data)
{
	struct max77705_dev *max77705 = irq_get_chip_data(data->irq);
	const struct max77705_irq_data *irq_data =
	    irq_to_max77705_irq(max77705, data->irq);

	if (irq_data->group >= MAX77705_IRQ_GROUP_NR)
		return;

	max77705->irq_masks_cur[irq_data->group] &= ~irq_data->mask;
}

inline int max77705_irq_mask_subdevice(struct max77705_dev *max77705, unsigned int mask)
{
	int ret;
	unsigned int data;

	ret = regmap_read(max77705->regmap, MAX77705_PMIC_REG_INTSRC_MASK,
			  &data);
	if (ret) {
		dev_err(max77705->dev, "fail to read MAX77705_PMIC_REG_INTSRC_MASK reg\n");
		return ret;
	}
	data |= mask;

	regmap_write(max77705->regmap, MAX77705_PMIC_REG_INTSRC_MASK,
			   data);
	return 0;
}
EXPORT_SYMBOL_GPL(max77705_irq_mask_subdevice);

inline int max77705_irq_unmask_subdevice(struct max77705_dev *max77705, unsigned int mask)
{
	int ret;
	unsigned int data;

	ret = regmap_read(max77705->regmap, MAX77705_PMIC_REG_INTSRC_MASK,
			  &data);
	if (ret) {
		dev_err(max77705->dev, "fail to read MAX77705_PMIC_REG_INTSRC_MASK reg\n");
		return ret;
	}
	data &= ~(mask);

	regmap_write(max77705->regmap, MAX77705_PMIC_REG_INTSRC_MASK,
			   data);
	return 0;
}
EXPORT_SYMBOL_GPL(max77705_irq_unmask_subdevice);

static void max77705_irq_disable(struct irq_data *data)
{
	max77705_irq_mask(data);
}

static struct irq_chip max77705_irq_chip = {
	.name			= MFD_DEV_NAME,
	.irq_bus_lock		= max77705_irq_lock,
	.irq_bus_sync_unlock	= max77705_irq_sync_unlock,
	.irq_mask		= max77705_irq_mask,
	.irq_unmask		= max77705_irq_unmask,
	.irq_disable            = max77705_irq_disable,
};

static irqreturn_t max77705_irq_thread(int irq, void *data)
{
	struct max77705_dev *max77705 = data;
	unsigned int irq_reg[MAX77705_IRQ_GROUP_NR] = {0};
	unsigned int irq_src;
	int i, ret;
	u8 pmic_rev = max77705->pmic_rev;

	max77705->doing_irq = 1;

	ret = regmap_read(max77705->regmap,
					MAX77705_PMIC_REG_INTSRC, &irq_src);
	if (ret) {
		pr_err("%s:%s Failed to read interrupt source: %d\n",
			MFD_DEV_NAME, __func__, ret);

		max77705->doing_irq = 0;
		return IRQ_NONE;
	}

	if (irq_src & MAX77705_IRQSRC_CHG) {
		max77705_read_irq_reg(max77705->regmap_charger, pmic_rev,
					MAX77705_CHG_REG_INT, &irq_reg[CHG_INT]);
		pr_info("%s: charger interrupt(0x%02x)\n",
				__func__, irq_reg[CHG_INT]);
	}

	/* Apply masking */
	for (i = 0; i < MAX77705_IRQ_GROUP_NR; i++)
		irq_reg[i] &= ~max77705->irq_masks_cur[i];

	/* Report */
	for (i = 0; i < MAX77705_IRQ_NR; i++) {
		if (irq_reg[max77705_irqs[i].group] & max77705_irqs[i].mask)
			handle_nested_irq(max77705->irq_base + i);
	}

	max77705->doing_irq = 0;

	return IRQ_HANDLED;
}

int max77705_irq_init(struct max77705_dev *max77705)
{
	int i;
	int ret = 0;
	int cur_irq;

	if (!max77705->irq_base) {
		dev_err(max77705->dev, "No interrupt base specified.\n");
		return 0;
	}

	mutex_init(&max77705->irqlock);

	/* Mask individual interrupt sources */
	for (i = 0; i < MAX77705_IRQ_GROUP_NR; i++) {
		struct regmap *i2c;
		/* MUIC IRQ  0:MASK 1:NOT MASK => NOT USE */
		/* Other IRQ 1:MASK 0:NOT MASK */
		max77705->irq_masks_cur[i] = 0xff;
		max77705->irq_masks_cache[i] = 0xff;

		i2c = get_i2c(max77705, i);

		if (IS_ERR_OR_NULL(i2c))
			continue;
		if (max77705_mask_reg[i] == MAX77705_REG_INVALID)
			continue;
		regmap_write(i2c, max77705_mask_reg[i], 0xff);
	}

	/* Register with genirq */
	for (i = 0; i < MAX77705_IRQ_NR; i++) {
		cur_irq = i + max77705->irq_base;
		irq_set_chip_data(cur_irq, max77705);
		irq_set_chip_and_handler(cur_irq, &max77705_irq_chip,
					 handle_level_irq);
		irq_set_nested_thread(cur_irq, 1);
#ifdef CONFIG_ARM
		set_irq_flags(cur_irq, IRQF_VALID);
#else
		irq_set_noprobe(cur_irq);
#endif
	}

	ret = max77705_irq_mask_subdevice(max77705, MAX77705_IRQSRC_CHG | MAX77705_IRQSRC_TOP |
					  MAX77705_IRQSRC_FG | MAX77705_IRQSRC_USBC);
	if (ret) {
		dev_err(max77705->dev, "Failed to mask subdevice irqs\n");
		return ret;
	}

	ret = devm_request_threaded_irq(max77705->dev, max77705->irq, NULL, max77705_irq_thread,
					IRQF_TRIGGER_LOW | IRQF_ONESHOT,
					"max77705-irq", max77705);
	if (ret) {
		dev_err(max77705->dev, "Failed to request IRQ %d: %d\n",
				max77705->irq, ret);
	}

	return ret;
}

MODULE_LICENSE("GPL");
