// SPDX-License-Identifier: GPL-2.0
/*
 * Marvell Berlin SoC pinctrl core driver
 *
 * Copyright (C) 2014 Marvell Technology Group Ltd.
 *
 * Antoine Ténart <antoine.tenart@free-electrons.com>
 */

#include <linux/io.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/pinctrl/pinconf-generic.h>
#include <linux/pinctrl/pinconf.h>
#include <linux/pinctrl/pinctrl.h>
#include <linux/pinctrl/pinmux.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/slab.h>

#include "../core.h"
#include "../pinconf.h"
#include "../pinctrl-utils.h"
#include "berlin.h"

#define PINCFG_DS_10BIT		GENMASK(3, 0)
#define PINCFG_IE_10BIT		BIT(4)
#define PINCFG_PE_10BIT		BIT(5)
#define PINCFG_PS_10BIT		BIT(6)
#define PINCFG_SL_10BIT		BIT(7)
#define PINCFG_SPU_10BIT	BIT(8)
#define PINCFG_ST_10BIT		BIT(9)
#define PINCFG_DS_8BIT		GENMASK(2, 0)
#define PINCFG_IE_8BIT		BIT(3)
#define PINCFG_PD_8BIT		BIT(4)
#define PINCFG_PU_8BIT		BIT(5)
#define PINCFG_SL_8BIT		BIT(6)
#define PINCFG_ST_8BIT		BIT(7)

struct berlin_pinctrl {
	struct regmap *regmap;
	struct device *dev;
	const struct berlin_pinctrl_desc *desc;
	struct pinfunction *functions;
	unsigned nfunctions;
	struct pinctrl_dev *pctrl_dev;
};

static int berlin_pinctrl_get_group_count(struct pinctrl_dev *pctrl_dev)
{
	struct berlin_pinctrl *pctrl = pinctrl_dev_get_drvdata(pctrl_dev);

	return pctrl->desc->ngroups;
}

static const char *berlin_pinctrl_get_group_name(struct pinctrl_dev *pctrl_dev,
						 unsigned group)
{
	struct berlin_pinctrl *pctrl = pinctrl_dev_get_drvdata(pctrl_dev);

	return pctrl->desc->groups[group].name;
}

static int berlin_pinctrl_dt_node_to_map(struct pinctrl_dev *pctrl_dev,
					 struct device_node *node,
					 struct pinctrl_map **map,
					 unsigned *num_maps)
{
	struct berlin_pinctrl *pctrl = pinctrl_dev_get_drvdata(pctrl_dev);
	struct property *prop;
	const char *function_name, *group_name;
	unsigned reserved_maps = 0;
	unsigned long *configs = NULL;
	unsigned int num_configs = 0;
	int ret, ngroups;

	*map = NULL;
	*num_maps = 0;

	ret = of_property_read_string(node, "function", &function_name);
	if (ret) {
		dev_err(pctrl->dev,
			"missing function property in node %pOFn\n", node);
		return -EINVAL;
	}

	ngroups = of_property_count_strings(node, "groups");
	if (ngroups < 0) {
		dev_err(pctrl->dev,
			"missing groups property in node %pOFn\n", node);
		return -EINVAL;
	}

	ret = pinconf_generic_parse_dt_config(node, pctrl_dev, &configs,
					      &num_configs);
	if (ret < 0) {
		dev_err(pctrl->dev, "%s: could not parse node property\n",
			node->name);
		return ret;
	}

	ret = pinctrl_utils_reserve_map(pctrl_dev, map, &reserved_maps,
					num_maps, ngroups * (num_configs + 1));
	if (ret) {
		dev_err(pctrl->dev, "can't reserve map: %d\n", ret);
		goto exit;
	}

	of_property_for_each_string(node, "groups", prop, group_name) {
		ret = pinctrl_utils_add_map_mux(pctrl_dev, map, &reserved_maps,
						num_maps, group_name,
						function_name);
		if (ret) {
			dev_err(pctrl->dev, "can't add map: %d\n", ret);
			goto exit;
		}

		if (num_configs) {
			ret = pinctrl_utils_add_map_configs(pctrl_dev, map,
					&reserved_maps, num_maps, group_name,
					configs, num_configs,
					PIN_MAP_TYPE_CONFIGS_GROUP);
			if (ret < 0)
				goto exit;
		}
	}

exit:
	kfree(configs);
	return ret;
}

static const struct pinctrl_ops berlin_pinctrl_ops = {
	.get_groups_count	= berlin_pinctrl_get_group_count,
	.get_group_name		= berlin_pinctrl_get_group_name,
	.dt_node_to_map		= berlin_pinctrl_dt_node_to_map,
	.dt_free_map		= pinctrl_utils_free_map,
};

