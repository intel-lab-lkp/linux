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
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/pm.h>
#include <linux/slab.h>
#include <linux/regmap.h>
#include <linux/mfd/syscon.h>
#include <linux/pinctrl/pinctrl.h>
#include <linux/pinctrl/pinmux.h>
#include <linux/pinctrl/pinconf.h>
#include <linux/pinctrl/pinconf-generic.h>
#include <linux/seq_file.h>

#include "pinconf.h"
#include "pinctrl-ambarella.h"

/* ==========================================================================*/

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
};

struct ambpin_group {
	const char		*name;
	const u32		*pinmux;
	unsigned int		*pins;
	unsigned int		num_pins;
};

struct amb_pinctrl_soc_data {
	struct device			*dev;
	const struct amb_pinctrl_data	*data;
	void __iomem			*iomux_base;
	struct regmap			*ds_regmap;
	struct regmap			*pull_regmap;
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

static bool amb_iomux_accessible(const struct amb_pinctrl_soc_data *soc)
{
	return soc->data->hsm_domain_id == 0;
}

static void amb_iomux_commit(struct amb_pinctrl_soc_data *soc)
{
	if (!amb_iomux_accessible(soc))
		return;

	writel_relaxed(0x1, soc->iomux_base + IOMUX_CTRL_SET_OFFSET);
	writel_relaxed(0x0, soc->iomux_base + IOMUX_CTRL_SET_OFFSET);
}

static void amb_iomux_save_bank(struct amb_pinctrl_soc_data *soc, u32 bank)
{
	if (!amb_iomux_accessible(soc) || bank >= AMBA_MAX_BANKS)
		return;

	soc->pm[bank].iomux[0] =
		readl_relaxed(soc->iomux_base + IOMUX_OFFSET(bank, 0));
	soc->pm[bank].iomux[1] =
		readl_relaxed(soc->iomux_base + IOMUX_OFFSET(bank, 1));
	soc->pm[bank].iomux[2] =
		readl_relaxed(soc->iomux_base + IOMUX_OFFSET(bank, 2));
}

static void amb_iomux_restore_bank(struct amb_pinctrl_soc_data *soc, u32 bank)
{
	if (!amb_iomux_accessible(soc) || bank >= AMBA_MAX_BANKS)
		return;

	writel_relaxed(soc->pm[bank].iomux[0],
		       soc->iomux_base + IOMUX_OFFSET(bank, 0));
	writel_relaxed(soc->pm[bank].iomux[1],
		       soc->iomux_base + IOMUX_OFFSET(bank, 1));
	writel_relaxed(soc->pm[bank].iomux[2],
		       soc->iomux_base + IOMUX_OFFSET(bank, 2));
}

static void amb_pinmux_set_altfunc(struct amb_pinctrl_soc_data *soc,
				   u32 bank, u32 offset, u32 altfunc)
{
	u32 i, data;

	if (!amb_iomux_accessible(soc))
		return;

	for (i = 0; i < 3; i++) {
		data = readl_relaxed(soc->iomux_base + IOMUX_OFFSET(bank, i));
		data &= (~(0x1 << offset));
		data |= (((altfunc >> i) & 0x1) << offset);
		writel_relaxed(data, soc->iomux_base + IOMUX_OFFSET(bank, i));
	}

	amb_iomux_commit(soc);
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
	u32 i, bank, offset;
	unsigned long config;
	enum pin_config_param param;
	u32 arg;
	int ds;

	bank = PINID_TO_BANK(pin);
	if (bank >= soc->data->nr_banks)
		return -EINVAL;

	offset = PINID_TO_OFFSET(pin);

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
			if (ds < 0)
				return ds;
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
			return -EOPNOTSUPP;
		}
	}

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
	enum pin_config_param param = pinconf_to_config_param(*config);
	u32 bank, offset, pull_en, pull_dir, ds0, ds1, ds2, ds;
	int ret, strength;

	bank = PINID_TO_BANK(pin);
	if (bank >= soc->data->nr_banks)
		return -EINVAL;

	offset = PINID_TO_OFFSET(pin);

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
	u32 pull_en, pull_dir, ds0, ds1, ds2, ds;
	u32 bank, offset;
	int strength;

	bank = PINID_TO_BANK(pin);
	if (bank >= soc->data->nr_banks) {
		seq_puts(s, " (no pinconf)");
		return;
	}

	offset = PINID_TO_OFFSET(pin);

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

