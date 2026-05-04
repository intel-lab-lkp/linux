// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Core driver for the S32 CC (Common Chassis) pin controller
 *
 * Copyright 2017-2022,2024-2026 NXP
 * Copyright (C) 2022 SUSE LLC
 * Copyright 2015-2016 Freescale Semiconductor, Inc.
 */

#include <linux/bitops.h>
#include <linux/err.h>
#include <linux/gpio/driver.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pinctrl/machine.h>
#include <linux/pinctrl/pinconf.h>
#include <linux/pinctrl/pinctrl.h>
#include <linux/pinctrl/pinmux.h>
#include <linux/regmap.h>
#include <linux/seq_file.h>
#include <linux/slab.h>

#include "../core.h"
#include "../pinconf.h"
#include "../pinctrl-utils.h"
#include "pinctrl-s32.h"

#define S32_PIN_ID_SHIFT	4
#define S32_PIN_ID_MASK		GENMASK(31, S32_PIN_ID_SHIFT)

#define S32_MSCR_SSS_MASK	GENMASK(2, 0)
#define S32_MSCR_PUS		BIT(12)
#define S32_MSCR_PUE		BIT(13)
#define S32_MSCR_SRE(X)		(((X) & GENMASK(3, 0)) << 14)
#define S32_MSCR_IBE		BIT(19)
#define S32_MSCR_ODE		BIT(20)
#define S32_MSCR_OBE		BIT(21)

/* PGPDOs are 16bit registers that come in big endian
 * order if they are grouped in pairs of two.
 *
 * For example, the order is PGPDO1, PGPDO0, PGPDO3, PGPDO2...
 */
#define S32_PGPD(N)		(((N) ^ 1) * 2)
#define S32_PGPD_SIZE		16

enum s32_write_type {
	S32_PINCONF_UPDATE_ONLY,
	S32_PINCONF_OVERWRITE,
};

static struct regmap_config s32_regmap_config = {
	.reg_bits = 32,
	.val_bits = 32,
	.reg_stride = 4,
};

static u32 get_pin_no(u32 pinmux)
{
	return (pinmux & S32_PIN_ID_MASK) >> S32_PIN_ID_SHIFT;
}

static u32 get_pin_func(u32 pinmux)
{
	return pinmux & GENMASK(3, 0);
}

/**
 * struct s32_pinctrl_mem_region - memory region for a set of SIUL2 registers
 * @map: regmap used for this range
 * @pin_range: the pins controlled by these registers
 * @name: name of the current range
 */
struct s32_pinctrl_mem_region {
	struct regmap *map;
	const struct s32_pin_range *pin_range;
	char name[8];
};

/**
 * struct s32_gpio_regmaps - GPIO register maps for a SIUL2 instance
 * @pgpdo: regmap for Parallel GPIO Pad Data Out registers
 * @pgpdi: regmap for Parallel GPIO Pad Data In registers
 * @range: GPIO range info
 */
struct s32_gpio_regmaps {
	struct regmap *pgpdo;
	struct regmap *pgpdi;
	const struct s32_gpio_range *range;
};

/**
 * struct gpio_pin_config - holds pin configuration for GPIO's
 * @pin_id: Pin ID for this GPIO
 * @config: Pin settings
 * @list: Linked list entry for each gpio pin
 */
struct gpio_pin_config {
	unsigned int pin_id;
	unsigned int config;
	struct list_head list;
};

/**
 * struct s32_pinctrl_context - pad config save/restore for suspend/resume
 * @pads: saved values for the pards
 */
struct s32_pinctrl_context {
	unsigned int *pads;
};

/**
 * struct s32_pinctrl - private driver data
 * @dev: a pointer back to containing device
 * @pctl: a pointer to the pinctrl device structure
 * @gc: a pointer to the gpio_chip
 * @regions: reserved memory regions with start/end pin
 * @info: structure containing information about the pin
 * @gpio_regmaps: PGPDO/PGPDI regmaps for each SIUL2 module
 * @num_gpio_regmaps: number of GPIO regmap entries
 * @gpio_configs: saved configurations for GPIO pins
 * @gpio_configs_lock: lock for the `gpio_configs` list
 * @saved_context: configuration saved over system sleep
 */
struct s32_pinctrl {
	struct device *dev;
	struct pinctrl_dev *pctl;
	struct gpio_chip gc;
	struct s32_pinctrl_mem_region *regions;
	struct s32_pinctrl_soc_info *info;
	struct s32_gpio_regmaps *gpio_regmaps;
	unsigned int num_gpio_regmaps;
	struct list_head gpio_configs;
	spinlock_t gpio_configs_lock;
#ifdef CONFIG_PM_SLEEP
	struct s32_pinctrl_context saved_context;
#endif
};

static struct s32_pinctrl_mem_region *
s32_get_region(struct pinctrl_dev *pctldev, unsigned int pin)
{
	struct s32_pinctrl *ipctl = pinctrl_dev_get_drvdata(pctldev);
	const struct s32_pin_range *pin_range;
	unsigned int mem_regions = ipctl->info->soc_data->mem_regions;
	unsigned int i;

	for (i = 0; i < mem_regions; i++) {
		pin_range = ipctl->regions[i].pin_range;
		if (pin >= pin_range->start && pin <= pin_range->end)
			return &ipctl->regions[i];
	}

	return NULL;
}

static int s32_check_pin(struct pinctrl_dev *pctldev,
			 unsigned int pin)
{
	return s32_get_region(pctldev, pin) ? 0 : -EINVAL;
}

static int s32_regmap_read(struct pinctrl_dev *pctldev,
			   unsigned int pin, unsigned int *val)
{
	struct s32_pinctrl_mem_region *region;
	unsigned int offset;

	region = s32_get_region(pctldev, pin);
	if (!region)
		return -EINVAL;

	offset = (pin - region->pin_range->start) *
			regmap_get_reg_stride(region->map);

	return regmap_read(region->map, offset, val);
}