static int berlin_pinmux_get_functions_count(struct pinctrl_dev *pctrl_dev)
{
	struct berlin_pinctrl *pctrl = pinctrl_dev_get_drvdata(pctrl_dev);

	return pctrl->nfunctions;
}

static const char *berlin_pinmux_get_function_name(struct pinctrl_dev *pctrl_dev,
						   unsigned function)
{
	struct berlin_pinctrl *pctrl = pinctrl_dev_get_drvdata(pctrl_dev);

	return pctrl->functions[function].name;
}

static int berlin_pinmux_get_function_groups(struct pinctrl_dev *pctrl_dev,
					     unsigned function,
					     const char * const **groups,
					     unsigned * const ngroups)
{
	struct berlin_pinctrl *pctrl = pinctrl_dev_get_drvdata(pctrl_dev);

	*groups = pctrl->functions[function].groups;
	*ngroups = pctrl->functions[function].ngroups;

	return 0;
}

static struct berlin_desc_function *
berlin_pinctrl_find_function_by_name(struct berlin_pinctrl *pctrl,
				     const struct berlin_desc_group *group,
				     const char *fname)
{
	struct berlin_desc_function *function = group->functions;

	while (function->name) {
		if (!strcmp(function->name, fname))
			return function;

		function++;
	}

	return NULL;
}

static int berlin_pinmux_set(struct pinctrl_dev *pctrl_dev,
			     unsigned function,
			     unsigned group)
{
	struct berlin_pinctrl *pctrl = pinctrl_dev_get_drvdata(pctrl_dev);
	const struct berlin_desc_group *group_desc = pctrl->desc->groups + group;
	struct pinfunction *func = pctrl->functions + function;
	struct berlin_desc_function *function_desc =
		berlin_pinctrl_find_function_by_name(pctrl, group_desc,
						     func->name);
	u32 mask, val;

	if (!function_desc)
		return -EINVAL;

	mask = GENMASK(group_desc->lsb + group_desc->bit_width - 1,
		       group_desc->lsb);
	val = function_desc->muxval << group_desc->lsb;
	regmap_update_bits(pctrl->regmap, group_desc->offset, mask, val);

	return 0;
}

static const struct pinmux_ops berlin_pinmux_ops = {
	.get_functions_count	= &berlin_pinmux_get_functions_count,
	.get_function_name	= &berlin_pinmux_get_function_name,
	.get_function_groups	= &berlin_pinmux_get_function_groups,
	.set_mux		= &berlin_pinmux_set,
};

static int berlin_pinconf_group_get(struct pinctrl_dev *pctrl_dev,
				    unsigned int group, unsigned long *configs)
{
	enum pin_config_param param = pinconf_to_config_param(*configs);
	struct berlin_pinctrl *pctrl = pinctrl_dev_get_drvdata(pctrl_dev);
	const struct berlin_desc_group *group_desc = pctrl->desc->groups + group;
	bool enabled;
	u32 val;
	int ret;
	u8 conf_bits = group_desc->conf_bits;

	if (!conf_bits)
		return -EOPNOTSUPP;

	ret = regmap_read(pctrl->regmap, group_desc->conf_offset, &val);
	if (ret)
		return ret;

	switch (param) {
	case PIN_CONFIG_BIAS_DISABLE:
		if (conf_bits == 10)
			enabled = !(val & PINCFG_PE_10BIT);
		else
			enabled = !(val & (PINCFG_PD_8BIT | PINCFG_PU_8BIT));
		val = 0;
		break;
	case PIN_CONFIG_BIAS_PULL_DOWN:
		if (conf_bits == 10)
			enabled = (val & (PINCFG_PE_10BIT | PINCFG_PS_10BIT)) == PINCFG_PE_10BIT;
		else
			enabled = val & PINCFG_PD_8BIT;
		val = enabled ? 1 : 0;
		break;
	case PIN_CONFIG_BIAS_PULL_UP:
		if (conf_bits == 10) {
			if ((val & (PINCFG_PE_10BIT | PINCFG_PS_10BIT)) == (PINCFG_PE_10BIT | PINCFG_PS_10BIT)) {
				enabled = true;
				if (val & PINCFG_SPU_10BIT)
					val = 2;
				else
					val = 1;
			} else {
				enabled = false;
				val = 0;
			}
		} else {
			enabled = val & PINCFG_PU_8BIT;
			val = enabled ? 1 : 0;
		}
		break;
	case PIN_CONFIG_DRIVE_STRENGTH:
		enabled = true;
		if (conf_bits == 10)
			val = FIELD_GET(PINCFG_DS_10BIT, val);
		else
			val = FIELD_GET(PINCFG_DS_8BIT, val);
		break;
	case PIN_CONFIG_INPUT_ENABLE:
		if (conf_bits == 10)
			enabled = val & PINCFG_IE_10BIT;
		else
			enabled = val & PINCFG_IE_8BIT;
		val = enabled ? 1 : 0;
		break;
	case PIN_CONFIG_INPUT_SCHMITT_ENABLE:
		if (conf_bits == 10)
			enabled = val & PINCFG_ST_10BIT;
		else
			enabled = val & PINCFG_ST_8BIT;
		val = enabled ? 1 : 0;
		break;
	case PIN_CONFIG_SLEW_RATE:
		if (conf_bits == 10)
			enabled = val & PINCFG_SL_10BIT;
		else
			enabled = val & PINCFG_SL_8BIT;
		val = enabled ? 1 : 0;
		break;
	default:
		return -EOPNOTSUPP;
	}

	*configs = pinconf_to_config_packed(param, val);
	return enabled ? 0 : -EINVAL;
}

