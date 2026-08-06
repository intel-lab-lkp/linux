// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Pinctrl driver for Ambarella SoCs
 *
 * History:
 *	2013/12/18 - [Cao Rongrong] created file
 *
 * Copyright (C) 2012-2026, Ambarella, Inc.
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/bitmap.h>
#include <linux/io.h>
#include <linux/interrupt.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/of_irq.h>
#include <linux/pm.h>
#include <linux/slab.h>
#include <linux/regmap.h>
#include <linux/mfd/syscon.h>
#include <linux/irqdomain.h>
#include <linux/irqchip/chained_irq.h>
#include <linux/gpio/driver.h>
#include <linux/pinctrl/pinctrl.h>
#include <linux/pinctrl/pinmux.h>
#include <linux/pinctrl/pinconf.h>
#include <linux/pinctrl/pinconf-generic.h>
#include <linux/seq_file.h>

#include "pinconf.h"
#include "pinctrl-ambarella.h"

/* ==========================================================================*/

#define GPIO_DATA_OFFSET		0x00
#define GPIO_DIR_OFFSET			0x04
#define GPIO_IS_OFFSET			0x08
#define GPIO_IBE_OFFSET			0x0c
#define GPIO_IEV_OFFSET			0x10
#define GPIO_IE_OFFSET			0x14
#define GPIO_AFSEL_OFFSET		0x18
#define GPIO_RIS_OFFSET			0x1c
#define GPIO_MIS_OFFSET			0x20
#define GPIO_IC_OFFSET			0x24
#define GPIO_MASK_OFFSET		0x28
#define GPIO_ENABLE_OFFSET		0x2c

#define IOMUX_OFFSET(bank, n)		(((bank) * 0xc) + ((n) * 4))
#define IOMUX_CTRL_SET_OFFSET		0xf0

/* ==========================================================================*/

#define AMBA_MAX_PINS			(AMBA_MAX_BANKS * 32)

#define PINID_TO_BANK(p)		((p) >> 5)
#define PINID_TO_OFFSET(p)		((p) & 0x1f)

struct amb_pinctrl_pm_state {
	u32 iomux[3];
	u32 pull[2];
	u32 ds[3];
	u32 data;
	u32 dir;
	u32 is;
	u32 ibe;
	u32 iev;
	u32 ie;
	u32 afsel;
	u32 mask;
};

struct ambpin_group {
	const char		*name;
	const u32		*pinmux;
	unsigned int		*pins;
	unsigned int		num_pins;
};

struct amb_pinctrl_soc_data;

struct amb_gpio_bank {
	struct amb_pinctrl_soc_data	*soc;
	void __iomem			*base;
	struct gpio_chip		gc;
	struct irq_domain		*domain;
	unsigned int			pin_base;
	unsigned int			hw_id;
	unsigned int			irq_wake_mask;
	int				irq;
};

struct amb_pinctrl_soc_data {
	struct device			*dev;
	const struct amb_pinctrl_data	*data;
	struct amb_gpio_bank		banks[AMBA_MAX_BANKS];
	void __iomem			*iomux_base;
	struct regmap			*ds_regmap;
	struct regmap			*pull_regmap;
	unsigned int			bank_num;
	unsigned int			npins;
	unsigned long			used[BITS_TO_LONGS(AMBA_MAX_PINS)];
	raw_spinlock_t lock;

	struct pinctrl_dev		*pctl;

	const struct ambpin_function	*functions;
	unsigned int			nr_functions;
	struct ambpin_group		*groups;
	unsigned int			nr_groups;

	struct amb_pinctrl_pm_state	pm[AMBA_MAX_BANKS];
};

static struct amb_gpio_bank *amb_irq_data_to_bank(struct irq_data *data)
{
	struct gpio_chip *gc = irq_data_get_irq_chip_data(data);

	return gpiochip_get_data(gc);
}

static struct amb_gpio_bank *
amb_pin_to_bank(struct amb_pinctrl_soc_data *soc, unsigned int pin)
{
	unsigned int i;

	for (i = 0; i < soc->bank_num; i++) {
		struct amb_gpio_bank *bank = &soc->banks[i];

		if (pin >= bank->pin_base &&
		    pin < bank->pin_base + bank->gc.ngpio)
			return bank;
	}

	return NULL;
}

/* Exclusive end of the GPIO pin number space (max pin_base + ngpio). */
static unsigned int amb_gpio_pins_end(const struct amb_pinctrl_soc_data *soc)
{
	unsigned int end = 0, i;

	for (i = 0; i < soc->bank_num; i++)
		end = max(end, soc->banks[i].pin_base + soc->banks[i].gc.ngpio);

	return end;
}

/* check if the selector is a valid pin group selector */
static int amb_get_group_count(struct pinctrl_dev *pctldev)
{
	struct amb_pinctrl_soc_data *soc = pinctrl_dev_get_drvdata(pctldev);

	return soc->nr_groups;
}

/* return the name of the group selected by the group selector */
static const char *amb_get_group_name(struct pinctrl_dev *pctldev,
				      unsigned int selector)
{
	struct amb_pinctrl_soc_data *soc = pinctrl_dev_get_drvdata(pctldev);

	return soc->groups[selector].name;
}

/* return the pin numbers associated with the specified group */
static int amb_get_group_pins(struct pinctrl_dev *pctldev,
			      unsigned int selector, const unsigned int **pins,
			      unsigned int *num_pins)
{
	struct amb_pinctrl_soc_data *soc = pinctrl_dev_get_drvdata(pctldev);

	*pins = soc->groups[selector].pins;
	*num_pins = soc->groups[selector].num_pins;

	return 0;
}

#if IS_ENABLED(CONFIG_DEBUG_FS)
static void amb_pin_dbg_show(struct pinctrl_dev *pctldev,
			     struct seq_file *s, unsigned int pin)
{
	seq_printf(s, " %s", pinctrl_dev_get_devname(pctldev));
}
#endif

/* list of pinctrl callbacks for the pinctrl core */
static const struct pinctrl_ops amb_pctrl_ops = {
	.get_groups_count	= amb_get_group_count,
	.get_group_name		= amb_get_group_name,
	.get_group_pins		= amb_get_group_pins,
#if IS_ENABLED(CONFIG_DEBUG_FS)
	.pin_dbg_show		= amb_pin_dbg_show,
#endif
	.dt_node_to_map		= pinconf_generic_dt_node_to_map_all,
	.dt_free_map		= pinconf_generic_dt_free_map,
};

/* check if the selector is a valid pin function selector */
static int amb_pinmux_request(struct pinctrl_dev *pctldev, unsigned int pin)
{
	struct amb_pinctrl_soc_data *soc = pinctrl_dev_get_drvdata(pctldev);

	if (test_and_set_bit(pin, soc->used))
		return -EBUSY;

	return 0;
}

/* check if the selector is a valid pin function selector */
static int amb_pinmux_free(struct pinctrl_dev *pctldev, unsigned int pin)
{
	struct amb_pinctrl_soc_data *soc = pinctrl_dev_get_drvdata(pctldev);

	clear_bit(pin, soc->used);

	return 0;
}

/* check if the selector is a valid pin function selector */
static int amb_pinmux_get_fcount(struct pinctrl_dev *pctldev)
{
	struct amb_pinctrl_soc_data *soc = pinctrl_dev_get_drvdata(pctldev);

	return soc->nr_functions;
}