static int s32_regmap_write(struct pinctrl_dev *pctldev,
			    unsigned int pin,
			    unsigned int val)
{
	struct s32_pinctrl_mem_region *region;
	unsigned int offset;

	region = s32_get_region(pctldev, pin);
	if (!region)
		return -EINVAL;

	offset = (pin - region->pin_range->start) *
			regmap_get_reg_stride(region->map);

	return regmap_write(region->map, offset, val);

}

static int s32_regmap_update(struct pinctrl_dev *pctldev, unsigned int pin,
			     unsigned int mask, unsigned int val)
{
	struct s32_pinctrl_mem_region *region;
	unsigned int offset;

	region = s32_get_region(pctldev, pin);
	if (!region)
		return -EINVAL;

	offset = (pin - region->pin_range->start) *
			regmap_get_reg_stride(region->map);

	return regmap_update_bits(region->map, offset, mask, val);
}

static int s32_get_groups_count(struct pinctrl_dev *pctldev)
{
	struct s32_pinctrl *ipctl = pinctrl_dev_get_drvdata(pctldev);
	const struct s32_pinctrl_soc_info *info = ipctl->info;

	return info->ngroups;
}

static const char *s32_get_group_name(struct pinctrl_dev *pctldev,
				      unsigned int selector)
{
	struct s32_pinctrl *ipctl = pinctrl_dev_get_drvdata(pctldev);
	const struct s32_pinctrl_soc_info *info = ipctl->info;

	return info->groups[selector].data.name;
}

static int s32_get_group_pins(struct pinctrl_dev *pctldev,
			      unsigned int selector, const unsigned int **pins,
			      unsigned int *npins)
{
	struct s32_pinctrl *ipctl = pinctrl_dev_get_drvdata(pctldev);
	const struct s32_pinctrl_soc_info *info = ipctl->info;

	*pins = info->groups[selector].data.pins;
	*npins = info->groups[selector].data.npins;

	return 0;
}

static void s32_pin_dbg_show(struct pinctrl_dev *pctldev, struct seq_file *s,
			     unsigned int offset)
{
	seq_printf(s, "%s", dev_name(pctldev->dev));
}

static int s32_dt_group_node_to_map(struct pinctrl_dev *pctldev,
				    struct device_node *np,
				    struct pinctrl_map **map,
				    unsigned int *reserved_maps,
				    unsigned int *num_maps,
				    const char *func_name)
{
	struct s32_pinctrl *ipctl = pinctrl_dev_get_drvdata(pctldev);
	struct device *dev = ipctl->dev;
	unsigned long *cfgs = NULL;
	unsigned int n_cfgs, reserve = 1;
	int n_pins, ret;

	n_pins = of_property_count_elems_of_size(np, "pinmux", sizeof(u32));
	if (n_pins < 0) {
		dev_warn(dev, "Can't find 'pinmux' property in node %pOFn\n", np);
	} else if (!n_pins) {
		return -EINVAL;
	}

	ret = pinconf_generic_parse_dt_config(np, pctldev, &cfgs, &n_cfgs);
	if (ret)
		return dev_err_probe(dev, ret,
				     "%pOF: could not parse node property\n",
				     np);

	if (n_cfgs)
		reserve++;

	ret = pinctrl_utils_reserve_map(pctldev, map, reserved_maps, num_maps,
					reserve);
	if (ret < 0)
		goto free_cfgs;

	ret = pinctrl_utils_add_map_mux(pctldev, map, reserved_maps, num_maps,
					np->name, func_name);
	if (ret < 0)
		goto free_cfgs;

	if (n_cfgs) {
		ret = pinctrl_utils_add_map_configs(pctldev, map, reserved_maps,
						    num_maps, np->name, cfgs, n_cfgs,
						    PIN_MAP_TYPE_CONFIGS_GROUP);
		if (ret < 0)
			goto free_cfgs;
	}

free_cfgs:
	kfree(cfgs);
	return ret;
}

static int s32_dt_node_to_map(struct pinctrl_dev *pctldev,
			      struct device_node *np_config,
			      struct pinctrl_map **map,
			      unsigned int *num_maps)
{
	unsigned int reserved_maps;
	int ret;

	reserved_maps = 0;
	*map = NULL;
	*num_maps = 0;

	for_each_available_child_of_node_scoped(np_config, np) {
		ret = s32_dt_group_node_to_map(pctldev, np, map,
					       &reserved_maps, num_maps,
					       np_config->name);
		if (ret < 0) {
			pinctrl_utils_free_map(pctldev, *map, *num_maps);
			return ret;
		}
	}

	return 0;
}

static const struct pinctrl_ops s32_pctrl_ops = {
	.get_groups_count = s32_get_groups_count,
	.get_group_name = s32_get_group_name,
	.get_group_pins = s32_get_group_pins,
	.pin_dbg_show = s32_pin_dbg_show,
	.dt_node_to_map = s32_dt_node_to_map,
	.dt_free_map = pinctrl_utils_free_map,
};

static int s32_pmx_set(struct pinctrl_dev *pctldev, unsigned int selector,
		       unsigned int group)
{
	struct s32_pinctrl *ipctl = pinctrl_dev_get_drvdata(pctldev);
	const struct s32_pinctrl_soc_info *info = ipctl->info;
	int i, ret;
	struct s32_pin_group *grp;

	/*
	 * Configure the mux mode for each pin in the group for a specific
	 * function.
	 */
	grp = &info->groups[group];

	dev_dbg(ipctl->dev, "set mux for function %s group %s\n",
		info->functions[selector].name, grp->data.name);

	/* Check beforehand so we don't have a partial config. */
	for (i = 0; i < grp->data.npins; i++) {
		if (s32_check_pin(pctldev, grp->data.pins[i]) != 0) {
			dev_err(info->dev, "Invalid pin: %u in group: %u\n",
				grp->data.pins[i], group);
			return -EINVAL;
		}
	}

	for (i = 0, ret = 0; i < grp->data.npins && !ret; i++) {
		ret = s32_regmap_update(pctldev, grp->data.pins[i],
					S32_MSCR_SSS_MASK, grp->pin_sss[i]);
		if (ret) {
			dev_err(info->dev, "Failed to set pin %u\n",
				grp->data.pins[i]);
			return ret;
		}
	}

	return 0;
}