static int berlin_pinconf_group_set(struct pinctrl_dev *pctrl_dev,
				    unsigned int group, unsigned long *configs,
				    unsigned int nconfigs)
{
	struct berlin_pinctrl *pctrl = pinctrl_dev_get_drvdata(pctrl_dev);
	const struct berlin_desc_group *group_desc = pctrl->desc->groups + group;
	u8 conf_bits = group_desc->conf_bits;
	u32 mask = 0, val = 0;
	int i;

	if (!conf_bits)
		return -EOPNOTSUPP;

	for (i = 0; i < nconfigs; i++) {
		enum pin_config_param param =
					pinconf_to_config_param(configs[i]);
		u32 arg = pinconf_to_config_argument(configs[i]);

		switch (param) {
		case PIN_CONFIG_BIAS_DISABLE:
			if (conf_bits == 10)
				mask |= (PINCFG_PE_10BIT | PINCFG_PS_10BIT | PINCFG_SPU_10BIT);
			else
				mask |= (PINCFG_PU_8BIT | PINCFG_PD_8BIT);
			break;
		case PIN_CONFIG_BIAS_PULL_DOWN:
			if (conf_bits == 10) {
				mask |= (PINCFG_PE_10BIT | PINCFG_PS_10BIT | PINCFG_SPU_10BIT);
				val |= PINCFG_PE_10BIT;
			} else {
				mask |= (PINCFG_PU_8BIT | PINCFG_PD_8BIT);
				val |= PINCFG_PD_8BIT;
			}
			break;
		case PIN_CONFIG_BIAS_PULL_UP:
			if (conf_bits == 10) {
				mask |= (PINCFG_PE_10BIT | PINCFG_PS_10BIT | PINCFG_SPU_10BIT);
				val |= (PINCFG_PE_10BIT | PINCFG_PS_10BIT);
				if (arg == 2)
					val |= PINCFG_SPU_10BIT;
			} else {
				mask |= (PINCFG_PU_8BIT | PINCFG_PD_8BIT);
				val |= PINCFG_PU_8BIT;
			}
			break;
		case PIN_CONFIG_DRIVE_STRENGTH:
			if (conf_bits == 10)
				mask |= PINCFG_DS_10BIT;
			else
				mask |= PINCFG_DS_8BIT;
			val |= arg;
			break;
		case PIN_CONFIG_INPUT_ENABLE:
			if (conf_bits == 10) {
				mask |= PINCFG_IE_10BIT;
				if (arg)
					val |= PINCFG_IE_10BIT;
			} else {
				mask |= PINCFG_IE_8BIT;
				if (arg)
					val |= PINCFG_IE_8BIT;
			}
			break;
		case PIN_CONFIG_INPUT_SCHMITT_ENABLE:
			if (conf_bits == 10) {
				mask |= PINCFG_ST_10BIT;
				if (arg)
					val |= PINCFG_ST_10BIT;
			} else {
				mask |= PINCFG_ST_8BIT;
				if (arg)
					val |= PINCFG_ST_8BIT;
			}
			break;
		case PIN_CONFIG_SLEW_RATE:
			if (conf_bits == 10) {
				mask |= PINCFG_SL_10BIT;
				if (arg)
					val |= PINCFG_SL_10BIT;
			} else {
				mask |= PINCFG_SL_8BIT;
				if (arg)
					val |= PINCFG_SL_8BIT;
			}
			break;
		default:
			return -EOPNOTSUPP;
		}
	}

	return regmap_update_bits(pctrl->regmap, group_desc->conf_offset, mask, val);
}

static const struct pinconf_ops berlin_confops = {
	.is_generic = true,
	.pin_config_group_get = berlin_pinconf_group_get,
	.pin_config_group_set = berlin_pinconf_group_set,
};