/* return the name of the pin function specified */
static const char *amb_pinmux_get_fname(struct pinctrl_dev *pctldev,
					unsigned int selector)
{
	struct amb_pinctrl_soc_data *soc = pinctrl_dev_get_drvdata(pctldev);

	return soc->functions[selector].name;
}

/* return the groups associated for the specified function selector */
static int amb_pinmux_get_groups(struct pinctrl_dev *pctldev,
				 unsigned int selector,
				 const char * const **groups,
				 unsigned int * const num_groups)
{
	struct amb_pinctrl_soc_data *soc = pinctrl_dev_get_drvdata(pctldev);

	*groups = soc->functions[selector].groups;
	*num_groups = soc->functions[selector].num_groups;

	return 0;
}

static void amb_pinmux_set_altfunc(struct amb_pinctrl_soc_data *soc,
				   u32 bank, u32 offset, u32 altfunc)
{
	u32 i, data;

	/* On CV3 platform, only ARM cluster0 (safety domain) can access pinctrl registers */
	if (soc->data->hsm_domain_id != 0)
		return;

	for (i = 0; i < 3; i++) {
		data = readl_relaxed(soc->iomux_base + IOMUX_OFFSET(bank, i));
		data &= (~(0x1 << offset));
		data |= (((altfunc >> i) & 0x1) << offset);
		writel_relaxed(data, soc->iomux_base + IOMUX_OFFSET(bank, i));
	}

	writel_relaxed(0x1, soc->iomux_base + IOMUX_CTRL_SET_OFFSET);
	writel_relaxed(0x0, soc->iomux_base + IOMUX_CTRL_SET_OFFSET);
}

/* enable a specified pinmux by writing to registers */
static int amb_pinmux_set_mux(struct pinctrl_dev *pctldev,
			      unsigned int selector, unsigned int group)
{
	struct amb_pinctrl_soc_data *soc = pinctrl_dev_get_drvdata(pctldev);
	const struct ambpin_group *grp;
	u32 i, pin, alt, bank, offset;
	unsigned long flags;

	grp = &soc->groups[group];

	raw_spin_lock_irqsave(&soc->lock, flags);
	for (i = 0; i < grp->num_pins; i++) {
		pin = AMBA_PINMUX_TO_PIN(grp->pinmux[i]);
		alt = AMBA_PINMUX_TO_ALT(grp->pinmux[i]);
		bank = PINID_TO_BANK(pin);
		offset = PINID_TO_OFFSET(pin);
		amb_pinmux_set_altfunc(soc, bank, offset, alt);
	}
	raw_spin_unlock_irqrestore(&soc->lock, flags);

	return 0;
}

static int amb_pinmux_gpio_request_enable(struct pinctrl_dev *pctldev,
					  struct pinctrl_gpio_range *range,
					  unsigned int pin)
{
	struct amb_pinctrl_soc_data *soc = pinctrl_dev_get_drvdata(pctldev);
	u32 bank, offset;
	unsigned long flags;

	if (!range || !range->gc) {
		dev_err(soc->dev, "invalid range: %p\n", range);
		return -EINVAL;
	}

	if (test_and_set_bit(pin, soc->used))
		return -EBUSY;

	bank = PINID_TO_BANK(pin);
	offset = PINID_TO_OFFSET(pin);

	raw_spin_lock_irqsave(&soc->lock, flags);
	amb_pinmux_set_altfunc(soc, bank, offset, 0);
	raw_spin_unlock_irqrestore(&soc->lock, flags);

	return 0;
}

static void amb_pinmux_gpio_disable_free(struct pinctrl_dev *pctldev,
					 struct pinctrl_gpio_range *range,
					 unsigned int pin)
{
	struct amb_pinctrl_soc_data *soc = pinctrl_dev_get_drvdata(pctldev);

	dev_dbg(soc->dev, "disable pin %u as GPIO\n", pin);
	/* Set the pin to some default state, GPIO is usually default */

	clear_bit(pin, soc->used);
}

/* list of pinmux callbacks for the pinmux vertical in pinctrl core */
static const struct pinmux_ops amb_pinmux_ops = {
	.request		= amb_pinmux_request,
	.free			= amb_pinmux_free,
	.get_functions_count	= amb_pinmux_get_fcount,
	.get_function_name	= amb_pinmux_get_fname,
	.get_function_groups	= amb_pinmux_get_groups,
	.set_mux                = amb_pinmux_set_mux,
	.gpio_request_enable	= amb_pinmux_gpio_request_enable,
	.gpio_disable_free	= amb_pinmux_gpio_disable_free,
};

static int amb_drive_strength_to_reg(struct amb_pinctrl_soc_data *soc,
				     u32 strength)
{
	if (soc->data->have_ds2) {
		switch (strength) {
		case 3:
			return 0;
		case 4:
		case 5:
			return 1;
		case 6:
			return 2;
		case 7:
		case 8:
			return 3;
		case 9:
			return 4;
		case 12:
			return 5;
		default:
			return -EINVAL;
		}
	}

	switch (strength) {
	case 2:
		return 0;
	case 4:
		return 1;
	case 8:
		return 2;
	case 12:
		return 3;
	default:
		return -EINVAL;
	}
}

static int amb_reg_to_drive_strength(struct amb_pinctrl_soc_data *soc, u32 ds)
{
	static const int ds2_ma[] = { 3, 4, 6, 8, 9, 12 };
	static const int ds_ma[] = { 2, 4, 8, 12 };

	if (soc->data->have_ds2) {
		if (ds >= ARRAY_SIZE(ds2_ma))
			return -EINVAL;

		return ds2_ma[ds];
	}

	if (ds >= ARRAY_SIZE(ds_ma))
		return -EINVAL;

	return ds_ma[ds];
}

/* set the pin config settings for a specified pin */
static int amb_pinconf_set(struct pinctrl_dev *pctldev, unsigned int pin,
			   unsigned long *configs, unsigned int num_configs)
{
	struct amb_pinctrl_soc_data *soc = pinctrl_dev_get_drvdata(pctldev);
	struct amb_gpio_bank *gpio_bank;
	u32 i, bank, offset;
	unsigned long config, flags;
	enum pin_config_param param;
	u32 arg;
	int ds;

	gpio_bank = amb_pin_to_bank(soc, pin);
	if (!gpio_bank)
		return -EINVAL;

	bank = gpio_bank->hw_id;
	offset = pin - gpio_bank->pin_base;

	raw_spin_lock_irqsave(&soc->lock, flags);
	for (i = 0; i < num_configs; i++) {
		config = configs[i];
		param = pinconf_to_config_param(config);
		arg = pinconf_to_config_argument(config);

		switch (param) {
		case PIN_CONFIG_BIAS_DISABLE:
			regmap_update_bits(soc->pull_regmap,
					   soc->data->pull_en[bank], BIT(offset), 0);
			break;
		case PIN_CONFIG_BIAS_PULL_DOWN:
		case PIN_CONFIG_BIAS_PULL_UP:
			regmap_update_bits(soc->pull_regmap, soc->data->pull_dir[bank],
					   BIT(offset),
					   (param == PIN_CONFIG_BIAS_PULL_UP) ?
					   BIT(offset) : 0);
			regmap_update_bits(soc->pull_regmap, soc->data->pull_en[bank],
					   BIT(offset), BIT(offset));
			break;
		case PIN_CONFIG_DRIVE_STRENGTH:
			ds = amb_drive_strength_to_reg(soc, arg);
			if (ds < 0) {
				raw_spin_unlock_irqrestore(&soc->lock, flags);
				return ds;
			}
			if (soc->data->have_ds2) {
				regmap_update_bits(soc->ds_regmap,
						   soc->data->ds0[bank], BIT(offset),
						   (ds & BIT(0)) ? BIT(offset) : 0);
				regmap_update_bits(soc->ds_regmap,
						   soc->data->ds1[bank], BIT(offset),
						   (ds & BIT(1)) ? BIT(offset) : 0);
				regmap_update_bits(soc->ds_regmap,
						   soc->data->ds2[bank], BIT(offset),
						   (ds & BIT(2)) ? BIT(offset) : 0);
			} else {
				regmap_update_bits(soc->ds_regmap,
						   soc->data->ds0[bank], BIT(offset),
						   (ds & BIT(1)) ? BIT(offset) : 0);
				regmap_update_bits(soc->ds_regmap,
						   soc->data->ds1[bank], BIT(offset),
						   (ds & BIT(0)) ? BIT(offset) : 0);
			}
			break;
		default:
			raw_spin_unlock_irqrestore(&soc->lock, flags);
			return -EOPNOTSUPP;
		}
	}
	raw_spin_unlock_irqrestore(&soc->lock, flags);

	return 0;
}