static int s32_pmx_get_funcs_count(struct pinctrl_dev *pctldev)
{
	struct s32_pinctrl *ipctl = pinctrl_dev_get_drvdata(pctldev);
	const struct s32_pinctrl_soc_info *info = ipctl->info;

	return info->nfunctions;
}

static const char *s32_pmx_get_func_name(struct pinctrl_dev *pctldev,
					 unsigned int selector)
{
	struct s32_pinctrl *ipctl = pinctrl_dev_get_drvdata(pctldev);
	const struct s32_pinctrl_soc_info *info = ipctl->info;

	return info->functions[selector].name;
}

static int s32_pmx_get_groups(struct pinctrl_dev *pctldev,
			      unsigned int selector,
			      const char * const **groups,
			      unsigned int * const num_groups)
{
	struct s32_pinctrl *ipctl = pinctrl_dev_get_drvdata(pctldev);
	const struct s32_pinctrl_soc_info *info = ipctl->info;

	*groups = info->functions[selector].groups;
	*num_groups = info->functions[selector].ngroups;

	return 0;
}

static int s32_pmx_gpio_set_direction(struct pinctrl_dev *pctldev,
				      struct pinctrl_gpio_range *range,
				      unsigned int offset,
				      bool input)
{
	/* Always enable IBE for GPIOs. This allows us to read the
	 * actual line value and compare it with the one set.
	 */
	unsigned int config = S32_MSCR_IBE;
	unsigned int mask = S32_MSCR_IBE | S32_MSCR_OBE;

	/* Enable output buffer */
	if (!input)
		config |= S32_MSCR_OBE;

	return s32_regmap_update(pctldev, offset, mask, config);
}

static const struct pinmux_ops s32_pmx_ops = {
	.get_functions_count = s32_pmx_get_funcs_count,
	.get_function_name = s32_pmx_get_func_name,
	.get_function_groups = s32_pmx_get_groups,
	.set_mux = s32_pmx_set,
	.gpio_set_direction = s32_pmx_gpio_set_direction,
};

/* Set the reserved elements as -1 */
static const int support_slew[] = {208, -1, -1, -1, 166, 150, 133, 83};

static int s32_get_slew_regval(int arg)
{
	unsigned int i;

	/* Translate a real slew rate (MHz) to a register value */
	for (i = 0; i < ARRAY_SIZE(support_slew); i++) {
		if (arg == support_slew[i])
			return i;
	}

	return -EINVAL;
}

static void s32_pin_set_pull(enum pin_config_param param,
			     unsigned int *mask, unsigned int *config)
{
	switch (param) {
	case PIN_CONFIG_BIAS_DISABLE:
	case PIN_CONFIG_BIAS_HIGH_IMPEDANCE:
		*config &= ~(S32_MSCR_PUS | S32_MSCR_PUE);
		break;
	case PIN_CONFIG_BIAS_PULL_UP:
		*config |= S32_MSCR_PUS | S32_MSCR_PUE;
		break;
	case PIN_CONFIG_BIAS_PULL_DOWN:
		*config &= ~S32_MSCR_PUS;
		*config |= S32_MSCR_PUE;
		break;
	default:
		return;
	}

	*mask |= S32_MSCR_PUS | S32_MSCR_PUE;
}

static int s32_parse_pincfg(unsigned long pincfg, unsigned int *mask,
			    unsigned int *config)
{
	enum pin_config_param param;
	u32 arg;
	int ret;

	param = pinconf_to_config_param(pincfg);
	arg = pinconf_to_config_argument(pincfg);

	switch (param) {
	/* All pins are persistent over suspend */
	case PIN_CONFIG_PERSIST_STATE:
		return 0;
	case PIN_CONFIG_DRIVE_OPEN_DRAIN:
		*config |= S32_MSCR_ODE;
		*mask |= S32_MSCR_ODE;
		break;
	case PIN_CONFIG_DRIVE_PUSH_PULL:
		*config &= ~S32_MSCR_ODE;
		*mask |= S32_MSCR_ODE;
		break;
	case PIN_CONFIG_OUTPUT_ENABLE:
		if (arg)
			*config |= S32_MSCR_OBE;
		else
			*config &= ~S32_MSCR_OBE;
		*mask |= S32_MSCR_OBE;
		break;
	case PIN_CONFIG_INPUT_ENABLE:
		if (arg)
			*config |= S32_MSCR_IBE;
		else
			*config &= ~S32_MSCR_IBE;
		*mask |= S32_MSCR_IBE;
		break;
	case PIN_CONFIG_SLEW_RATE:
		ret = s32_get_slew_regval(arg);
		if (ret < 0)
			return ret;
		*config |= S32_MSCR_SRE((u32)ret);
		*mask |= S32_MSCR_SRE(~0);
		break;
	case PIN_CONFIG_BIAS_DISABLE:
	case PIN_CONFIG_BIAS_PULL_UP:
	case PIN_CONFIG_BIAS_PULL_DOWN:
		s32_pin_set_pull(param, mask, config);
		break;
	case PIN_CONFIG_BIAS_HIGH_IMPEDANCE:
		*config &= ~(S32_MSCR_ODE | S32_MSCR_OBE | S32_MSCR_IBE);
		*mask |= S32_MSCR_ODE | S32_MSCR_OBE | S32_MSCR_IBE;
		s32_pin_set_pull(param, mask, config);
		break;
	default:
		return -EOPNOTSUPP;
	}

	return 0;
}

