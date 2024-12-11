// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2024 Amlogic, Inc. All rights reserved.
 * Author: Xianwei Zhao <xianwei.zhao@amlogic.com>
 */

#include <linux/err.h>
#include <linux/gpio/driver.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/string_helpers.h>

#include <linux/pinctrl/consumer.h>
#include <linux/pinctrl/pinconf.h>
#include <linux/pinctrl/pinctrl.h>
#include <linux/pinctrl/pinmux.h>
#include <dt-bindings/pinctrl/amlogic,pinctrl.h>

#include "core.h"
#include "pinconf.h"

#define gpio_chip_to_bank(chip) \
		container_of(chip, struct aml_gpio_bank, gpio_chip)
/**
 * enum aml_reg_type - type of registers encoded in @aml_reg_desc
 */
enum aml_reg_type {
	REG_PULLEN,
	REG_PULL,
	REG_DIR,
	REG_OUT,
	REG_IN,
	REG_DS,
	NUM_REG,
};

static const unsigned int aml_bit_strides[NUM_REG] = {
	1, 1, 1, 1, 1, 2
};

static const unsigned int aml_reg_type_offset[NUM_REG] = {
	3, 4, 2, 1, 0, 7
};

enum aml_pinconf_drv {
	PINCONF_DRV_500UA,
	PINCONF_DRV_2500UA,
	PINCONF_DRV_3000UA,
	PINCONF_DRV_4000UA,
};

struct aml_pio_control {
	int reg_mux_offset;
	int bit_mux_offset;
	int reg_gpio_offset;
	int reg_offset[NUM_REG];
};

struct aml_pctl_data {
};

struct aml_pinconf {
	int		pin;
	int		altfunc;
};

struct aml_pmx_func {
	const char	*name;
	const char	**groups;
	unsigned int	ngroups;
};

struct aml_pctl_group {
	const char		*name;
	unsigned short		npins;
	unsigned int		*pins;
	struct aml_pinconf	*pin_conf;
	unsigned long           *configs;
	unsigned int            num_configs;
};

struct aml_gpio_bank {
	struct gpio_chip		gpio_chip;
	struct aml_pio_control		pc;
	unsigned int                    bank_idx;
	unsigned int			pin_base;
};

struct aml_pinctrl {
	struct device			*dev;
	struct pinctrl_dev		*pctl;
	struct aml_gpio_bank		*banks;
	int				nbanks;
	struct aml_pmx_func		*functions;
	int				nfunctions;
	struct aml_pctl_group		*groups;
	int				ngroups;

	struct regmap			*reg_mux;
	struct regmap			*reg_gpio;
	struct regmap			*reg_pullen;
	struct regmap			*reg_pull;
	struct regmap			*reg_ds;
	const struct aml_pctl_data	*data;
};

static int aml_pmx_calc_reg_and_offset(struct pinctrl_gpio_range *range,
				       unsigned int pin, unsigned int *reg,
				       unsigned int *offset)
{
	struct aml_gpio_bank *bank = gpio_chip_to_bank(range->gc);
	unsigned int shift;

	shift = ((pin - range->pin_base) << 2) + bank->pc.bit_mux_offset;
	*reg = (bank->pc.reg_mux_offset + (shift / 32)) * 4;
	*offset = shift % 32;

	return 0;
}

static int aml_pctl_set_function(struct aml_pinctrl *info,
				 struct pinctrl_gpio_range *range,
				 int pin_id, int func)
{
	int reg;
	int offset;

	aml_pmx_calc_reg_and_offset(range, pin_id, &reg, &offset);
	return regmap_update_bits(info->reg_mux, reg,
			0xf << offset, (func & 0xf) << offset);
}

static int aml_pmx_get_funcs_count(struct pinctrl_dev *pctldev)
{
	struct aml_pinctrl *info = pinctrl_dev_get_drvdata(pctldev);

	return info->nfunctions;
}

static const char *aml_pmx_get_fname(struct pinctrl_dev *pctldev,
				     unsigned int selector)
{
	struct aml_pinctrl *info = pinctrl_dev_get_drvdata(pctldev);

	return info->functions[selector].name;
}

static int aml_pmx_get_groups(struct pinctrl_dev *pctldev,
			      unsigned int selector,
			      const char * const **grps,
			      unsigned * const ngrps)
{
	struct aml_pinctrl *info = pinctrl_dev_get_drvdata(pctldev);

	*grps = info->functions[selector].groups;
	*ngrps = info->functions[selector].ngroups;

	return 0;
}