static int amb_pinconf_group_set(struct pinctrl_dev *pctldev,
				 unsigned int selector,
				 unsigned long *configs,
				 unsigned int num_configs)
{
	struct amb_pinctrl_soc_data *soc = pinctrl_dev_get_drvdata(pctldev);
	const struct ambpin_group *grp = &soc->groups[selector];
	int ret;
	u32 i;

	for (i = 0; i < grp->num_pins; i++) {
		ret = amb_pinconf_set(pctldev, grp->pins[i], configs,
				      num_configs);
		if (ret)
			return ret;
	}

	return 0;
}

/* get the pin config settings for a specified pin */
static int amb_pinconf_get(struct pinctrl_dev *pctldev,
			   unsigned int pin, unsigned long *config)
{
	struct amb_pinctrl_soc_data *soc = pinctrl_dev_get_drvdata(pctldev);
	struct amb_gpio_bank *gpio_bank;
	enum pin_config_param param = pinconf_to_config_param(*config);
	u32 bank, offset, pull_en, pull_dir, ds0, ds1, ds2, ds;
	int ret, strength;

	gpio_bank = amb_pin_to_bank(soc, pin);
	if (!gpio_bank)
		return -EINVAL;

	bank = gpio_bank->hw_id;
	offset = pin - gpio_bank->pin_base;

	switch (param) {
	case PIN_CONFIG_BIAS_DISABLE:
	case PIN_CONFIG_BIAS_PULL_DOWN:
	case PIN_CONFIG_BIAS_PULL_UP:
		ret = regmap_read(soc->pull_regmap, soc->data->pull_en[bank],
				  &pull_en);
		if (ret)
			return ret;

		ret = regmap_read(soc->pull_regmap, soc->data->pull_dir[bank],
				  &pull_dir);
		if (ret)
			return ret;

		pull_en = (pull_en >> offset) & 1;
		pull_dir = (pull_dir >> offset) & 1;

		if (param == PIN_CONFIG_BIAS_DISABLE) {
			if (pull_en)
				return -EINVAL;
			*config = pinconf_to_config_packed(param, 0);
			return 0;
		}

		if (!pull_en)
			return -EINVAL;
		if (param == PIN_CONFIG_BIAS_PULL_UP && !pull_dir)
			return -EINVAL;
		if (param == PIN_CONFIG_BIAS_PULL_DOWN && pull_dir)
			return -EINVAL;

		*config = pinconf_to_config_packed(param, 1);
		return 0;

	case PIN_CONFIG_DRIVE_STRENGTH:
		ret = regmap_read(soc->ds_regmap, soc->data->ds0[bank], &ds0);
		if (ret)
			return ret;

		ret = regmap_read(soc->ds_regmap, soc->data->ds1[bank], &ds1);
		if (ret)
			return ret;

		ds0 = (ds0 >> offset) & 1;
		ds1 = (ds1 >> offset) & 1;
		if (soc->data->have_ds2) {
			ret = regmap_read(soc->ds_regmap, soc->data->ds2[bank], &ds2);
			if (ret)
				return ret;

			ds2 = (ds2 >> offset) & 1;
			ds = (ds2 << 2) | (ds1 << 1) | ds0;
		} else {
			ds = (ds0 << 1) | ds1;
		}

		strength = amb_reg_to_drive_strength(soc, ds);
		if (strength < 0)
			return strength;

		*config = pinconf_to_config_packed(param, strength);
		return 0;

	default:
		return -EOPNOTSUPP;
	}
}

#if IS_ENABLED(CONFIG_DEBUG_FS)
static void amb_pinconf_dbg_show(struct pinctrl_dev *pctldev,
				 struct seq_file *s, unsigned int pin)
{
	struct amb_pinctrl_soc_data *soc = pinctrl_dev_get_drvdata(pctldev);
	struct amb_gpio_bank *gpio_bank;
	u32 pull_en, pull_dir, ds0, ds1, ds2, ds;
	u32 bank, offset;
	int strength;

	gpio_bank = amb_pin_to_bank(soc, pin);
	if (!gpio_bank) {
		seq_puts(s, " (no pinconf)");
		return;
	}

	bank = gpio_bank->hw_id;
	offset = pin - gpio_bank->pin_base;

	regmap_read(soc->pull_regmap, soc->data->pull_en[bank], &pull_en);
	pull_en = (pull_en >> offset) & 1;
	regmap_read(soc->pull_regmap, soc->data->pull_dir[bank], &pull_dir);
	pull_dir = (pull_dir >> offset) & 1;
	seq_printf(s, " pull: %s,",
		   pull_en ? (pull_dir ? "up" : "down") : "disable");

	regmap_read(soc->ds_regmap, soc->data->ds0[bank], &ds0);
	ds0 = (ds0 >> offset) & 1;
	regmap_read(soc->ds_regmap, soc->data->ds1[bank], &ds1);
	ds1 = (ds1 >> offset) & 1;
	if (soc->data->have_ds2) {
		regmap_read(soc->ds_regmap, soc->data->ds2[bank], &ds2);
		ds2 = (ds2 >> offset) & 1;
		ds = (ds2 << 2) | (ds1 << 1) | ds0;
	} else {
		ds = (ds0 << 1) | ds1;
	}

	strength = amb_reg_to_drive_strength(soc, ds);
	if (strength < 0)
		seq_puts(s, " drive-strength: invalid");
	else
		seq_printf(s, " drive-strength: %dmA", strength);
}
#endif

/* list of pinconfig callbacks for pinconfig vertical in the pinctrl code */
static const struct pinconf_ops amb_pinconf_ops = {
	.is_generic		= true,
	.pin_config_get		= amb_pinconf_get,
	.pin_config_set		= amb_pinconf_set,
	.pin_config_group_set	= amb_pinconf_group_set,
#if IS_ENABLED(CONFIG_DEBUG_FS)
	.pin_config_dbg_show	= amb_pinconf_dbg_show,
#endif
};