static int s32_pinconf_mscr_write(struct pinctrl_dev *pctldev,
				   unsigned int pin_id,
				   unsigned long *configs,
				   unsigned int num_configs,
				   enum s32_write_type write_type)
{
	struct s32_pinctrl *ipctl = pinctrl_dev_get_drvdata(pctldev);
	unsigned int config = 0, mask = 0;
	int i, ret;

	ret = s32_check_pin(pctldev, pin_id);
	if (ret)
		return ret;

	dev_dbg(ipctl->dev, "pinconf set pin %s with %u configs\n",
		pin_get_name(pctldev, pin_id), num_configs);

	for (i = 0; i < num_configs; i++) {
		ret = s32_parse_pincfg(configs[i], &mask, &config);
		if (ret)
			return ret;
	}

	/* If the MSCR configuration has to be written,
	 * the SSS field should not be touched.
	 */
	if (write_type == S32_PINCONF_OVERWRITE)
		mask = (unsigned int)~S32_MSCR_SSS_MASK;

	if (!config && !mask)
		return 0;

	if (write_type == S32_PINCONF_OVERWRITE)
		dev_dbg(ipctl->dev, "set: pin %u cfg 0x%x\n", pin_id, config);
	else
		dev_dbg(ipctl->dev, "update: pin %u cfg 0x%x\n", pin_id,
			config);

	return s32_regmap_update(pctldev, pin_id, mask, config);
}

static int s32_pinconf_get(struct pinctrl_dev *pctldev,
			   unsigned int pin_id,
			   unsigned long *config)
{
	return s32_regmap_read(pctldev, pin_id, (unsigned int *)config);
}

static int s32_pinconf_set(struct pinctrl_dev *pctldev,
			   unsigned int pin_id, unsigned long *configs,
			   unsigned int num_configs)
{
	return s32_pinconf_mscr_write(pctldev, pin_id, configs,
				       num_configs, S32_PINCONF_UPDATE_ONLY);
}

static int s32_pconf_group_set(struct pinctrl_dev *pctldev, unsigned int selector,
			       unsigned long *configs, unsigned int num_configs)
{
	struct s32_pinctrl *ipctl = pinctrl_dev_get_drvdata(pctldev);
	const struct s32_pinctrl_soc_info *info = ipctl->info;
	struct s32_pin_group *grp;
	int i, ret;

	grp = &info->groups[selector];
	for (i = 0; i < grp->data.npins; i++) {
		ret = s32_pinconf_mscr_write(pctldev, grp->data.pins[i],
					      configs, num_configs, S32_PINCONF_OVERWRITE);
		if (ret)
			return ret;
	}

	return 0;
}

static void s32_pinconf_dbg_show(struct pinctrl_dev *pctldev,
				 struct seq_file *s, unsigned int pin_id)
{
	unsigned int config;
	int ret;

	ret = s32_regmap_read(pctldev, pin_id, &config);
	if (ret)
		return;

	seq_printf(s, "0x%x", config);
}

static void s32_pinconf_group_dbg_show(struct pinctrl_dev *pctldev,
				       struct seq_file *s, unsigned int selector)
{
	struct s32_pinctrl *ipctl = pinctrl_dev_get_drvdata(pctldev);
	const struct s32_pinctrl_soc_info *info = ipctl->info;
	struct s32_pin_group *grp;
	unsigned int config;
	const char *name;
	int i, ret;

	seq_puts(s, "\n");
	grp = &info->groups[selector];
	for (i = 0; i < grp->data.npins; i++) {
		name = pin_get_name(pctldev, grp->data.pins[i]);
		ret = s32_regmap_read(pctldev, grp->data.pins[i], &config);
		if (ret)
			return;
		seq_printf(s, "%s: 0x%x\n", name, config);
	}
}

static const struct pinconf_ops s32_pinconf_ops = {
	.pin_config_get = s32_pinconf_get,
	.pin_config_set	= s32_pinconf_set,
	.pin_config_group_set = s32_pconf_group_set,
	.pin_config_dbg_show = s32_pinconf_dbg_show,
	.pin_config_group_dbg_show = s32_pinconf_group_dbg_show,
};

static struct s32_pinctrl *to_s32_pinctrl(struct gpio_chip *chip)
{
	return container_of(chip, struct s32_pinctrl, gc);
}

static struct regmap *s32_gpio_get_pgpd_regmap(struct gpio_chip *chip,
					       unsigned int gpio,
					       bool output,
						   unsigned int *relative_pin)
{
	struct s32_pinctrl *ipctl = to_s32_pinctrl(chip);
	const struct s32_gpio_range *range;
	int i;

	for (i = 0; i < ipctl->num_gpio_regmaps; i++) {
		range = ipctl->gpio_regmaps[i].range;
		if (gpio >= range->gpio_base &&
		    gpio < range->gpio_base + range->gpio_num) {
			if (relative_pin)
				*relative_pin = gpio - range->gpio_base;
			return output ? ipctl->gpio_regmaps[i].pgpdo :
					ipctl->gpio_regmaps[i].pgpdi;
		}
	}

	return NULL;
}

static int s32_gpio_request(struct gpio_chip *gc, unsigned int gpio)
{
	struct s32_pinctrl *ipctl = to_s32_pinctrl(gc);
	struct pinctrl_dev *pctldev = ipctl->pctl;
	struct gpio_pin_config *gpio_pin;
	unsigned int config;
	int ret;

	ret = s32_regmap_read(pctldev, gpio, &config);
	if (ret)
		return ret;

	/* Save current configuration */
	gpio_pin = kmalloc(sizeof(*gpio_pin), GFP_KERNEL);
	if (!gpio_pin)
		return -ENOMEM;

	gpio_pin->pin_id = gpio;
	gpio_pin->config = config;
	INIT_LIST_HEAD(&gpio_pin->list);

	/* GPIO pin means SSS = 0 */
	config &= ~S32_MSCR_SSS_MASK;

	ret = s32_regmap_write(pctldev, gpio, config);
	if (ret) {
		kfree(gpio_pin);
		return ret;
	}

	scoped_guard(spinlock_irqsave, &ipctl->gpio_configs_lock)
		list_add(&gpio_pin->list, &ipctl->gpio_configs);

	return 0;
}