static int amb_pinctrl_probe(struct platform_device *pdev)
{
	struct amb_pinctrl_soc_data *soc;
	struct device_node *np;
	unsigned int *group_pins;
	unsigned int group, group_pin;
	size_t nr_group_pins = 0;
	int rval;

	soc = devm_kzalloc(&pdev->dev, sizeof(*soc), GFP_KERNEL);
	if (!soc)
		return -ENOMEM;

	soc->dev = &pdev->dev;
	soc->data = of_device_get_match_data(&pdev->dev);
	if (!soc->data)
		return dev_err_probe(&pdev->dev, -EINVAL, "missing soc data");
	if (!soc->data->nr_banks ||
	    soc->data->nr_banks > AMBA_MAX_BANKS ||
	    !soc->data->npins ||
	    soc->data->npins > AMBA_MAX_PINS ||
	    soc->data->nr_banks * 32 > soc->data->npins ||
	    soc->data->clk_au_dedicated_pin >= soc->data->npins ||
	    PINID_TO_BANK(soc->data->clk_au_dedicated_pin) >= AMBA_MAX_BANKS)
		return dev_err_probe(&pdev->dev, -EINVAL,
				     "invalid bank or pin count\n");

	soc->npins = soc->data->npins;

	np = pdev->dev.of_node;
	soc->iomux_base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(soc->iomux_base))
		return dev_err_probe(&pdev->dev, PTR_ERR(soc->iomux_base),
							 "couldn't get iomux reg");

	soc->ds_regmap = syscon_regmap_lookup_by_phandle(np, "ambarella,drive-strength-syscon");
	if (IS_ERR(soc->ds_regmap))
		return dev_err_probe(&pdev->dev, PTR_ERR(soc->ds_regmap),
				     "couldn't get drive-strength regmap");

	soc->pull_regmap = syscon_regmap_lookup_by_phandle(np, "ambarella,pull-syscon");
	if (IS_ERR(soc->pull_regmap))
		return dev_err_probe(&pdev->dev, PTR_ERR(soc->pull_regmap),
				     "couldn't get pull regmap");

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
	bitmap_clear(soc->used, 0, soc->data->nr_banks * 32);
	clear_bit(soc->data->clk_au_dedicated_pin, soc->used);

	rval = amb_pinctrl_register(soc);
	if (rval)
		return dev_err_probe(&pdev->dev, rval, "pinctrl register failed!");

	platform_set_drvdata(pdev, soc);
	dev_info(&pdev->dev, "Ambarella pinctrl driver registered");

	return 0;
}

static int amb_pinctrl_suspend(struct device *dev)
{
	struct amb_pinctrl_soc_data *soc = dev_get_drvdata(dev);
	u32 bank, dedicated = soc->data->clk_au_dedicated_pin;

	for (bank = 0; bank < soc->data->nr_banks; bank++) {
		regmap_read(soc->pull_regmap, soc->data->pull_en[bank],
			    &soc->pm[bank].pull[0]);
		regmap_read(soc->pull_regmap, soc->data->pull_dir[bank],
			    &soc->pm[bank].pull[1]);

		regmap_read(soc->ds_regmap, soc->data->ds0[bank],
			    &soc->pm[bank].ds[0]);
		regmap_read(soc->ds_regmap, soc->data->ds1[bank],
			    &soc->pm[bank].ds[1]);
		if (soc->data->have_ds2)
			regmap_read(soc->ds_regmap, soc->data->ds2[bank],
				    &soc->pm[bank].ds[2]);

		amb_iomux_save_bank(soc, bank);
	}

	if (dedicated >= soc->data->nr_banks * 32)
		amb_iomux_save_bank(soc, PINID_TO_BANK(dedicated));

	return 0;
}

static int amb_pinctrl_resume(struct device *dev)
{
	struct amb_pinctrl_soc_data *soc = dev_get_drvdata(dev);
	u32 bank, dedicated = soc->data->clk_au_dedicated_pin;

	for (bank = 0; bank < soc->data->nr_banks; bank++)
		amb_iomux_restore_bank(soc, bank);

	if (dedicated >= soc->data->nr_banks * 32)
		amb_iomux_restore_bank(soc, PINID_TO_BANK(dedicated));

	wmb();
	amb_iomux_commit(soc);

	for (bank = 0; bank < soc->data->nr_banks; bank++) {
		regmap_write(soc->ds_regmap, soc->data->ds0[bank],
			     soc->pm[bank].ds[0]);
		regmap_write(soc->ds_regmap, soc->data->ds1[bank],
			     soc->pm[bank].ds[1]);
		if (soc->data->have_ds2)
			regmap_write(soc->ds_regmap, soc->data->ds2[bank],
				     soc->pm[bank].ds[2]);

		regmap_write(soc->pull_regmap, soc->data->pull_dir[bank],
			     soc->pm[bank].pull[1]);
		regmap_write(soc->pull_regmap, soc->data->pull_en[bank],
			     soc->pm[bank].pull[0]);
	}

	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(amb_pinctrl_pm_ops,
				amb_pinctrl_suspend,
				amb_pinctrl_resume);

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