/* register the pinctrl interface with the pinctrl subsystem */
static int amb_pinctrl_register(struct amb_pinctrl_soc_data *soc)
{
	struct pinctrl_pin_desc *pindesc;
	struct pinctrl_desc *amb_pinctrl_desc;
	unsigned int pin;

	/* dynamically populate the pin number and pin name for pindesc */
	pindesc = devm_kcalloc(soc->dev, soc->npins, sizeof(*pindesc),
			       GFP_KERNEL);
	if (!pindesc)
		return -ENOMEM;

	for (pin = 0; pin < soc->npins; pin++) {
		pindesc[pin].number = pin;
		pindesc[pin].name = devm_kasprintf(soc->dev, GFP_KERNEL,
						   "io%u", pin);
		if (!pindesc[pin].name)
			return -ENOMEM;
	}

	amb_pinctrl_desc = devm_kzalloc(soc->dev, sizeof(*amb_pinctrl_desc), GFP_KERNEL);
	if (!amb_pinctrl_desc)
		return -ENOMEM;

	amb_pinctrl_desc->name = dev_name(soc->dev);
	amb_pinctrl_desc->pins = pindesc;
	amb_pinctrl_desc->npins = soc->npins;
	amb_pinctrl_desc->pctlops = &amb_pctrl_ops;
	amb_pinctrl_desc->pmxops = &amb_pinmux_ops;
	amb_pinctrl_desc->confops = &amb_pinconf_ops;
	amb_pinctrl_desc->owner = THIS_MODULE;

	soc->pctl = devm_pinctrl_register(soc->dev, amb_pinctrl_desc, soc);
	if (IS_ERR(soc->pctl)) {
		dev_err(soc->dev, "could not register pinctrl driver\n");
		return PTR_ERR(soc->pctl);
	}

	return 0;
}

/* gpiolib gpio_set callback function */
static int amb_gpio_set(struct gpio_chip *gc, unsigned int pin, int value)
{
	struct amb_gpio_bank *bank = gpiochip_get_data(gc);
	struct amb_pinctrl_soc_data *soc = bank->soc;
	u32 data;
	unsigned long flags;

	raw_spin_lock_irqsave(&soc->lock, flags);
	writel_relaxed(BIT(pin), bank->base + GPIO_MASK_OFFSET);
	data = value ? BIT(pin) : 0;
	writel_relaxed(data, bank->base + GPIO_DATA_OFFSET);
	raw_spin_unlock_irqrestore(&soc->lock, flags);

	return 0;
}

/* gpiolib gpio_get callback function */
static int amb_gpio_get(struct gpio_chip *gc, unsigned int pin)
{
	struct amb_gpio_bank *bank = gpiochip_get_data(gc);
	struct amb_pinctrl_soc_data *soc = bank->soc;
	u32 data;
	unsigned long flags;

	raw_spin_lock_irqsave(&soc->lock, flags);
	writel_relaxed(BIT(pin), bank->base + GPIO_MASK_OFFSET);
	data = readl_relaxed(bank->base + GPIO_DATA_OFFSET);
	raw_spin_unlock_irqrestore(&soc->lock, flags);

	return !!(data & BIT(pin));
}

static int amb_gpio_get_direction(struct gpio_chip *gc, unsigned int pin)
{
	struct amb_gpio_bank *bank = gpiochip_get_data(gc);
	struct amb_pinctrl_soc_data *soc = bank->soc;
	u32 data;
	unsigned long flags;

	raw_spin_lock_irqsave(&soc->lock, flags);
	data = readl_relaxed(bank->base + GPIO_DIR_OFFSET);
	raw_spin_unlock_irqrestore(&soc->lock, flags);

	return data & BIT(pin) ? GPIO_LINE_DIRECTION_OUT : GPIO_LINE_DIRECTION_IN;
}

static int amb_gpio_set_direction(struct gpio_chip *gc, unsigned int pin, bool input)
{
	struct amb_gpio_bank *bank = gpiochip_get_data(gc);
	struct amb_pinctrl_soc_data *soc = bank->soc;
	u32 data;
	unsigned long flags;

	raw_spin_lock_irqsave(&soc->lock, flags);
	data = readl_relaxed(bank->base + GPIO_DIR_OFFSET);
	if (input)
		data &= ~BIT(pin);
	else
		data |= BIT(pin);
	writel_relaxed(data, bank->base + GPIO_DIR_OFFSET);
	raw_spin_unlock_irqrestore(&soc->lock, flags);

	return 0;
}

/* gpiolib gpio_direction_input callback function */
static int amb_gpio_direction_input(struct gpio_chip *gc, unsigned int pin)
{
	return amb_gpio_set_direction(gc, pin, true);
}

/* gpiolib gpio_direction_output callback function */
static int amb_gpio_direction_output(struct gpio_chip *gc, unsigned int pin, int value)
{
	struct amb_gpio_bank *bank = gpiochip_get_data(gc);
	struct amb_pinctrl_soc_data *soc = bank->soc;
	unsigned long flags;
	u32 data;

	raw_spin_lock_irqsave(&soc->lock, flags);

	writel_relaxed(BIT(pin), bank->base + GPIO_MASK_OFFSET);
	writel_relaxed(value ? BIT(pin) : 0,
		       bank->base + GPIO_DATA_OFFSET);

	data = readl_relaxed(bank->base + GPIO_DIR_OFFSET);
	writel_relaxed(data | BIT(pin), bank->base + GPIO_DIR_OFFSET);

	raw_spin_unlock_irqrestore(&soc->lock, flags);

	return 0;
}

/* gpiolib gpio_to_irq callback function */
static int amb_gpio_to_irq(struct gpio_chip *gc, unsigned int pin)
{
	struct amb_gpio_bank *bank = gpiochip_get_data(gc);

	return irq_create_mapping(bank->domain, pin);
}

#if IS_ENABLED(CONFIG_DEBUG_FS)
static void amb_gpio_dbg_show(struct seq_file *s, struct gpio_chip *gc)
{
	struct amb_gpio_bank *bank = gpiochip_get_data(gc);
	struct amb_pinctrl_soc_data *soc = bank->soc;
	u32 afsel, data, dir, mask, iomux0, iomux1, iomux2, alt;
	unsigned long flags;
	u32 i;

	raw_spin_lock_irqsave(&soc->lock, flags);
	afsel = readl_relaxed(bank->base + GPIO_AFSEL_OFFSET);
	dir = readl_relaxed(bank->base + GPIO_DIR_OFFSET);
	mask = readl_relaxed(bank->base + GPIO_MASK_OFFSET);
	writel_relaxed(0xffffffff, bank->base + GPIO_MASK_OFFSET);
	data = readl_relaxed(bank->base + GPIO_DATA_OFFSET);
	writel_relaxed(mask, bank->base + GPIO_MASK_OFFSET);

	iomux0 = readl_relaxed(soc->iomux_base + IOMUX_OFFSET(bank->hw_id, 0));
	iomux1 = readl_relaxed(soc->iomux_base + IOMUX_OFFSET(bank->hw_id, 1));
	iomux2 = readl_relaxed(soc->iomux_base + IOMUX_OFFSET(bank->hw_id, 2));
	raw_spin_unlock_irqrestore(&soc->lock, flags);

	seq_printf(s, "\nGPIO[%d]:\n", bank->hw_id);
	seq_printf(s, "GPIO_AFSEL:\t0x%08X\n", afsel);
	seq_printf(s, "GPIO_DIR:\t0x%08X\n", dir);
	seq_printf(s, "GPIO_MASK:\t0x%08X\n", mask);
	seq_printf(s, "GPIO_DATA:\t0x%08X\n", data);
	seq_printf(s, "IOMUX_REG%d_0:\t0x%08X\n", bank->hw_id, iomux0);
	seq_printf(s, "IOMUX_REG%d_1:\t0x%08X\n", bank->hw_id, iomux1);
	seq_printf(s, "IOMUX_REG%d_2:\t0x%08X\n", bank->hw_id, iomux2);

	for (i = 0; i < gc->ngpio; i++) {

		seq_printf(s, " gpio-%-3d", gc->base + i);

		alt = ((iomux2 >> i) & 1) << 2;
		alt |= ((iomux1 >> i) & 1) << 1;
		alt |= ((iomux0 >> i) & 1) << 0;
		if (alt) {
			seq_printf(s, " [HW  ] (alt%d)\n", alt);
		} else {
			char *label __free(kfree) = gpiochip_dup_line_label(gc, i);
			if (IS_ERR(label)) {
				pr_debug("Failed to duplicate label\n");
				continue;
			}

			seq_printf(s, " [GPIO] (%-20.20s) %s %s\n",
				   label ? label : "",
				   (dir & BIT(i)) ? "out" : "in ",
				   (data & BIT(i)) ? "hi" : "lo");
		}
	}
}
#endif