static void s32_gpio_free(struct gpio_chip *gc, unsigned int gpio)
{
	struct s32_pinctrl *ipctl = to_s32_pinctrl(gc);
	struct pinctrl_dev *pctldev = ipctl->pctl;
	struct gpio_pin_config *gpio_pin, *tmp;
	int ret;

	guard(spinlock_irqsave)(&ipctl->gpio_configs_lock);

	list_for_each_entry_safe(gpio_pin, tmp, &ipctl->gpio_configs, list) {
		if (gpio_pin->pin_id == gpio) {
			list_del(&gpio_pin->list);
			ret = s32_regmap_write(pctldev, gpio_pin->pin_id,
					       gpio_pin->config);
			if (ret)
				dev_warn(gc->parent, "Failed to restore config for pin %u\n", gpio);
			kfree(gpio_pin);
			return;
		}
	}
}

static int s32_gpio_get_dir(struct gpio_chip *chip, unsigned int gpio)
{
	struct s32_pinctrl *ipctl = to_s32_pinctrl(chip);
	unsigned int reg_value;
	int ret;

	ret = s32_regmap_read(ipctl->pctl, gpio, &reg_value);
	if (ret)
		return ret;

	if (!(reg_value & S32_MSCR_IBE))
		return -EIO;

	return reg_value & S32_MSCR_OBE ? GPIO_LINE_DIRECTION_OUT :
					  GPIO_LINE_DIRECTION_IN;
}

static unsigned int s32_pin2pad(unsigned int pin)
{
	return pin / S32_PGPD_SIZE;
}

static u16 s32_pin2mask(unsigned int pin)
{
	/*
	 * From Reference manual :
	 * PGPDOx[PPDOy] = GPDO(x × 16) + (15 - y)[PDO_(x × 16) + (15 - y)]
	 */
	return BIT(S32_PGPD_SIZE - 1 - pin % S32_PGPD_SIZE);
}

static struct regmap *s32_gpio_get_regmap_offset_mask(struct gpio_chip *chip,
						      unsigned int gpio,
						      unsigned int *reg_offset,
						      u16 *mask,
						      bool output)
{
	struct regmap *regmap;
	unsigned int pad, relative_pin;

	regmap = s32_gpio_get_pgpd_regmap(chip, gpio, output, &relative_pin);
	if (!regmap)
		return NULL;

	*mask = s32_pin2mask(relative_pin);
	pad = s32_pin2pad(relative_pin);

	*reg_offset = S32_PGPD(pad);

	return regmap;
}

static int s32_gpio_set_val(struct gpio_chip *chip, unsigned int gpio,
			    int value)
{
	unsigned int reg_offset;
	struct regmap *regmap;
	u16 mask;

	regmap = s32_gpio_get_regmap_offset_mask(chip, gpio, &reg_offset,
						 &mask, true);
	if (!regmap)
		return -ENODEV;

	value = value ? mask : 0;

	return regmap_update_bits(regmap, reg_offset, mask, value);
}

static int s32_gpio_set(struct gpio_chip *chip, unsigned int gpio,
			int value)
{
	return s32_gpio_set_val(chip, gpio, value);
}

static int s32_gpio_get(struct gpio_chip *chip, unsigned int gpio)
{
	unsigned int reg_offset, value;
	struct regmap *regmap;
	u16 mask;
	int ret;

	regmap = s32_gpio_get_regmap_offset_mask(chip, gpio, &reg_offset,
						 &mask, false);
	if (!regmap)
		return -EINVAL;

	ret = regmap_read(regmap, reg_offset, &value);
	if (ret)
		return ret;

	return !!(value & mask);
}

static int s32_gpio_dir_out(struct gpio_chip *chip, unsigned int gpio,
			    int val)
{
	struct s32_pinctrl *ipctl = to_s32_pinctrl(chip);
	int ret;

	ret = s32_gpio_set_val(chip, gpio, val);
	if (ret)
		return ret;

	return s32_pmx_gpio_set_direction(ipctl->pctl, NULL, gpio, false);
}

static int s32_gpio_dir_in(struct gpio_chip *chip, unsigned int gpio)
{
	struct s32_pinctrl *ipctl = to_s32_pinctrl(chip);

	return s32_pmx_gpio_set_direction(ipctl->pctl, NULL, gpio, true);
}

static bool s32_gpio_is_valid(struct gpio_chip *chip, unsigned int gpio)
{
	struct s32_pinctrl *ipctl = to_s32_pinctrl(chip);
	const struct s32_pinctrl_soc_data *soc_data;
	const struct pinctrl_pin_desc *pins;
	int i;

	soc_data = ipctl->info->soc_data;
	pins = ipctl->info->soc_data->pins;
	for (i = 0; i < soc_data->npins && pins[i].number <= gpio; i++)
		if (pins[i].number == gpio)
			return true;

	return false;
}

static int s32_init_valid_mask(struct gpio_chip *chip, unsigned long *mask,
			       unsigned int ngpios)
{
	struct s32_pinctrl *ipctl = to_s32_pinctrl(chip);
	const struct s32_pinctrl_soc_data *soc_data;
	const struct pinctrl_pin_desc *pins;
	int i;

	bitmap_zero(mask, ngpios);

	soc_data = ipctl->info->soc_data;
	pins = soc_data->pins;

	for (i = 0; i < soc_data->npins; i++)
		if (pins[i].number < ngpios)
			bitmap_set(mask, pins[i].number, 1);

	return 0;
}