static int aml_pmx_set_mux(struct pinctrl_dev *pctldev, unsigned int fselector,
			   unsigned int group)
{
	struct aml_pinctrl *info = pinctrl_dev_get_drvdata(pctldev);
	struct aml_pinconf *conf = info->groups[group].pin_conf;
	struct pinctrl_gpio_range *range;
	int i;

	for (i = 0; i < info->groups[group].npins; i++) {
		range =  pinctrl_find_gpio_range_from_pin(pctldev, conf[i].pin);
		aml_pctl_set_function(info, range, conf[i].pin, conf[i].altfunc);
	}

	return 0;
}

static int aml_pmx_request_gpio(struct pinctrl_dev *pctldev,
				struct pinctrl_gpio_range *range,
				unsigned int pin)
{
	struct aml_pinctrl *info = pinctrl_dev_get_drvdata(pctldev);

	return aml_pctl_set_function(info, range, pin, 0);
}

static const struct pinmux_ops aml_pmx_ops = {
	.set_mux		= aml_pmx_set_mux,
	.get_functions_count	= aml_pmx_get_funcs_count,
	.get_function_name	= aml_pmx_get_fname,
	.get_function_groups	= aml_pmx_get_groups,
	.gpio_request_enable	= aml_pmx_request_gpio,
};

static int aml_calc_reg_and_bit(struct pinctrl_dev *pctldev,
				unsigned int pin,
				enum aml_reg_type reg_type,
				unsigned int *reg, unsigned int *bit)
{
	struct pinctrl_gpio_range *range =
			 pinctrl_find_gpio_range_from_pin(pctldev, pin);
	struct aml_gpio_bank *bank = gpio_chip_to_bank(range->gc);

	*bit = (pin - range->pin_base) * aml_bit_strides[reg_type];
	*reg = (bank->pc.reg_offset[reg_type] + (*bit / 32)) * 4;
	*bit &= 0x1f;

	return 0;
}

static int aml_pinconf_get_pull(struct aml_pinctrl *info, unsigned int pin)
{
	unsigned int reg, bit, val;
	int ret, conf;

	aml_calc_reg_and_bit(info->pctl, pin, REG_PULLEN, &reg, &bit);

	ret = regmap_read(info->reg_pullen, reg, &val);
	if (ret)
		return ret;

	if (!(val & BIT(bit))) {
		conf = PIN_CONFIG_BIAS_DISABLE;
	} else {
		aml_calc_reg_and_bit(info->pctl, pin, REG_PULL, &reg, &bit);

		ret = regmap_read(info->reg_pull, reg, &val);
		if (ret)
			return ret;

		if (val & BIT(bit))
			conf = PIN_CONFIG_BIAS_PULL_UP;
		else
			conf = PIN_CONFIG_BIAS_PULL_DOWN;
	}

	return conf;
}