static void amb_gpio_irq_enable(struct irq_data *data)
{
	struct gpio_chip *gc = irq_data_get_irq_chip_data(data);
	struct amb_gpio_bank *bank = amb_irq_data_to_bank(data);
	struct amb_pinctrl_soc_data *soc = bank->soc;
	void __iomem *gpio_base = bank->base;
	void __iomem *iomux_base = soc->iomux_base;
	u32 i, val, offset;
	unsigned long flags;

	offset = irqd_to_hwirq(data);

	gpiochip_enable_irq(gc, offset);

	raw_spin_lock_irqsave(&soc->lock, flags);

	val = readl_relaxed(gpio_base + GPIO_DIR_OFFSET);
	val &= ~(0x1 << offset);
	writel_relaxed(val, gpio_base + GPIO_DIR_OFFSET);

	for (i = 0; i < 3; i++) {
		val = readl_relaxed(iomux_base + IOMUX_OFFSET(bank->hw_id, i));
		val &= ~(0x1 << offset);
		writel_relaxed(val, iomux_base + IOMUX_OFFSET(bank->hw_id, i));
	}
	writel_relaxed(0x1, iomux_base + IOMUX_CTRL_SET_OFFSET);
	writel_relaxed(0x0, iomux_base + IOMUX_CTRL_SET_OFFSET);

	writel_relaxed(0x1 << offset, gpio_base + GPIO_IC_OFFSET);

	val = readl_relaxed(gpio_base + GPIO_IE_OFFSET);
	val |= 0x1 << offset;
	writel_relaxed(val, gpio_base + GPIO_IE_OFFSET);

	raw_spin_unlock_irqrestore(&soc->lock, flags);
}

static void amb_gpio_irq_disable(struct irq_data *data)
{
	struct gpio_chip *gc = irq_data_get_irq_chip_data(data);
	struct amb_gpio_bank *bank = amb_irq_data_to_bank(data);
	struct amb_pinctrl_soc_data *soc = bank->soc;
	void __iomem *gpio_base = bank->base;
	u32 offset, ie;
	unsigned long flags;

	offset = irqd_to_hwirq(data);

	raw_spin_lock_irqsave(&soc->lock, flags);
	ie = readl_relaxed(gpio_base + GPIO_IE_OFFSET);
	writel_relaxed(ie & ~(0x1 << offset), gpio_base + GPIO_IE_OFFSET);
	writel_relaxed(0x1 << offset, gpio_base + GPIO_IC_OFFSET);
	raw_spin_unlock_irqrestore(&soc->lock, flags);

	gpiochip_disable_irq(gc, offset);
}

static void amb_gpio_irq_ack(struct irq_data *data)
{
	struct amb_gpio_bank *bank = amb_irq_data_to_bank(data);
	struct amb_pinctrl_soc_data *soc = bank->soc;
	void __iomem *gpio_base = bank->base;
	u32 offset = irqd_to_hwirq(data);
	unsigned long flags;

	raw_spin_lock_irqsave(&soc->lock, flags);
	writel_relaxed(0x1 << offset, gpio_base + GPIO_IC_OFFSET);
	raw_spin_unlock_irqrestore(&soc->lock, flags);
}

static void amb_gpio_irq_mask(struct irq_data *data)
{
	struct amb_gpio_bank *bank = amb_irq_data_to_bank(data);
	struct amb_pinctrl_soc_data *soc = bank->soc;
	void __iomem *gpio_base = bank->base;
	u32 offset, ie;
	unsigned long flags;

	offset = irqd_to_hwirq(data);

	raw_spin_lock_irqsave(&soc->lock, flags);
	ie = readl_relaxed(gpio_base + GPIO_IE_OFFSET);
	writel_relaxed(ie & ~(0x1 << offset), gpio_base + GPIO_IE_OFFSET);
	raw_spin_unlock_irqrestore(&soc->lock, flags);
}

static void amb_gpio_irq_mask_ack(struct irq_data *data)
{
	struct amb_gpio_bank *bank = amb_irq_data_to_bank(data);
	struct amb_pinctrl_soc_data *soc = bank->soc;
	void __iomem *gpio_base = bank->base;
	u32 offset, ie;
	unsigned long flags;

	offset = irqd_to_hwirq(data);

	raw_spin_lock_irqsave(&soc->lock, flags);
	ie = readl_relaxed(gpio_base + GPIO_IE_OFFSET);
	writel_relaxed(ie & ~(0x1 << offset), gpio_base + GPIO_IE_OFFSET);
	writel_relaxed(0x1 << offset, gpio_base + GPIO_IC_OFFSET);
	raw_spin_unlock_irqrestore(&soc->lock, flags);
}

static void amb_gpio_irq_unmask(struct irq_data *data)
{
	struct amb_gpio_bank *bank = amb_irq_data_to_bank(data);
	struct amb_pinctrl_soc_data *soc = bank->soc;
	void __iomem *gpio_base = bank->base;
	u32 offset, ie;
	unsigned long flags;

	offset = irqd_to_hwirq(data);

	raw_spin_lock_irqsave(&soc->lock, flags);
	ie = readl_relaxed(gpio_base + GPIO_IE_OFFSET);
	writel_relaxed(ie | (0x1 << offset), gpio_base + GPIO_IE_OFFSET);
	raw_spin_unlock_irqrestore(&soc->lock, flags);
}