static int berlin_pinctrl_add_function(struct berlin_pinctrl *pctrl,
				       const char *name)
{
	struct pinfunction *function = pctrl->functions;

	while (function->name) {
		if (!strcmp(function->name, name)) {
			function->ngroups++;
			return -EEXIST;
		}
		function++;
	}

	function->name = name;
	function->ngroups = 1;

	pctrl->nfunctions++;

	return 0;
}

static int berlin_pinctrl_build_state(struct platform_device *pdev)
{
	struct berlin_pinctrl *pctrl = platform_get_drvdata(pdev);
	const struct berlin_desc_group *desc_group;
	const struct berlin_desc_function *desc_function;
	int i, max_functions = 0;
	struct pinfunction *new_functions;

	pctrl->nfunctions = 0;

	for (i = 0; i < pctrl->desc->ngroups; i++) {
		desc_group = pctrl->desc->groups + i;
		/* compute the maximum number of functions a group can have */
		max_functions += 1 << (desc_group->bit_width + 1);
	}

	/* we will reallocate later */
	pctrl->functions = devm_kcalloc(&pdev->dev, max_functions,
					sizeof(*pctrl->functions), GFP_KERNEL);
	if (!pctrl->functions)
		return -ENOMEM;

	/* register all functions */
	for (i = 0; i < pctrl->desc->ngroups; i++) {
		desc_group = pctrl->desc->groups + i;
		desc_function = desc_group->functions;

		while (desc_function->name) {
			berlin_pinctrl_add_function(pctrl, desc_function->name);
			desc_function++;
		}
	}

	new_functions = devm_krealloc(&pdev->dev, pctrl->functions,
				      pctrl->nfunctions * sizeof(*pctrl->functions),
				      GFP_KERNEL);
	if (!new_functions)
		return -ENOMEM;

	pctrl->functions = new_functions;
	/* map functions to theirs groups */
	for (i = 0; i < pctrl->desc->ngroups; i++) {
		desc_group = pctrl->desc->groups + i;
		desc_function = desc_group->functions;

		while (desc_function->name) {
			struct pinfunction *function = pctrl->functions;
			const char **groups;
			bool found = false;

			while (function->name) {
				if (!strcmp(desc_function->name, function->name)) {
					found = true;
					break;
				}
				function++;
			}

			if (!found)
				return -EINVAL;

			if (!function->groups) {
				function->groups =
					devm_kcalloc(&pdev->dev,
						     function->ngroups,
						     sizeof(*function->groups),
						     GFP_KERNEL);

				if (!function->groups)
					return -ENOMEM;
			}

			groups = (const char **)function->groups;
			while (*groups)
				groups++;

			*groups = desc_group->name;

			desc_function++;
		}
	}

	return 0;
}

static const struct pinctrl_desc berlin_pctrl_desc = {
	.name		= "berlin-pinctrl",
	.pctlops	= &berlin_pinctrl_ops,
	.pmxops		= &berlin_pinmux_ops,
	.confops	= &berlin_confops,
	.owner		= THIS_MODULE,
};

int berlin_pinctrl_probe_regmap(struct platform_device *pdev,
				const struct berlin_pinctrl_desc *desc,
				struct regmap *regmap)
{
	struct device *dev = &pdev->dev;
	struct berlin_pinctrl *pctrl;
	int ret;

	pctrl = devm_kzalloc(dev, sizeof(*pctrl), GFP_KERNEL);
	if (!pctrl)
		return -ENOMEM;

	platform_set_drvdata(pdev, pctrl);

	pctrl->regmap = regmap;
	pctrl->dev = &pdev->dev;
	pctrl->desc = desc;

	ret = berlin_pinctrl_build_state(pdev);
	if (ret) {
		dev_err(dev, "cannot build driver state: %d\n", ret);
		return ret;
	}

	pctrl->pctrl_dev = devm_pinctrl_register(dev, &berlin_pctrl_desc,
						 pctrl);
	if (IS_ERR(pctrl->pctrl_dev)) {
		dev_err(dev, "failed to register pinctrl driver\n");
		return PTR_ERR(pctrl->pctrl_dev);
	}

	return 0;
}
EXPORT_SYMBOL_GPL(berlin_pinctrl_probe_regmap);

int berlin_pinctrl_probe(struct platform_device *pdev,
			 const struct berlin_pinctrl_desc *desc)
{
	struct device *dev = &pdev->dev;
	struct device_node *parent_np = of_get_parent(dev->of_node);
	struct regmap *regmap = syscon_node_to_regmap(parent_np);

	of_node_put(parent_np);
	if (IS_ERR(regmap))
		return PTR_ERR(regmap);

	return berlin_pinctrl_probe_regmap(pdev, desc, regmap);
}
EXPORT_SYMBOL_GPL(berlin_pinctrl_probe);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("pinctrl library for Synaptics SoCs");