static int s32_gpio_gen_names(struct device *dev, struct gpio_chip *gc,
			      unsigned int cnt, char **names, char *ch_index,
			      unsigned int *num_index)
{
	unsigned int gpio;
	unsigned int i;

	/*
	 * GPIO names follow the format P<port>_<pin>, for example:
	 *   PA_00 .. PA_15, PB_00 .. PB_15, ..
	 *
	 * @num_index tracks the absolute GPIO index. The port letter is
	 * advanced whenever the index crosses a 16-pin boundary.
	 */
	for (i = 0; i < cnt; i++) {
		gpio = *num_index;
		if (i != 0 && (gpio % 16) == 0)
			(*ch_index)++;

		if (s32_gpio_is_valid(gc, gpio)) {
			names[i] = devm_kasprintf(dev, GFP_KERNEL, "P%c_%02d",
						  *ch_index, gpio & 0xF);
			if (!names[i])
				return -ENOMEM;
		}

		(*num_index)++;
	}

	return 0;
}

static int s32_gpio_populate_names(struct device *dev,
				   struct s32_pinctrl *ipctl)
{
	const struct s32_pinctrl_soc_data *soc_data = ipctl->info->soc_data;
	const struct s32_gpio_range *range;
	unsigned int num_index = 0;
	char ch_index = 'A';
	char **names;
	int i, ret;

	names = devm_kcalloc(dev, ipctl->gc.ngpio, sizeof(*names),
			     GFP_KERNEL);
	if (!names)
		return -ENOMEM;

	for (i = 0; i < soc_data->num_gpio_ranges; i++) {
		range = &soc_data->gpio_ranges[i];

		if (range->gpio_base % 16 == 0)
			num_index = 0;

		ret = s32_gpio_gen_names(dev, &ipctl->gc, range->gpio_num,
					 names + range->gpio_base,
					 &ch_index, &num_index);
		if (ret)
			return dev_err_probe(dev, ret,
					     "Error setting SIUL2_%d names\n",
					     i);

		ch_index++;
	}

	ipctl->gc.names = (const char *const *)names;

	return 0;
}

static int s32_pinctrl_init_gpio_regmaps(struct platform_device *pdev,
					 struct s32_pinctrl *ipctl)
{
	const struct s32_pinctrl_soc_data *soc_data = ipctl->info->soc_data;
	static const struct regmap_config pgpd_config = {
		.reg_bits = 32,
		.val_bits = 16,
		.reg_stride = 2,
	};
	struct regmap_config cfg;
	struct resource *res;
	void __iomem *base;
	unsigned int pgpdo_idx, pgpdi_idx;
	unsigned int i;

	if (!soc_data->gpio_ranges || !soc_data->num_gpio_ranges)
		return 0;

	ipctl->num_gpio_regmaps = soc_data->num_gpio_ranges;
	ipctl->gpio_regmaps = devm_kcalloc(&pdev->dev, ipctl->num_gpio_regmaps,
					   sizeof(*ipctl->gpio_regmaps),
					   GFP_KERNEL);
	if (!ipctl->gpio_regmaps)
		return -ENOMEM;

	for (i = 0; i < ipctl->num_gpio_regmaps; i++) {
		ipctl->gpio_regmaps[i].range = &soc_data->gpio_ranges[i];

		/*
		 * GPIO resources are placed after the pinctrl regions
		 */
		pgpdo_idx = soc_data->mem_regions + i * 2;
		pgpdi_idx = soc_data->mem_regions + i * 2 + 1;

		/* PGPDO */
		res = platform_get_resource(pdev, IORESOURCE_MEM, pgpdo_idx);
		if (!res)
			return dev_err_probe(&pdev->dev, -ENOENT,
						 "Missing PGPDO resource %u\n", i);

		base = devm_ioremap_resource(&pdev->dev, res);
		if (IS_ERR(base))
			return PTR_ERR(base);

		cfg = pgpd_config;
		cfg.name = devm_kasprintf(&pdev->dev, GFP_KERNEL, "pgpdo%u", i);
		if (!cfg.name)
			return -ENOMEM;

		cfg.max_register = resource_size(res) - cfg.reg_stride;

		ipctl->gpio_regmaps[i].pgpdo =
			devm_regmap_init_mmio(&pdev->dev, base, &cfg);
		if (IS_ERR(ipctl->gpio_regmaps[i].pgpdo))
			return dev_err_probe(&pdev->dev,
						 PTR_ERR(ipctl->gpio_regmaps[i].pgpdo),
						 "Failed to init PGPDO regmap %u\n", i);

		/* PGPDI */
		res = platform_get_resource(pdev, IORESOURCE_MEM, pgpdi_idx);
		if (!res)
			return dev_err_probe(&pdev->dev, -ENOENT,
						 "Missing PGPDI resource %u\n", i);

		base = devm_ioremap_resource(&pdev->dev, res);
		if (IS_ERR(base))
			return PTR_ERR(base);

		cfg = pgpd_config;
		cfg.name = devm_kasprintf(&pdev->dev, GFP_KERNEL, "pgpdi%u", i);
		if (!cfg.name)
			return -ENOMEM;

		cfg.max_register = resource_size(res) - cfg.reg_stride;

		ipctl->gpio_regmaps[i].pgpdi =
			devm_regmap_init_mmio(&pdev->dev, base, &cfg);
		if (IS_ERR(ipctl->gpio_regmaps[i].pgpdi))
			return dev_err_probe(&pdev->dev,
						 PTR_ERR(ipctl->gpio_regmaps[i].pgpdi),
						 "Failed to init PGPDI regmap %u\n", i);
	}

	return 0;
}