static int amb_gpio_irq_set_type(struct irq_data *data, unsigned int type)
{
	struct amb_gpio_bank *bank = amb_irq_data_to_bank(data);
	struct amb_pinctrl_soc_data *soc = bank->soc;
	void __iomem *gpio_base = bank->base;
	u32 offset = irqd_to_hwirq(data);
	u32 mask, bit, sense, bothedges, event;
	unsigned long flags;

	mask = ~BIT(offset);
	bit = BIT(offset);

	raw_spin_lock_irqsave(&soc->lock, flags);
	sense = readl_relaxed(gpio_base + GPIO_IS_OFFSET);
	bothedges = readl_relaxed(gpio_base + GPIO_IBE_OFFSET);
	event = readl_relaxed(gpio_base + GPIO_IEV_OFFSET);

	switch (type) {
	case IRQ_TYPE_EDGE_RISING:
		sense &= mask;
		bothedges &= mask;
		event |= bit;
		irq_set_handler_locked(data, handle_edge_irq);
		break;
	case IRQ_TYPE_EDGE_FALLING:
		sense &= mask;
		bothedges &= mask;
		event &= mask;
		irq_set_handler_locked(data, handle_edge_irq);
		break;
	case IRQ_TYPE_EDGE_BOTH:
		sense &= mask;
		bothedges |= bit;
		event &= mask;
		irq_set_handler_locked(data, handle_edge_irq);
		break;
	case IRQ_TYPE_LEVEL_HIGH:
		sense |= bit;
		bothedges &= mask;
		event |= bit;
		irq_set_handler_locked(data, handle_level_irq);
		break;
	case IRQ_TYPE_LEVEL_LOW:
		sense |= bit;
		bothedges &= mask;
		event &= mask;
		irq_set_handler_locked(data, handle_level_irq);
		break;
	default:
		raw_spin_unlock_irqrestore(&soc->lock, flags);
		return -EINVAL;
	}

	writel_relaxed(sense, gpio_base + GPIO_IS_OFFSET);
	writel_relaxed(bothedges, gpio_base + GPIO_IBE_OFFSET);
	writel_relaxed(event, gpio_base + GPIO_IEV_OFFSET);
	/* clear obsolete irq */
	writel_relaxed(BIT(offset), gpio_base + GPIO_IC_OFFSET);
	raw_spin_unlock_irqrestore(&soc->lock, flags);

	return 0;
}

static int amb_gpio_irq_set_wake(struct irq_data *data, unsigned int on)
{
	if (IS_ENABLED(CONFIG_PM)) {
		struct amb_gpio_bank *bank = amb_irq_data_to_bank(data);
		struct amb_pinctrl_soc_data *soc = bank->soc;
		u32 offset = irqd_to_hwirq(data);
		unsigned long flags;
		int ret;

		ret = irq_set_irq_wake(bank->irq, on);
		if (ret)
			return ret;

		raw_spin_lock_irqsave(&soc->lock, flags);
		if (on)
			bank->irq_wake_mask |= BIT(offset);
		else
			bank->irq_wake_mask &= ~BIT(offset);
		raw_spin_unlock_irqrestore(&soc->lock, flags);

		return 0;
	}
	return 0;
}

static struct irq_chip amb_gpio_irqchip = {
	.name		= "GPIO",
	.irq_enable	= amb_gpio_irq_enable,
	.irq_disable	= amb_gpio_irq_disable,
	.irq_ack	= amb_gpio_irq_ack,
	.irq_mask	= amb_gpio_irq_mask,
	.irq_mask_ack	= amb_gpio_irq_mask_ack,
	.irq_unmask	= amb_gpio_irq_unmask,
	.irq_set_type	= amb_gpio_irq_set_type,
	.irq_set_wake	= amb_gpio_irq_set_wake,
	.irq_request_resources = gpiochip_irq_reqres,
	.irq_release_resources = gpiochip_irq_relres,
	.flags		= IRQCHIP_SET_TYPE_MASKED | IRQCHIP_MASK_ON_SUSPEND,
};

static int amb_gpio_irqdomain_map(struct irq_domain *d, unsigned int irq,
				  irq_hw_number_t hwirq)
{
	struct amb_gpio_bank *bank = d->host_data;

	irq_set_chip_data(irq, &bank->gc);
	irq_set_chip_and_handler(irq, &amb_gpio_irqchip, handle_level_irq);
	irq_set_noprobe(irq);

	return 0;
}

static const struct irq_domain_ops amb_gpio_irq_domain_ops = {
	.map = amb_gpio_irqdomain_map,
	.xlate = irq_domain_xlate_twocell,
};

static void amb_gpio_handle_irq(struct irq_desc *desc)
{
	struct amb_gpio_bank *bank;
	struct irq_chip *irqchip;
	u32 bit, gpio_mis;

	irqchip = irq_desc_get_chip(desc);
	chained_irq_enter(irqchip, desc);

	bank = irq_desc_get_handler_data(desc);
	gpio_mis = readl_relaxed(bank->base + GPIO_MIS_OFFSET);
	while (gpio_mis) {
		bit = __ffs(gpio_mis);
		generic_handle_domain_irq(bank->domain, bit);
		gpio_mis &= ~BIT(bit);
	}

	chained_irq_exit(irqchip, desc);
}

static int amb_gpio_parse_dt(struct amb_pinctrl_soc_data *soc)
{
	struct of_phandle_args args;
	struct resource res;
	unsigned int hw_id, i;
	int rval;

	for_each_available_child_of_node_scoped(soc->dev->of_node, np) {
		struct amb_gpio_bank *bank;

		if (!of_property_present(np, "gpio-controller"))
			continue;

		if (soc->bank_num >= AMBA_MAX_BANKS)
			return dev_err_probe(soc->dev, -EINVAL,
					     "too many GPIO banks\n");

		rval = of_parse_phandle_with_fixed_args(np, "gpio-ranges", 3, 0,
						       &args);
		if (rval)
			return dev_err_probe(soc->dev, rval,
					     "%pOF: invalid gpio-ranges\n", np);

		if (args.np != soc->dev->of_node || args.args[0] ||
		    args.args[1] % 32 || !args.args[2] || args.args[2] > 32) {
			of_node_put(args.np);
			return dev_err_probe(soc->dev, -EINVAL,
					     "%pOF: invalid GPIO pin range\n", np);
		}

		hw_id = args.args[1] / 32;
		of_node_put(args.np);
		if (hw_id >= AMBA_MAX_BANKS)
			return dev_err_probe(soc->dev, -EINVAL,
					     "%pOF: invalid GPIO bank\n", np);

		for (i = 0; i < soc->bank_num; i++) {
			if (soc->banks[i].hw_id == hw_id)
				return dev_err_probe(soc->dev, -EINVAL,
						     "%pOF: duplicate GPIO bank\n",
						     np);
		}

		bank = &soc->banks[soc->bank_num];
		bank->soc = soc;
		bank->pin_base = args.args[1];
		bank->hw_id = hw_id;
		bank->gc.ngpio = args.args[2];
		bank->gc.fwnode = of_fwnode_handle(np);

		rval = of_address_to_resource(np, 0, &res);
		if (rval)
			return dev_err_probe(soc->dev, rval,
					     "%pOF: couldn't get registers\n", np);

		bank->base = devm_ioremap_resource(soc->dev, &res);
		if (IS_ERR(bank->base))
			return dev_err_probe(soc->dev, PTR_ERR(bank->base),
					     "%pOF: couldn't map registers\n", np);

		bank->irq = of_irq_get(np, 0);
		if (bank->irq < 0)
			return dev_err_probe(soc->dev, bank->irq,
					     "%pOF: couldn't get interrupt\n", np);

		soc->bank_num++;
	}

	if (!soc->bank_num)
		return dev_err_probe(soc->dev, -ENODEV,
				     "no gpio-controller child nodes\n");

	return 0;
}

static void amb_gpio_irq_cleanup(void *data)
{
	struct amb_gpio_bank *bank = data;

	irq_set_chained_handler_and_data(bank->irq, NULL, NULL);
	irq_domain_remove(bank->domain);
	bank->domain = NULL;
}