static int aml_pinconf_get_drive_strength(struct aml_pinctrl *info,
					  unsigned int pin,
					  u16 *drive_strength_ua)
{
	unsigned int reg, bit;
	unsigned int val;
	int ret;

	if (!info->reg_ds)
		return -EOPNOTSUPP;

	aml_calc_reg_and_bit(info->pctl, pin, REG_DS, &reg, &bit);
	ret = regmap_read(info->reg_ds, reg, &val);
	if (ret)
		return ret;

	switch ((val >> bit) & 0x3) {
	case PINCONF_DRV_500UA:
		*drive_strength_ua = 500;
		break;
	case PINCONF_DRV_2500UA:
		*drive_strength_ua = 2500;
		break;
	case PINCONF_DRV_3000UA:
		*drive_strength_ua = 3000;
		break;
	case PINCONF_DRV_4000UA:
		*drive_strength_ua = 4000;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int aml_pinconf_get_gpio_bit(struct aml_pinctrl *info,
				    unsigned int pin,
				    unsigned int reg_type)
{
	unsigned int reg, bit, val;
	int ret;

	aml_calc_reg_and_bit(info->pctl, pin, reg_type, &reg, &bit);
	ret = regmap_read(info->reg_gpio, reg, &val);
	if (ret)
		return ret;

	return BIT(bit) & val ? 1 : 0;
}

static int aml_pinconf_get_output(struct aml_pinctrl *info,
				  unsigned int pin)
{
	int ret = aml_pinconf_get_gpio_bit(info, pin, REG_DIR);

	if (ret < 0)
		return ret;

	return !ret;
}

static int aml_pinconf_get_drive(struct aml_pinctrl *info,
				 unsigned int pin)
{
	return aml_pinconf_get_gpio_bit(info, pin, REG_OUT);
}

static int aml_pinconf_get(struct pinctrl_dev *pcdev, unsigned int pin,
			   unsigned long *config)
{
	struct aml_pinctrl *info = pinctrl_dev_get_drvdata(pcdev);
	enum pin_config_param param = pinconf_to_config_param(*config);
	u16 arg;
	int ret;

	switch (param) {
	case PIN_CONFIG_BIAS_DISABLE:
	case PIN_CONFIG_BIAS_PULL_DOWN:
	case PIN_CONFIG_BIAS_PULL_UP:
		if (aml_pinconf_get_pull(info, pin) == param)
			arg = 1;
		else
			return -EINVAL;
		break;
	case PIN_CONFIG_DRIVE_STRENGTH_UA:
		ret = aml_pinconf_get_drive_strength(info, pin, &arg);
		if (ret)
			return ret;
		break;
	case PIN_CONFIG_OUTPUT_ENABLE:
		ret = aml_pinconf_get_output(info, pin);
		if (ret <= 0)
			return -EINVAL;
		arg = 1;
		break;
	case PIN_CONFIG_OUTPUT:
		ret = aml_pinconf_get_output(info, pin);
		if (ret <= 0)
			return -EINVAL;

		ret = aml_pinconf_get_drive(info, pin);
		if (ret < 0)
			return -EINVAL;

		arg = ret;
		break;

	default:
		return -ENOTSUPP;
	}

	*config = pinconf_to_config_packed(param, arg);
	dev_dbg(info->dev, "pinconf for pin %u is %lu\n", pin, *config);

	return 0;
}

static int aml_pinconf_disable_bias(struct aml_pinctrl *info,
				    unsigned int pin)
{
	unsigned int reg, bit = 0;

	aml_calc_reg_and_bit(info->pctl, pin, REG_PULLEN, &reg, &bit);

	return regmap_update_bits(info->reg_pullen, reg, BIT(bit), 0);
}

static int aml_pinconf_enable_bias(struct aml_pinctrl *info, unsigned int pin,
				   bool pull_up)
{
	unsigned int reg, bit, val = 0;
	int ret;

	aml_calc_reg_and_bit(info->pctl, pin, REG_PULL, &reg, &bit);
	if (pull_up)
		val = BIT(bit);

	ret = regmap_update_bits(info->reg_pull, reg, BIT(bit), val);
	if (ret)
		return ret;

	aml_calc_reg_and_bit(info->pctl, pin, REG_PULLEN, &reg, &bit);
	return regmap_update_bits(info->reg_pullen, reg, BIT(bit), BIT(bit));
}

static int aml_pinconf_set_drive_strength(struct aml_pinctrl *info,
					  unsigned int pin,
					  u16 drive_strength_ua)
{
	unsigned int reg, bit, ds_val;

	if (!info->reg_ds) {
		dev_err(info->dev, "drive-strength not supported\n");
		return -EOPNOTSUPP;
	}

	aml_calc_reg_and_bit(info->pctl, pin, REG_DS, &reg, &bit);

	if (drive_strength_ua <= 500) {
		ds_val = PINCONF_DRV_500UA;
	} else if (drive_strength_ua <= 2500) {
		ds_val = PINCONF_DRV_2500UA;
	} else if (drive_strength_ua <= 3000) {
		ds_val = PINCONF_DRV_3000UA;
	} else if (drive_strength_ua <= 4000) {
		ds_val = PINCONF_DRV_4000UA;
	} else {
		dev_warn_once(info->dev,
			      "pin %u: invalid drive-strength : %d , default to 4mA\n",
			      pin, drive_strength_ua);
		ds_val = PINCONF_DRV_4000UA;
	}

	return regmap_update_bits(info->reg_ds, reg, 0x3 << bit, ds_val << bit);
}

static int aml_pinconf_set_gpio_bit(struct aml_pinctrl *info,
				    unsigned int pin,
				    unsigned int reg_type,
				    bool arg)
{
	unsigned int reg, bit;

	aml_calc_reg_and_bit(info->pctl, pin, reg_type, &reg, &bit);
	return regmap_update_bits(info->reg_gpio, reg, BIT(bit),
				  arg ? BIT(bit) : 0);
}

static int aml_pinconf_set_output(struct aml_pinctrl *info,
				  unsigned int pin,
				  bool out)
{
	return aml_pinconf_set_gpio_bit(info, pin, REG_DIR, !out);
}

static int aml_pinconf_set_drive(struct aml_pinctrl *info,
				 unsigned int pin,
				 bool high)
{
	return aml_pinconf_set_gpio_bit(info, pin, REG_OUT, high);
}

static int aml_pinconf_set_output_drive(struct aml_pinctrl *info,
					unsigned int pin,
					bool high)
{
	int ret;

	ret = aml_pinconf_set_output(info, pin, true);
	if (ret)
		return ret;

	return aml_pinconf_set_drive(info, pin, high);
}

static int aml_pinconf_set(struct pinctrl_dev *pcdev, unsigned int pin,
			   unsigned long *configs, unsigned int num_configs)
{
	struct aml_pinctrl *info = pinctrl_dev_get_drvdata(pcdev);
	enum pin_config_param param;
	unsigned int arg = 0;
	int i, ret;

	for (i = 0; i < num_configs; i++) {
		param = pinconf_to_config_param(configs[i]);

		switch (param) {
		case PIN_CONFIG_DRIVE_STRENGTH_UA:
		case PIN_CONFIG_OUTPUT_ENABLE:
		case PIN_CONFIG_OUTPUT:
			arg = pinconf_to_config_argument(configs[i]);
			break;

		default:
			break;
		}

		switch (param) {
		case PIN_CONFIG_BIAS_DISABLE:
			ret = aml_pinconf_disable_bias(info, pin);
			break;
		case PIN_CONFIG_BIAS_PULL_UP:
			ret = aml_pinconf_enable_bias(info, pin, true);
			break;
		case PIN_CONFIG_BIAS_PULL_DOWN:
			ret = aml_pinconf_enable_bias(info, pin, false);
			break;
		case PIN_CONFIG_DRIVE_STRENGTH_UA:
			ret = aml_pinconf_set_drive_strength(info, pin, arg);
			break;
		case PIN_CONFIG_OUTPUT_ENABLE:
			ret = aml_pinconf_set_output(info, pin, arg);
			break;
		case PIN_CONFIG_OUTPUT:
			ret = aml_pinconf_set_output_drive(info, pin, arg);
			break;
		default:
			ret = -ENOTSUPP;
		}

		if (ret)
			return ret;
	}

	return 0;
}

static int aml_pinconf_group_set(struct pinctrl_dev *pcdev,
				 unsigned int num_group,
				 unsigned long *configs,
				 unsigned int num_configs)
{
	struct aml_pinctrl *info = pinctrl_dev_get_drvdata(pcdev);
	int i;

	for (i = 0; i < info->groups[num_group].npins; i++) {
		aml_pinconf_set(pcdev, info->groups[num_group].pins[i], configs,
				num_configs);
	}

	return 0;
}

static int aml_pinconf_group_get(struct pinctrl_dev *pcdev,
				 unsigned int group, unsigned long *config)
{
	return -EOPNOTSUPP;
}

static const struct pinconf_ops aml_pinconf_ops = {
	.pin_config_get		= aml_pinconf_get,
	.pin_config_set		= aml_pinconf_set,
	.pin_config_group_get	= aml_pinconf_group_get,
	.pin_config_group_set	= aml_pinconf_group_set,
	.is_generic		= true,
};

static int aml_get_groups_count(struct pinctrl_dev *pctldev)
{
	struct aml_pinctrl *info = pinctrl_dev_get_drvdata(pctldev);

	return info->ngroups;
}

static const char *aml_get_group_name(struct pinctrl_dev *pctldev,
				      unsigned int selector)
{
	struct aml_pinctrl *info = pinctrl_dev_get_drvdata(pctldev);

	return info->groups[selector].name;
}

static int aml_get_group_pins(struct pinctrl_dev *pctldev,
			      unsigned int selector, const unsigned int **pins,
			      unsigned int *npins)
{
	struct aml_pinctrl *info = pinctrl_dev_get_drvdata(pctldev);

	if (selector >= info->ngroups)
		return -EINVAL;

	*pins = info->groups[selector].pins;
	*npins = info->groups[selector].npins;

	return 0;
}

static inline const struct aml_pctl_group *
	aml_pctl_find_group_by_name(const struct aml_pinctrl *info,
				    const char *name)
{
	int i;

	for (i = 0; i < info->ngroups; i++) {
		if (!strcmp(info->groups[i].name, name))
			return &info->groups[i];
	}

	return NULL;
}

static int aml_dt_node_to_map(struct pinctrl_dev *pctldev,
			      struct device_node *np,
			      struct pinctrl_map **map,
			      unsigned int *num_maps)
{
	struct aml_pinctrl *info = pinctrl_dev_get_drvdata(pctldev);
	const struct aml_pctl_group *grp;
	struct device *dev = info->dev;
	struct pinctrl_map *new_map;
	struct device_node *parent;
	int map_num, i;

	grp = aml_pctl_find_group_by_name(info, np->name);
	if (!grp) {
		dev_err(dev, "unable to find group for node %pOFn\n", np);
		return -EINVAL;
	}

	if (grp->num_configs)
		map_num = grp->npins + 1;
	else
		map_num = 1;
	new_map = devm_kcalloc(dev, map_num, sizeof(*new_map), GFP_KERNEL);
	if (!new_map)
		return -ENOMEM;

	parent = of_get_parent(np);
	if (!parent) {
		devm_kfree(dev, new_map);
		return -EINVAL;
	}

	*map = new_map;
	*num_maps = map_num;
	new_map[0].type = PIN_MAP_TYPE_MUX_GROUP;
	new_map[0].data.mux.function = parent->name;
	new_map[0].data.mux.group = np->name;
	of_node_put(parent);

	if (grp->num_configs) {
		new_map++;
		for (i = 0; i < grp->npins; i++) {
			new_map[i].type = PIN_MAP_TYPE_CONFIGS_PIN;
			new_map[i].data.configs.group_or_pin =
				pin_get_name(pctldev, grp->pins[i]);
			new_map[i].data.configs.configs = grp->configs;
			new_map[i].data.configs.num_configs = grp->num_configs;
		}
	}

	dev_info(dev, "maps: function %s group %s num %d\n",
		 (*map)->data.mux.function, grp->name, map_num);

	return 0;
}

static void aml_dt_free_map(struct pinctrl_dev *pctldev,
			    struct pinctrl_map *map, unsigned int num_maps)
{
}

static void aml_pin_dbg_show(struct pinctrl_dev *pcdev, struct seq_file *s,
			     unsigned int offset)
{
	seq_printf(s, " %s", dev_name(pcdev->dev));
}

static const struct pinctrl_ops aml_pctrl_ops = {
	.get_groups_count	= aml_get_groups_count,
	.get_group_name		= aml_get_group_name,
	.get_group_pins		= aml_get_group_pins,
	.dt_node_to_map		= aml_dt_node_to_map,
	.dt_free_map		= aml_dt_free_map,
	.pin_dbg_show		= aml_pin_dbg_show,
};

static int aml_pctl_dt_calculate_pin(struct aml_pinctrl *info,
				     unsigned int bank_idx, unsigned int offset)
{
	struct aml_gpio_bank *bank;
	int retval = -EINVAL;
	int i;

	for (i = 0; i < info->nbanks; i++) {
		bank = &info->banks[i];
		if (bank->bank_idx == bank_idx) {
			if (offset < bank->gpio_chip.ngpio)
				retval = bank->pin_base + offset;
			break;
		}
	}
	if (retval == -EINVAL)
		dev_err(info->dev, "pin [bank:%d, offset:%d] is not present\n", bank_idx, offset);

	return retval;
}

static int aml_pctl_dt_parse_groups(struct device_node *np,
				    struct aml_pctl_group *grp,
				    struct aml_pinctrl *info, int idx)
{
	struct device *dev = info->dev;
	struct aml_pinconf *conf;
	struct property *of_pins;
	unsigned int bank_idx;
	unsigned int offset, npins;
	int i = 0;
	int ret;

	of_pins = of_find_property(np, "pinmux", NULL);
	if (!of_pins) {
		dev_info(dev, "Missing pinmux property\n");
		return -ENOENT;
	}

	npins = of_pins->length / sizeof(u32);
	grp->npins = npins;
	grp->name = np->name;
	grp->pins = devm_kcalloc(dev, npins, sizeof(*grp->pins), GFP_KERNEL);
	grp->pin_conf = devm_kcalloc(dev, npins, sizeof(*grp->pin_conf), GFP_KERNEL);

	if (!grp->pins || !grp->pin_conf)
		return -ENOMEM;

	ret = pinconf_generic_parse_dt_config(np, info->pctl, &grp->configs,
					      &grp->num_configs);
	if (ret < 0) {
		dev_err(dev, "%pOF: could not parse node property\n", np);
		return ret;
	}

	for (i = 0; i < npins; i++) {
		u32 value;

		ret = of_property_read_u32_index(np, "pinmux", i, &value);
		if (ret)
			return ret;

		conf = &grp->pin_conf[i];

		bank_idx = AML_PINMUX_TO_BANK(value);
		offset = AML_PINMUX_TO_OFFSET(value);
		conf->pin = aml_pctl_dt_calculate_pin(info, bank_idx, offset);
		if (conf->pin < 0)
			return dev_err_probe(dev, -EINVAL, "pinmux get pin failed\n");
		conf->altfunc = value & AML_PIN_ALT_FUNC_MASK;

		grp->pins[i] = conf->pin;
	}

	return 0;
}

static int aml_pctl_parse_functions(struct device_node *np,
				    struct aml_pinctrl *info, u32 index,
				    int *grp_index)
{
	struct device *dev = info->dev;
	struct aml_pmx_func *func;
	struct aml_pctl_group *grp;
	int ret, i;

	func = &info->functions[index];
	func->name = np->name;
	func->ngroups = of_get_child_count(np);
	if (func->ngroups == 0)
		return dev_err_probe(dev, -EINVAL, "No groups defined\n");

	func->groups = devm_kcalloc(dev, func->ngroups, sizeof(*func->groups), GFP_KERNEL);
	if (!func->groups)
		return -ENOMEM;

	i = 0;
	for_each_child_of_node_scoped(np, child) {
		func->groups[i] = child->name;
		grp = &info->groups[*grp_index];
		*grp_index += 1;
		ret = aml_pctl_dt_parse_groups(child, grp, info, i++);
		if (ret)
			return ret;
	}
	dev_info(dev, "Function[%d\t name:%s,\tgroups:%d]\n", index, func->name, func->ngroups);

	return 0;
}

static u32 aml_bank_pins(struct device_node *np)
{
	u32 value;

	if (of_property_read_u32(np, "npins", &value) < 0)
		return 0;
	else
		return value;
}

static u32 aml_bank_reg_gpio_offset(struct device_node *np)
{
	u32 value;

	if (of_property_read_u32(np, "reg-gpio-offset", &value) < 0)
		return 0;
	else
		return value;
}

static u32 aml_bank_reg_mux_offset(struct device_node *np)
{
	u32 value;

	if (of_property_read_u32(np, "reg-mux-offset", &value) < 0)
		return 0;
	else
		return value;
}

static u32 aml_bank_bit_mux_offset(struct device_node *np)
{
	u32 value;

	if (of_property_read_u32(np, "bit-mux-offset", &value) < 0)
		return 0;
	else
		return value;
}

static u32 aml_bank_index(struct device_node *np)
{
	u32 value;

	if (of_property_read_u32(np, "bank-index", &value) < 0)
		return 0;
	else
		return value;
}

static unsigned int aml_count_pins(struct device_node *np)
{
	struct device_node *child;
	unsigned int pins = 0;

	for_each_child_of_node(np, child) {
		if (of_property_read_bool(child, "gpio-controller"))
			pins += aml_bank_pins(child);
	}

	return pins;
}

static void aml_pctl_dt_child_count(struct aml_pinctrl *info,
				    struct device_node *np)
{
	struct device_node *child;

	for_each_child_of_node(np, child) {
		if (of_property_read_bool(child, "gpio-controller")) {
			info->nbanks++;
		} else {
			info->nfunctions++;
			info->ngroups += of_get_child_count(child);
		}
	}
}

static struct regmap_config aml_regmap_config = {
	.reg_bits = 32,
	.val_bits = 32,
	.reg_stride = 4,
};

static struct regmap *aml_map_resource(struct aml_pinctrl *info,
				       struct device_node *node, char *name)
{
	struct resource res;
	void __iomem *base;
	int i;

	i = of_property_match_string(node, "reg-names", name);
	if (of_address_to_resource(node, i, &res))
		return NULL;

	base = devm_ioremap_resource(info->dev, &res);
	if (IS_ERR(base))
		return ERR_CAST(base);

	aml_regmap_config.max_register = resource_size(&res) - 4;
	aml_regmap_config.name = devm_kasprintf(info->dev, GFP_KERNEL,
						"%pOFn-%s", node,
						name);
	if (!aml_regmap_config.name)
		return ERR_PTR(-ENOMEM);

	return devm_regmap_init_mmio(info->dev, base, &aml_regmap_config);
}

static inline int aml_gpio_calc_reg_and_bit(struct aml_gpio_bank *bank,
					    enum aml_reg_type reg_type,
					    unsigned int gpio,
					    unsigned int *reg,
					    unsigned int *bit)
{
	*bit = gpio * aml_bit_strides[reg_type];
	*reg = (bank->pc.reg_offset[reg_type] + (*bit / 32)) * 4;
	*bit &= 0x1f;

	return 0;
}

static int aml_gpio_get_direction(struct gpio_chip *chip, unsigned int gpio)
{
	struct aml_gpio_bank *bank = gpiochip_get_data(chip);
	struct aml_pinctrl *info = dev_get_drvdata(chip->parent);
	unsigned int bit, reg, val;
	int ret;

	aml_gpio_calc_reg_and_bit(bank, REG_DIR, gpio, &reg, &bit);

	ret = regmap_read(info->reg_gpio, reg, &val);
	if (ret)
		return ret;

	return BIT(bit) & val ? GPIO_LINE_DIRECTION_OUT : GPIO_LINE_DIRECTION_IN;
}

static int aml_gpio_direction_input(struct gpio_chip *chip, unsigned int gpio)
{
	struct aml_gpio_bank *bank = gpiochip_get_data(chip);
	struct aml_pinctrl *info = dev_get_drvdata(chip->parent);
	unsigned int bit, reg;

	aml_gpio_calc_reg_and_bit(bank, REG_DIR, gpio, &reg, &bit);

	return regmap_update_bits(info->reg_gpio, reg, BIT(bit), BIT(bit));
}

static int aml_gpio_direction_output(struct gpio_chip *chip, unsigned int gpio,
				     int value)
{
	struct aml_gpio_bank *bank = gpiochip_get_data(chip);
	struct aml_pinctrl *info = dev_get_drvdata(chip->parent);
	unsigned int bit, reg;
	int ret;

	aml_gpio_calc_reg_and_bit(bank, REG_DIR, gpio, &reg, &bit);
	ret = regmap_update_bits(info->reg_gpio, reg, BIT(bit), BIT(bit));
	if (ret < 0)
		return ret;

	aml_gpio_calc_reg_and_bit(bank, REG_OUT, gpio, &reg, &bit);

	return regmap_update_bits(info->reg_gpio, reg, BIT(bit),
				  value ? BIT(bit) : 0);
}

static void aml_gpio_set(struct gpio_chip *chip, unsigned int gpio, int value)
{
	struct aml_gpio_bank *bank = gpiochip_get_data(chip);
	struct aml_pinctrl *info = dev_get_drvdata(chip->parent);
	unsigned int bit, reg;

	aml_gpio_calc_reg_and_bit(bank, REG_OUT, gpio, &reg, &bit);

	regmap_update_bits(info->reg_gpio, reg, BIT(bit),
			   value ? BIT(bit) : 0);
}

static int aml_gpio_get(struct gpio_chip *chip, unsigned int gpio)
{
	struct aml_gpio_bank *bank = gpiochip_get_data(chip);
	struct aml_pinctrl *info = dev_get_drvdata(chip->parent);
	unsigned int reg, bit, val;

	aml_gpio_calc_reg_and_bit(bank, REG_IN, gpio, &reg, &bit);
	regmap_read(info->reg_gpio, reg, &val);

	return !!(val & BIT(bit));
}

static const struct gpio_chip aml_gpio_template = {
	.request		= gpiochip_generic_request,
	.free			= gpiochip_generic_free,
	.set_config		= gpiochip_generic_config,
	.set			= aml_gpio_set,
	.get			= aml_gpio_get,
	.direction_input	= aml_gpio_direction_input,
	.direction_output	= aml_gpio_direction_output,
	.get_direction		= aml_gpio_get_direction,
	.can_sleep		= false,
};

static int aml_gpiolib_register_bank(struct aml_pinctrl *info,
				     int bank_nr, struct device_node *np,
				     int pin_next_index)
{
	struct aml_gpio_bank *bank = &info->banks[bank_nr];
	struct device *dev = info->dev;
	int err;
	int i;

	bank->pc.reg_gpio_offset = aml_bank_reg_gpio_offset(np);
	bank->pc.reg_mux_offset = aml_bank_reg_mux_offset(np);
	bank->pc.bit_mux_offset = aml_bank_bit_mux_offset(np);
	for (i = 0; i < NUM_REG; i++)
		bank->pc.reg_offset[i] = bank->pc.reg_gpio_offset + aml_reg_type_offset[i];

	bank->gpio_chip = aml_gpio_template;
	bank->gpio_chip.base = -1;
	bank->gpio_chip.ngpio = aml_bank_pins(np);
	bank->gpio_chip.fwnode = of_fwnode_handle(np);
	bank->gpio_chip.parent = dev;

	of_property_read_string(np, "bank-name", &bank->gpio_chip.label);

	bank->pin_base = pin_next_index;
	bank->bank_idx = aml_bank_index(np);

	err  = gpiochip_add_data(&bank->gpio_chip, bank);
	if (err)
		return dev_err_probe(dev, err, "Failed to add gpiochip(%d)!\n", bank_nr);

	dev_dbg(dev, "%s bank added.\n", bank->gpio_chip.label);

	return 0;
}

static void aml_pctl_reg_extra(struct aml_pinctrl *info)
{
	if (!info->reg_pull)
		info->reg_pull = info->reg_gpio;
	if (!info->reg_pullen)
		info->reg_pullen = info->reg_gpio;
	if (!info->reg_ds)
		info->reg_ds = info->reg_gpio;
}

static int aml_pctl_probe_dt(struct platform_device *pdev,
			     struct pinctrl_desc *pctl_desc,
			     struct aml_pinctrl *info)
{
	struct device *dev = &pdev->dev;
	struct pinctrl_pin_desc *pdesc;
	struct device_node *np = dev->of_node;
	int grp_index = 0;
	int i = 0, j = 0, k = 0, bank;
	int ret = 0;

	aml_pctl_dt_child_count(info, np);
	if (!info->nbanks)
		return dev_err_probe(dev, -EINVAL, "you need at least one gpio bank\n");

	dev_dbg(dev, "nbanks = %d\n", info->nbanks);
	dev_dbg(dev, "nfunctions = %d\n", info->nfunctions);
	dev_dbg(dev, "ngroups = %d\n", info->ngroups);

	info->functions = devm_kcalloc(dev, info->nfunctions, sizeof(*info->functions), GFP_KERNEL);

	info->groups = devm_kcalloc(dev, info->ngroups, sizeof(*info->groups), GFP_KERNEL);

	info->banks = devm_kcalloc(dev, info->nbanks, sizeof(*info->banks), GFP_KERNEL);

	if (!info->functions || !info->groups || !info->banks)
		return -ENOMEM;

	info->reg_mux = aml_map_resource(info, np, "mux");
	if (IS_ERR_OR_NULL(info->reg_mux))
		return dev_err_probe(dev, info->reg_mux ? PTR_ERR(info->reg_mux) : -ENOENT,
				     "mux registers not found\n");

	info->reg_gpio = aml_map_resource(info, np, "gpio");
	if (IS_ERR_OR_NULL(info->reg_gpio))
		return dev_err_probe(dev, info->reg_gpio ? PTR_ERR(info->reg_gpio) : -ENOENT,
				     "gpio registers not found\n");

	info->reg_pull = aml_map_resource(info, np, "pull");
	if (IS_ERR(info->reg_pull))
		info->reg_pull = NULL;

	info->reg_pullen = aml_map_resource(info, np, "pull-enable");
	if (IS_ERR(info->reg_pullen))
		info->reg_pullen = NULL;

	info->reg_ds = aml_map_resource(info, np, "ds");
	if (IS_ERR(info->reg_ds)) {
		dev_dbg(info->dev, "ds registers not found - skipping\n");
		info->reg_ds = NULL;
	}

	aml_pctl_reg_extra(info);

	info->data = (struct aml_pctl_data *)of_device_get_match_data(dev);

	pctl_desc->npins = aml_count_pins(np);

	pdesc =	devm_kcalloc(dev, pctl_desc->npins, sizeof(*pdesc), GFP_KERNEL);
	if (!pdesc)
		return -ENOMEM;

	pctl_desc->pins = pdesc;

	bank = 0;
	for_each_child_of_node_scoped(np, child) {
		if (of_property_read_bool(child, "gpio-controller")) {
			const char *bank_name = NULL;
			char **pin_names;

			ret = aml_gpiolib_register_bank(info, bank, child, k);
			if (ret)
				return ret;

			k = info->banks[bank].pin_base;
			bank_name = info->banks[bank].gpio_chip.label;

			pin_names = devm_kasprintf_strarray(dev, bank_name,
							    info->banks[bank].gpio_chip.ngpio);
			if (IS_ERR(pin_names))
				return PTR_ERR(pin_names);

			for (j = 0; j < info->banks[bank].gpio_chip.ngpio; j++, k++) {
				pdesc->number = k;
				pdesc->name = pin_names[j];
				pdesc++;
			}
			bank++;
		} else {
			ret = aml_pctl_parse_functions(child, info,
						       i++, &grp_index);
			if (ret)
				return ret;
		}
	}

	return 0;
}

static int aml_pctl_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct aml_pinctrl *info;
	struct pinctrl_desc *pctl_desc;
	int ret, i;

	pctl_desc = devm_kzalloc(dev, sizeof(*pctl_desc), GFP_KERNEL);
	if (!pctl_desc)
		return -ENOMEM;

	info = devm_kzalloc(dev, sizeof(*info), GFP_KERNEL);
	if (!info)
		return -ENOMEM;

	info->dev = dev;
	platform_set_drvdata(pdev, info);
	ret = aml_pctl_probe_dt(pdev, pctl_desc, info);
	if (ret)
		return ret;

	pctl_desc->owner	= THIS_MODULE;
	pctl_desc->pctlops	= &aml_pctrl_ops;
	pctl_desc->pmxops	= &aml_pmx_ops;
	pctl_desc->confops	= &aml_pinconf_ops;
	pctl_desc->name		= dev_name(dev);

	info->pctl = devm_pinctrl_register(dev, pctl_desc, info);
	if (IS_ERR(info->pctl)) {
		dev_err(dev, "Failed pinctrl registration\n");
		ret = PTR_ERR(info->pctl);
		goto error;
	}

	for (i = 0; i < info->nbanks; i++) {
		ret = gpiochip_add_pin_range(&info->banks[i].gpio_chip, dev_name(dev), 0,
					     info->banks[i].pin_base,
					     info->banks[i].gpio_chip.ngpio);
		if (ret < 0) {
			dev_err(dev, "Failed add pin range\n");
			goto error;
		}
	}

	return 0;
error:
	for (i = 0; i < info->nbanks; i++)
		gpiochip_remove(&info->banks[i].gpio_chip);
	return ret;
}

static const struct of_device_id aml_pctl_of_match[] = {
	{ .compatible = "amlogic,pinctrl", },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, aml_pctl_dt_match);

static struct platform_driver aml_pctl_driver = {
	.driver = {
		.name = "amlogic-pinctrl",
		.of_match_table = aml_pctl_of_match,
	},
	.probe = aml_pctl_probe,
};
module_platform_driver(aml_pctl_driver);

MODULE_AUTHOR("Xianwei Zhao <xianwei.zhao@amlogic.com>");
MODULE_DESCRIPTION("Pin controller and GPIO driver for Amlogic SoC");
MODULE_LICENSE("Dual BSD/GPL");