#ifdef CONFIG_PM_SLEEP
static bool s32_pinctrl_should_save(struct s32_pinctrl *ipctl,
				    unsigned int pin)
{
	const struct pin_desc *pd = pin_desc_get(ipctl->pctl, pin);

	if (!pd)
		return false;

	/*
	 * Only restore the pin if it is actually in use by the kernel (or
	 * by userspace).
	 */
	if (pd->mux_owner || pd->gpio_owner)
		return true;

	return false;
}

int s32_pinctrl_suspend(struct device *dev)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct s32_pinctrl *ipctl = platform_get_drvdata(pdev);
	const struct pinctrl_pin_desc *pin;
	const struct s32_pinctrl_soc_info *info = ipctl->info;
	struct s32_pinctrl_context *saved_context = &ipctl->saved_context;
	int i;
	int ret;
	unsigned int config;

	for (i = 0; i < info->soc_data->npins; i++) {
		pin = &info->soc_data->pins[i];

		if (!s32_pinctrl_should_save(ipctl, pin->number))
			continue;

		ret = s32_regmap_read(ipctl->pctl, pin->number, &config);
		if (ret)
			return -EINVAL;

		saved_context->pads[i] = config;
	}

	return 0;
}

int s32_pinctrl_resume(struct device *dev)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct s32_pinctrl *ipctl = platform_get_drvdata(pdev);
	const struct s32_pinctrl_soc_info *info = ipctl->info;
	const struct pinctrl_pin_desc *pin;
	struct s32_pinctrl_context *saved_context = &ipctl->saved_context;
	int ret, i;

	for (i = 0; i < info->soc_data->npins; i++) {
		pin = &info->soc_data->pins[i];

		if (!s32_pinctrl_should_save(ipctl, pin->number))
			continue;

		ret = s32_regmap_write(ipctl->pctl, pin->number,
					 saved_context->pads[i]);
		if (ret)
			return ret;
	}

	return 0;
}
#endif

static int s32_pinctrl_parse_groups(struct device_node *np,
				     struct s32_pin_group *grp,
				     struct s32_pinctrl_soc_info *info)
{
	struct device *dev;
	unsigned int *pins, *sss;
	int i, npins;
	u32 pinmux;

	dev = info->dev;

	dev_dbg(dev, "group: %pOFn\n", np);

	/* Initialise group */
	grp->data.name = np->name;

	npins = of_property_count_elems_of_size(np, "pinmux", sizeof(u32));
	if (npins < 0)
		return dev_err_probe(dev, -EINVAL,
				     "Failed to read 'pinmux' in node %s\n",
				     grp->data.name);

	if (!npins)
		return dev_err_probe(dev, -EINVAL,
				     "The group %s has no pins\n",
				     grp->data.name);

	grp->data.npins = npins;

	pins = devm_kcalloc(info->dev, npins, sizeof(*pins), GFP_KERNEL);
	sss = devm_kcalloc(info->dev, npins, sizeof(*sss), GFP_KERNEL);
	if (!pins || !sss)
		return -ENOMEM;

	i = 0;
	of_property_for_each_u32(np, "pinmux", pinmux) {
		pins[i] = get_pin_no(pinmux);
		sss[i] = get_pin_func(pinmux);

		dev_dbg(info->dev, "pin: 0x%x, sss: 0x%x", pins[i], sss[i]);
		i++;
	}

	grp->data.pins = pins;
	grp->pin_sss = sss;

	return 0;
}

static int s32_pinctrl_parse_functions(struct device_node *np,
					struct s32_pinctrl_soc_info *info,
					u32 index)
{
	struct pinfunction *func;
	struct s32_pin_group *grp;
	const char **groups;
	u32 i = 0;
	int ret = 0;

	dev_dbg(info->dev, "parse function(%u): %pOFn\n", index, np);

	func = &info->functions[index];

	/* Initialise function */
	func->name = np->name;
	func->ngroups = of_get_child_count(np);
	if (func->ngroups == 0)
		return dev_err_probe(info->dev, -EINVAL,
				     "No groups defined in %pOF\n", np);

	groups = devm_kcalloc(info->dev, func->ngroups,
				    sizeof(*func->groups), GFP_KERNEL);
	if (!groups)
		return -ENOMEM;

	for_each_child_of_node_scoped(np, child) {
		groups[i] = child->name;
		grp = &info->groups[info->grp_index++];
		ret = s32_pinctrl_parse_groups(child, grp, info);
		if (ret)
			return ret;
		i++;
	}

	func->groups = groups;

	return 0;
}

static int s32_pinctrl_probe_dt(struct platform_device *pdev,
				struct s32_pinctrl *ipctl)
{
	struct s32_pinctrl_soc_info *info = ipctl->info;
	struct device_node *np = pdev->dev.of_node;
	struct resource *res;
	struct regmap *map;
	void __iomem *base;
	unsigned int mem_regions = info->soc_data->mem_regions;
	int ret;
	u32 nfuncs = 0;
	u32 i = 0;

	if (!np)
		return -ENODEV;

	if (mem_regions == 0 || mem_regions >= 10000) {
		dev_err(&pdev->dev, "mem_regions is invalid: %u\n", mem_regions);
		return -EINVAL;
	}

	ipctl->regions = devm_kcalloc(&pdev->dev, mem_regions,
				      sizeof(*ipctl->regions), GFP_KERNEL);
	if (!ipctl->regions)
		return -ENOMEM;

	for (i = 0; i < mem_regions; i++) {
		base = devm_platform_get_and_ioremap_resource(pdev, i, &res);
		if (IS_ERR(base))
			return PTR_ERR(base);

		snprintf(ipctl->regions[i].name,
			 sizeof(ipctl->regions[i].name), "map%u", i);

		s32_regmap_config.name = ipctl->regions[i].name;
		s32_regmap_config.max_register = resource_size(res) -
						 s32_regmap_config.reg_stride;

		map = devm_regmap_init_mmio(&pdev->dev, base,
						&s32_regmap_config);
		if (IS_ERR(map)) {
			dev_err(&pdev->dev, "Failed to init regmap[%u]\n", i);
			return PTR_ERR(map);
		}

		ipctl->regions[i].map = map;
		ipctl->regions[i].pin_range = &info->soc_data->mem_pin_ranges[i];
	}