static int amb_gpio_register(struct amb_pinctrl_soc_data *soc)
{
	struct amb_gpio_bank *bank;
	unsigned int i;
	int rval;

	for (i = 0; i < soc->bank_num; i++) {
		bank = &soc->banks[i];
		bank->gc.label = devm_kasprintf(soc->dev, GFP_KERNEL,
					       "%s-gpio%u", dev_name(soc->dev),
					       bank->hw_id);
		if (!bank->gc.label)
			return -ENOMEM;

		bank->gc.base = -1;
		bank->gc.request = gpiochip_generic_request;
		bank->gc.free = gpiochip_generic_free;
		bank->gc.direction_input = amb_gpio_direction_input;
		bank->gc.direction_output = amb_gpio_direction_output;
		bank->gc.get_direction = amb_gpio_get_direction;
		bank->gc.get = amb_gpio_get;
		bank->gc.set = amb_gpio_set;
		bank->gc.to_irq = amb_gpio_to_irq;
#if IS_ENABLED(CONFIG_DEBUG_FS)
		bank->gc.dbg_show = amb_gpio_dbg_show;
#endif
		bank->gc.parent = soc->dev;

		bank->domain = irq_domain_add_linear(to_of_node(bank->gc.fwnode),
						    bank->gc.ngpio,
						    &amb_gpio_irq_domain_ops,
						    bank);
		if (!bank->domain)
			return dev_err_probe(soc->dev, -ENODEV,
					     "GPIO%u: failed to create irqdomain\n",
					     i);

		rval = devm_gpiochip_add_data(soc->dev, &bank->gc, bank);
		if (rval) {
			irq_domain_remove(bank->domain);
			bank->domain = NULL;
			return dev_err_probe(soc->dev, rval,
					     "GPIO%u: gpiochip registration failed\n",
					     i);
		}

		writel_relaxed(0xffffffff, bank->base + GPIO_ENABLE_OFFSET);
		writel_relaxed(0x00000000, bank->base + GPIO_AFSEL_OFFSET);
		writel_relaxed(0x00000000, bank->base + GPIO_MASK_OFFSET);

		irq_set_irq_type(bank->irq, IRQ_TYPE_LEVEL_HIGH);
		irq_set_chained_handler_and_data(bank->irq,
						 amb_gpio_handle_irq, bank);

		rval = devm_add_action_or_reset(soc->dev,
					       amb_gpio_irq_cleanup, bank);
		if (rval)
			return rval;
	}

	return 0;
}

static int amb_pinctrl_probe(struct platform_device *pdev)
{
	struct amb_pinctrl_soc_data *soc;
	struct device_node *np;
	unsigned int *group_pins;
	unsigned int gpio_pins, group, group_pin;
	size_t nr_group_pins = 0;
	int i, rval;

	soc = devm_kzalloc(&pdev->dev, sizeof(*soc), GFP_KERNEL);
	if (!soc)
		return -ENOMEM;

	soc->dev = &pdev->dev;
	soc->data = of_device_get_match_data(&pdev->dev);
	if (!soc->data)
		return dev_err_probe(&pdev->dev, -EINVAL, "missing soc data");

	np = pdev->dev.of_node;
	soc->iomux_base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(soc->iomux_base))
		return dev_err_probe(&pdev->dev, PTR_ERR(soc->iomux_base),
							 "couldn't get iomux reg");

	rval = amb_gpio_parse_dt(soc);
	if (rval)
		return rval;

	soc->ds_regmap = syscon_regmap_lookup_by_phandle(np, "ambarella,drive-strength-syscon");
	if (IS_ERR(soc->ds_regmap))
		return dev_err_probe(&pdev->dev, PTR_ERR(soc->ds_regmap),
				     "couldn't get drive-strength regmap");

	soc->pull_regmap = syscon_regmap_lookup_by_phandle(np, "ambarella,pull-syscon");
	if (IS_ERR(soc->pull_regmap))
		return dev_err_probe(&pdev->dev, PTR_ERR(soc->pull_regmap),
				     "couldn't get pull regmap");

	gpio_pins = amb_gpio_pins_end(soc);
	soc->npins = gpio_pins;
	if (soc->data->clk_au_dedicated_pin >= gpio_pins)
		soc->npins = soc->data->clk_au_dedicated_pin + 1;

	if (soc->npins > AMBA_MAX_PINS)
		return dev_err_probe(&pdev->dev, -EINVAL, "too many pins\n");

	soc->nr_groups = soc->data->nr_groups;
	soc->functions = soc->data->functions;
	soc->nr_functions = soc->data->nr_functions;
	if (!soc->data->groups || !soc->nr_groups ||
	    !soc->functions || !soc->nr_functions)
		return dev_err_probe(&pdev->dev, -EINVAL,
				     "missing pin groups or functions\n");

	soc->groups = devm_kcalloc(&pdev->dev, soc->nr_groups,
				   sizeof(*soc->groups), GFP_KERNEL);
	if (!soc->groups)
		return -ENOMEM;

	for (group = 0; group < soc->nr_groups; group++) {
		const struct ambpin_group_desc *desc =
			&soc->data->groups[group];

		if (!desc->name || !desc->pinmux || !desc->num_pins)
			return dev_err_probe(&pdev->dev, -EINVAL,
					     "invalid pin group %u\n", group);

		nr_group_pins += desc->num_pins;
	}

	group_pins = devm_kcalloc(&pdev->dev, nr_group_pins,
				  sizeof(*group_pins), GFP_KERNEL);
	if (!group_pins)
		return -ENOMEM;

	for (group = 0; group < soc->nr_groups; group++) {
		const struct ambpin_group_desc *desc =
			&soc->data->groups[group];
		struct ambpin_group *grp = &soc->groups[group];

		grp->name = desc->name;
		grp->pinmux = desc->pinmux;
		grp->pins = group_pins;
		grp->num_pins = desc->num_pins;
		for (group_pin = 0;
		     group_pin < grp->num_pins;
		     group_pin++) {
			u32 pinmux = grp->pinmux[group_pin];
			unsigned int pin = AMBA_PINMUX_TO_PIN(pinmux);
			unsigned int alt = AMBA_PINMUX_TO_ALT(pinmux);

			if (pin >= soc->npins || alt > 7)
				return dev_err_probe(&pdev->dev, -EINVAL,
						     "group %s has invalid pinmux %#x\n",
						     grp->name, pinmux);

			grp->pins[group_pin] = pin;
		}
		group_pins += grp->num_pins;
	}

	raw_spin_lock_init(&soc->lock);

	/* Mark all pins unavailable, then clear pins that exist. */
	bitmap_fill(soc->used, AMBA_MAX_PINS);
	for (i = 0; i < soc->bank_num; i++) {
		unsigned int pin;

		for (pin = soc->banks[i].pin_base;
		     pin < soc->banks[i].pin_base + soc->banks[i].gc.ngpio;
		     pin++)
			clear_bit(pin, soc->used);
	}

	if (soc->data->clk_au_dedicated_pin >= gpio_pins &&
	    soc->data->clk_au_dedicated_pin < AMBA_MAX_PINS)
		clear_bit(soc->data->clk_au_dedicated_pin, soc->used);

	rval = amb_pinctrl_register(soc);
	if (rval)
		return dev_err_probe(&pdev->dev, rval, "pinctrl register failed!");

	rval = amb_gpio_register(soc);
	if (rval)
		return dev_err_probe(&pdev->dev, rval, "gpio register failed!");

	platform_set_drvdata(pdev, soc);
	dev_info(&pdev->dev, "Ambarella pinctrl driver registered");

	return 0;
}