	nfuncs = of_get_child_count(np);
	if (nfuncs <= 0)
		return dev_err_probe(&pdev->dev, -EINVAL,
				     "No functions defined\n");

	info->nfunctions = nfuncs;
	info->functions = devm_kcalloc(&pdev->dev, nfuncs,
				       sizeof(*info->functions), GFP_KERNEL);
	if (!info->functions)
		return -ENOMEM;

	info->ngroups = 0;
	for_each_child_of_node_scoped(np, child)
		info->ngroups += of_get_child_count(child);

	info->groups = devm_kcalloc(&pdev->dev, info->ngroups,
				    sizeof(*info->groups), GFP_KERNEL);
	if (!info->groups)
		return -ENOMEM;

	i = 0;
	for_each_child_of_node_scoped(np, child) {
		ret = s32_pinctrl_parse_functions(child, info, i++);
		if (ret)
			return ret;
	}

	return 0;
}

int s32_pinctrl_probe(struct platform_device *pdev,
		      const struct s32_pinctrl_soc_data *soc_data)
{
#ifdef CONFIG_PM_SLEEP
	struct s32_pinctrl_context *saved_context;
#endif
	const struct s32_gpio_range *last_range;
	struct pinctrl_desc *s32_pinctrl_desc;
	struct s32_pinctrl_soc_info *info;
	struct s32_pinctrl *ipctl;
	struct gpio_chip *gc;
	int ret;

	if (!soc_data || !soc_data->pins || !soc_data->npins)
		return dev_err_probe(&pdev->dev, -EINVAL,
				     "Wrong pinctrl info\n");

	info = devm_kzalloc(&pdev->dev, sizeof(*info), GFP_KERNEL);
	if (!info)
		return -ENOMEM;

	info->soc_data = soc_data;
	info->dev = &pdev->dev;

	/* Create state holders etc for this driver */
	ipctl = devm_kzalloc(&pdev->dev, sizeof(*ipctl), GFP_KERNEL);
	if (!ipctl)
		return -ENOMEM;

	ipctl->info = info;
	ipctl->dev = info->dev;
	platform_set_drvdata(pdev, ipctl);

	INIT_LIST_HEAD(&ipctl->gpio_configs);
	spin_lock_init(&ipctl->gpio_configs_lock);

	s32_pinctrl_desc =
		devm_kzalloc(&pdev->dev, sizeof(*s32_pinctrl_desc), GFP_KERNEL);
	if (!s32_pinctrl_desc)
		return -ENOMEM;

	s32_pinctrl_desc->name = dev_name(&pdev->dev);
	s32_pinctrl_desc->pins = info->soc_data->pins;
	s32_pinctrl_desc->npins = info->soc_data->npins;
	s32_pinctrl_desc->pctlops = &s32_pctrl_ops;
	s32_pinctrl_desc->pmxops = &s32_pmx_ops;
	s32_pinctrl_desc->confops = &s32_pinconf_ops;
	s32_pinctrl_desc->owner = THIS_MODULE;

	ret = s32_pinctrl_probe_dt(pdev, ipctl);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "Fail to probe dt properties\n");

	ret = s32_pinctrl_init_gpio_regmaps(pdev, ipctl);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
					 "Failed to init GPIO regmaps\n");

	ret = devm_pinctrl_register_and_init(&pdev->dev, s32_pinctrl_desc,
					     ipctl, &ipctl->pctl);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "Could not register s32 pinctrl driver\n");

#ifdef CONFIG_PM_SLEEP
	saved_context = &ipctl->saved_context;
	saved_context->pads =
		devm_kcalloc(&pdev->dev, info->soc_data->npins,
			     sizeof(*saved_context->pads),
			     GFP_KERNEL);
	if (!saved_context->pads)
		return -ENOMEM;
#endif

	ret = pinctrl_enable(ipctl->pctl);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "Failed to enable pinctrl\n");

	dev_info(&pdev->dev, "Initialized S32 pinctrl driver\n");

	/* Setup GPIO if GPIO ranges are defined */
	if (!soc_data->gpio_ranges || !soc_data->num_gpio_ranges)
		return 0;

	gc = &ipctl->gc;
	gc->parent = &pdev->dev;
	gc->label = dev_name(&pdev->dev);
	gc->base = -1;
	/*
	 * In some cases, there is a gap between the SIUL GPIOs.
	 *
	 * gpio_ranges[] is expected to be sorted by increasing gpio_base.
	 * ngpio is derived from the last range and must cover the highest
	 * GPIO offset, even when gaps exist in the numbering.
	 */
	last_range = &soc_data->gpio_ranges[soc_data->num_gpio_ranges - 1];
	gc->ngpio = last_range->gpio_base + last_range->gpio_num;
	ret = s32_gpio_populate_names(&pdev->dev, ipctl);
	if (ret)
		return ret;

	gc->set = s32_gpio_set;
	gc->get = s32_gpio_get;
	gc->set_config = gpiochip_generic_config;
	gc->request = s32_gpio_request;
	gc->free = s32_gpio_free;
	gc->direction_output = s32_gpio_dir_out;
	gc->direction_input = s32_gpio_dir_in;
	gc->get_direction = s32_gpio_get_dir;
	gc->init_valid_mask = s32_init_valid_mask;

	ret = devm_gpiochip_add_data(&pdev->dev, gc, ipctl);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "Unable to add gpiochip\n");

	dev_info(&pdev->dev, "Initialized S32 GPIO functionality\n");

	return 0;
}