static int amb_pinctrl_suspend_noirq(struct device *dev)
{
	struct amb_pinctrl_soc_data *soc = dev_get_drvdata(dev);
	u32 bank, hw, i;

	for (i = 0; i < soc->bank_num; i++) {
		hw = soc->banks[i].hw_id;

		regmap_read(soc->pull_regmap, soc->data->pull_en[hw], &soc->pm[i].pull[0]);
		regmap_read(soc->pull_regmap, soc->data->pull_dir[hw], &soc->pm[i].pull[1]);

		regmap_read(soc->ds_regmap, soc->data->ds0[hw], &soc->pm[i].ds[0]);
		regmap_read(soc->ds_regmap, soc->data->ds1[hw], &soc->pm[i].ds[1]);
		if (soc->data->have_ds2)
			regmap_read(soc->ds_regmap, soc->data->ds2[hw], &soc->pm[i].ds[2]);

		soc->pm[i].iomux[0] = readl_relaxed(soc->iomux_base + IOMUX_OFFSET(hw, 0));
		soc->pm[i].iomux[1] = readl_relaxed(soc->iomux_base + IOMUX_OFFSET(hw, 1));
		soc->pm[i].iomux[2] = readl_relaxed(soc->iomux_base + IOMUX_OFFSET(hw, 2));

		soc->pm[i].afsel = readl_relaxed(soc->banks[i].base + GPIO_AFSEL_OFFSET);
		soc->pm[i].dir = readl_relaxed(soc->banks[i].base + GPIO_DIR_OFFSET);
		soc->pm[i].is = readl_relaxed(soc->banks[i].base + GPIO_IS_OFFSET);
		soc->pm[i].ibe = readl_relaxed(soc->banks[i].base + GPIO_IBE_OFFSET);
		soc->pm[i].iev = readl_relaxed(soc->banks[i].base + GPIO_IEV_OFFSET);
		soc->pm[i].ie = readl_relaxed(soc->banks[i].base + GPIO_IE_OFFSET);
		soc->pm[i].mask = readl_relaxed(soc->banks[i].base + GPIO_MASK_OFFSET);
		writel_relaxed(0xffffffff, soc->banks[i].base + GPIO_MASK_OFFSET);
		soc->pm[i].data = readl_relaxed(soc->banks[i].base + GPIO_DATA_OFFSET);

		if (soc->banks[i].irq_wake_mask)
			writel_relaxed(soc->banks[i].irq_wake_mask,
				       soc->banks[i].base + GPIO_IE_OFFSET);
	}

	if (soc->data->clk_au_dedicated_pin >= amb_gpio_pins_end(soc)) {
		bank = PINID_TO_BANK(soc->data->clk_au_dedicated_pin);
		soc->pm[bank].iomux[0] =
			readl_relaxed(soc->iomux_base + IOMUX_OFFSET(bank, 0));
		soc->pm[bank].iomux[1] =
			readl_relaxed(soc->iomux_base + IOMUX_OFFSET(bank, 1));
		soc->pm[bank].iomux[2] =
			readl_relaxed(soc->iomux_base + IOMUX_OFFSET(bank, 2));
	}

	return 0;
}

static int amb_pinctrl_resume_noirq(struct device *dev)
{
	struct amb_pinctrl_soc_data *soc = dev_get_drvdata(dev);
	u32 bank, hw, i;

	for (i = 0; i < soc->bank_num; i++) {
		hw = soc->banks[i].hw_id;

		regmap_write(soc->pull_regmap, soc->data->pull_en[hw], soc->pm[i].pull[0]);
		regmap_write(soc->pull_regmap, soc->data->pull_dir[hw], soc->pm[i].pull[1]);

		regmap_write(soc->ds_regmap, soc->data->ds0[hw], soc->pm[i].ds[0]);
		regmap_write(soc->ds_regmap, soc->data->ds1[hw], soc->pm[i].ds[1]);
		if (soc->data->have_ds2)
			regmap_write(soc->ds_regmap, soc->data->ds2[hw], soc->pm[i].ds[2]);

		writel_relaxed(soc->pm[i].iomux[0], soc->iomux_base + IOMUX_OFFSET(hw, 0));
		writel_relaxed(soc->pm[i].iomux[1], soc->iomux_base + IOMUX_OFFSET(hw, 1));
		writel_relaxed(soc->pm[i].iomux[2], soc->iomux_base + IOMUX_OFFSET(hw, 2));

		writel_relaxed(soc->pm[i].afsel, soc->banks[i].base + GPIO_AFSEL_OFFSET);
		writel_relaxed(soc->pm[i].dir, soc->banks[i].base + GPIO_DIR_OFFSET);
		/* Expose DATA writes while restoring the saved GPIO state. */
		writel_relaxed(0xffffffff,
			       soc->banks[i].base + GPIO_MASK_OFFSET);
		writel_relaxed(soc->pm[i].data,
			       soc->banks[i].base + GPIO_DATA_OFFSET);
		/* Ensure DATA restore reaches hardware before restoring mask. */
		wmb();
		writel_relaxed(soc->pm[i].mask, soc->banks[i].base + GPIO_MASK_OFFSET);
		writel_relaxed(soc->pm[i].is, soc->banks[i].base + GPIO_IS_OFFSET);
		writel_relaxed(soc->pm[i].ibe, soc->banks[i].base + GPIO_IBE_OFFSET);
		writel_relaxed(soc->pm[i].iev, soc->banks[i].base + GPIO_IEV_OFFSET);
		writel_relaxed(soc->pm[i].ie, soc->banks[i].base + GPIO_IE_OFFSET);
		writel_relaxed(0xffffffff, soc->banks[i].base + GPIO_ENABLE_OFFSET);
	}

	if (soc->data->clk_au_dedicated_pin >= amb_gpio_pins_end(soc)) {
		bank = PINID_TO_BANK(soc->data->clk_au_dedicated_pin);
		writel_relaxed(soc->pm[bank].iomux[0],
			       soc->iomux_base + IOMUX_OFFSET(bank, 0));
		writel_relaxed(soc->pm[bank].iomux[1],
			       soc->iomux_base + IOMUX_OFFSET(bank, 1));
		writel_relaxed(soc->pm[bank].iomux[2],
			       soc->iomux_base + IOMUX_OFFSET(bank, 2));
		/* Ensure dedicated-pin iomux writes reach hardware before commit. */
		wmb();
	}

	writel_relaxed(0x1, soc->iomux_base + IOMUX_CTRL_SET_OFFSET);
	writel_relaxed(0x0, soc->iomux_base + IOMUX_CTRL_SET_OFFSET);

	return 0;
}

static DEFINE_NOIRQ_DEV_PM_OPS(amb_pinctrl_pm_ops,
			       amb_pinctrl_suspend_noirq,
			       amb_pinctrl_resume_noirq);

static const struct of_device_id amb_pinctrl_dt_match[] = {
	{
		.compatible = "ambarella,cv75-pinctrl",
		.data = &ambarella_cv75_pinctrl_data,
	},
	{},
};
MODULE_DEVICE_TABLE(of, amb_pinctrl_dt_match);

static struct platform_driver amb_pinctrl_driver = {
	.probe	= amb_pinctrl_probe,
	.driver	= {
		.name	= "ambarella-pinctrl",
		.of_match_table = of_match_ptr(amb_pinctrl_dt_match),
		.pm = pm_sleep_ptr(&amb_pinctrl_pm_ops),
	},
};

static int __init amb_pinctrl_drv_register(void)
{
	return platform_driver_register(&amb_pinctrl_driver);
}
arch_initcall(amb_pinctrl_drv_register);

MODULE_AUTHOR("Cao Rongrong <rrcao@ambarella.com>");
MODULE_DESCRIPTION("Ambarella SoC pinctrl driver");
MODULE_LICENSE("GPL");
