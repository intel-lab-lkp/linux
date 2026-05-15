// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2026 UltraRISC Technology (Shanghai) Co., Ltd.
 *
 * Author: Jia Wang <wangjia@ultrarisc.com>
 */

#include <linux/bitfield.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/pinctrl/pinconf.h>
#include <linux/pinctrl/pinconf-generic.h>
#include <linux/pinctrl/pinmux.h>
#include <linux/platform_device.h>
#include <linux/seq_file.h>
#include <linux/slab.h>

#include "../core.h"
#include "../devicetree.h"
#include "../pinconf.h"
#include "../pinctrl-utils.h"
#include "../pinmux.h"

#include "pinctrl-ultrarisc.h"

#define UR_CONF_BIT_PER_PIN	4
#define UR_CONF_PIN_PER_REG	(32 / UR_CONF_BIT_PER_PIN)
static const int ur_drive_strengths[] = { 20, 27, 33, 40 };

static int ur_pin_num_to_port_pin(const struct ur_pinctrl_match_data *match_data,
				  struct ur_pin_val *pin_val, u32 pin_num)
{
	for (u32 i = 0; i < match_data->num_ports; i++) {
		if (pin_num < match_data->ports[i].npins) {
			pin_val->port = i;
			pin_val->pin = pin_num;
			return 0;
		}
		pin_num -= match_data->ports[i].npins;
	}

	return -EINVAL;
}

static int ur_pin_to_desc(struct pinctrl_dev *pctldev, struct ur_pin_val *pin_val)
{
	struct ur_pinctrl *ur_pinctrl = pinctrl_dev_get_drvdata(pctldev);
	int index = 0;

	for (u32 i = 0; i < pin_val->port; i++)
		index += ur_pinctrl->match_data->ports[i].npins;

	return index + pin_val->pin;
}

static u32 ur_get_pin_conf_offset(const struct ur_port_desc *port_desc, u32 pin)
{
	return port_desc->conf_offset +
	       (pin / UR_CONF_PIN_PER_REG) * sizeof(u32);
}

static u32 ur_read_pin_conf(struct ur_pinctrl *pctldata, unsigned int pin)
{
	const struct ur_port_desc *port_desc;
	struct ur_pin_val pin_val;
	u32 reg_offset;
	u32 shift;
	u32 conf;
	u32 mask;

	if (ur_pin_num_to_port_pin(pctldata->match_data, &pin_val, pin))
		return 0;

	port_desc = &pctldata->match_data->ports[pin_val.port];
	reg_offset = ur_get_pin_conf_offset(port_desc, pin_val.pin);
	shift = (pin_val.pin % UR_CONF_PIN_PER_REG) * UR_CONF_BIT_PER_PIN;
	mask = GENMASK(UR_CONF_BIT_PER_PIN - 1, 0) << shift;
	conf = field_get(mask, readl_relaxed(pctldata->base + reg_offset));

	return conf;
}

static int ur_write_pin_conf(struct ur_pinctrl *pctldata, unsigned int pin, u32 conf)
{
	const struct ur_port_desc *port_desc;
	struct ur_pin_val pin_val;
	unsigned long flags;
	void __iomem *reg;
	u32 reg_offset;
	u32 val;
	u32 shift;
	u32 mask;

	if (ur_pin_num_to_port_pin(pctldata->match_data, &pin_val, pin))
		return -EINVAL;

	port_desc = &pctldata->match_data->ports[pin_val.port];
	reg_offset = ur_get_pin_conf_offset(port_desc, pin_val.pin);
	reg = pctldata->base + reg_offset;
	shift = (pin_val.pin % UR_CONF_PIN_PER_REG) * UR_CONF_BIT_PER_PIN;
	mask = GENMASK(UR_CONF_BIT_PER_PIN - 1, 0) << shift;

	raw_spin_lock_irqsave(&pctldata->lock, flags);
	val = readl_relaxed(reg);
	val = (val & ~mask) | field_prep(mask, conf);
	writel_relaxed(val, reg);
	raw_spin_unlock_irqrestore(&pctldata->lock, flags);

	return 0;
}

static int ur_set_pin_mux(struct ur_pinctrl *pctldata, struct ur_pin_val *pin_val)
{
	const struct ur_port_desc *port_desc = &pctldata->match_data->ports[pin_val->port];
	void __iomem *reg = pctldata->base + port_desc->func_offset;
	unsigned long flags;
	u32 val;

	raw_spin_lock_irqsave(&pctldata->lock, flags);
	val = readl_relaxed(reg);
	val &= ~((UR_FUNC0 | UR_FUNC1) << pin_val->pin);
	val |= pin_val->mode << pin_val->pin;
	writel_relaxed(val, reg);
	raw_spin_unlock_irqrestore(&pctldata->lock, flags);

	return 0;
}

static int ur_set_pin_mux_by_num(struct ur_pinctrl *pctldata, unsigned int pin, u32 mode)
{
	struct ur_pin_val pin_val = { .mode = mode };
	const struct ur_port_desc *port_desc;
	int ret;

	ret = ur_pin_num_to_port_pin(pctldata->match_data, &pin_val, pin);
	if (ret)
		return ret;

	port_desc = &pctldata->match_data->ports[pin_val.port];
	if (mode != UR_FUNC_DEF && !(port_desc->supported_modes & mode))
		return -EINVAL;

	return ur_set_pin_mux(pctldata, &pin_val);
}

static int ur_hw_to_config(unsigned long *config, u32 conf)
{
	enum pin_config_param param = pinconf_to_config_param(*config);
	u32 drive = FIELD_GET(UR_DRIVE_MASK, conf);
	u32 pull = FIELD_GET(UR_PULL_MASK, conf);

	switch (param) {
	case PIN_CONFIG_BIAS_DISABLE:
	case PIN_CONFIG_BIAS_HIGH_IMPEDANCE:
		if (pull != UR_PULL_DIS)
			return -EINVAL;
		*config = pinconf_to_config_packed(param, 1);
		return 0;
	case PIN_CONFIG_BIAS_PULL_UP:
		if (pull != UR_PULL_UP)
			return -EINVAL;
		*config = pinconf_to_config_packed(param, 1);
		return 0;
	case PIN_CONFIG_BIAS_PULL_DOWN:
	case PIN_CONFIG_BIAS_PULL_PIN_DEFAULT:
		if (pull != UR_PULL_DOWN)
			return -EINVAL;
		*config = pinconf_to_config_packed(param, 1);
		return 0;
	case PIN_CONFIG_DRIVE_STRENGTH:
		if (drive >= ARRAY_SIZE(ur_drive_strengths))
			return -EINVAL;
		*config = pinconf_to_config_packed(param, ur_drive_strengths[drive]);
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}

static int ur_config_to_hw(unsigned long config, u32 *conf)
{
	enum pin_config_param param = pinconf_to_config_param(config);
	u32 arg = pinconf_to_config_argument(config);

	switch (param) {
	case PIN_CONFIG_BIAS_DISABLE:
	case PIN_CONFIG_BIAS_HIGH_IMPEDANCE:
		FIELD_MODIFY(UR_PULL_MASK, conf, UR_PULL_DIS);
		return 0;
	case PIN_CONFIG_BIAS_PULL_UP:
		FIELD_MODIFY(UR_PULL_MASK, conf, UR_PULL_UP);
		return 0;
	case PIN_CONFIG_BIAS_PULL_DOWN:
	case PIN_CONFIG_BIAS_PULL_PIN_DEFAULT:
		FIELD_MODIFY(UR_PULL_MASK, conf, UR_PULL_DOWN);
		return 0;
	case PIN_CONFIG_DRIVE_STRENGTH:
		for (u32 i = 0; i < ARRAY_SIZE(ur_drive_strengths); i++) {
			if (ur_drive_strengths[i] != arg)
				continue;
			FIELD_MODIFY(UR_DRIVE_MASK, conf, i);
			return 0;
		}
		return -EINVAL;
	case PIN_CONFIG_DRIVE_PUSH_PULL:
	case PIN_CONFIG_INPUT_ENABLE:
	case PIN_CONFIG_OUTPUT_ENABLE:
	case PIN_CONFIG_PERSIST_STATE:
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}

struct ur_legacy_prop_data {
	struct ur_pin_val *pin_vals;
	unsigned int *group_pins;
	unsigned int num_pins;
};

static int ur_legacy_parse_prop(struct pinctrl_dev *pctldev,
				struct device_node *np,
				const char *propname,
				struct ur_legacy_prop_data *prop)
{
	struct ur_pinctrl *pctldata = pinctrl_dev_get_drvdata(pctldev);
	int rows;

	rows = pinctrl_count_index_with_args(np, propname);
	if (rows < 0)
		return dev_err_probe(pctldev->dev, rows, "%pOF: invalid %s count\n",
				     np, propname);

	prop->pin_vals = devm_kcalloc(pctldev->dev, rows, sizeof(*prop->pin_vals),
				      GFP_KERNEL);
	if (!prop->pin_vals)
		return -ENOMEM;

	prop->group_pins = devm_kcalloc(pctldev->dev, rows, sizeof(*prop->group_pins),
					GFP_KERNEL);
	if (!prop->group_pins)
		return -ENOMEM;

	prop->num_pins = rows;

	for (int i = 0; i < rows; i++) {
		struct of_phandle_args pin_args;
		int ret;

		ret = pinctrl_parse_index_with_args(np, propname, i, &pin_args);
		if (ret)
			return dev_err_probe(pctldev->dev, ret,
					     "%pOF: failed to parse %s[%d]\n",
					     np, propname, i);

		if (pin_args.args_count != 3)
			return dev_err_probe(pctldev->dev, -EINVAL,
					     "%pOF: invalid %s[%d] args_count=%d\n",
					     np, propname, i, pin_args.args_count);

		prop->pin_vals[i].port = pin_args.args[0];
		prop->pin_vals[i].pin = pin_args.args[1];
		prop->pin_vals[i].mode = pin_args.args[2];

		if (prop->pin_vals[i].port >= pctldata->match_data->num_ports)
			return dev_err_probe(pctldev->dev, -EINVAL,
					     "%pOF: invalid %s[%d] port=%u\n",
					     np, propname, i, prop->pin_vals[i].port);

		if (prop->pin_vals[i].pin >=
		    pctldata->match_data->ports[prop->pin_vals[i].port].npins)
			return dev_err_probe(pctldev->dev, -EINVAL,
					     "%pOF: invalid %s[%d] pin=%u\n",
					     np, propname, i, prop->pin_vals[i].pin);

		prop->group_pins[i] = ur_pin_to_desc(pctldev, &prop->pin_vals[i]);
	}

	return 0;
}

static const char *ur_legacy_get_function_name(const struct ur_pinctrl_match_data *match_data,
					       u32 mode)
{
	for (u32 i = 0; i < match_data->num_functions; i++) {
		if (match_data->functions[i].mode == mode)
			return match_data->functions[i].name;
	}

	return NULL;
}

static int ur_legacy_conf_to_configs(struct pinctrl_dev *pctldev, u32 conf,
				     unsigned long **configs,
				     unsigned int *num_configs)
{
	u32 drive = FIELD_GET(UR_DRIVE_MASK, conf);
	u32 pull = FIELD_GET(UR_PULL_MASK, conf);
	unsigned long config;
	int ret;

	switch (pull) {
	case UR_PULL_DIS:
		config = pinconf_to_config_packed(PIN_CONFIG_BIAS_DISABLE, 1);
		break;
	case UR_PULL_UP:
		config = pinconf_to_config_packed(PIN_CONFIG_BIAS_PULL_UP, 1);
		break;
	case UR_PULL_DOWN:
		config = pinconf_to_config_packed(PIN_CONFIG_BIAS_PULL_DOWN, 1);
		break;
	default:
		return -EINVAL;
	}

	ret = pinctrl_utils_add_config(pctldev, configs, num_configs, config);
	if (ret)
		return ret;

	if (drive >= ARRAY_SIZE(ur_drive_strengths))
		return -EINVAL;

	config = pinconf_to_config_packed(PIN_CONFIG_DRIVE_STRENGTH,
					  ur_drive_strengths[drive]);

	return pinctrl_utils_add_config(pctldev, configs, num_configs, config);
}

static int ur_legacy_add_mux_maps(struct pinctrl_dev *pctldev,
				  struct pinctrl_map **map,
				  unsigned int *reserved_maps,
				  unsigned int *num_maps,
				  const struct ur_legacy_prop_data *prop)
{
	struct ur_pinctrl *pctldata = pinctrl_dev_get_drvdata(pctldev);

	for (u32 i = 0; i < prop->num_pins; i++) {
		const char *function;
		const char *group;
		int ret;

		function = ur_legacy_get_function_name(pctldata->match_data,
						       prop->pin_vals[i].mode);
		if (!function)
			return -EINVAL;

		group = pctldata->match_data->pins[prop->group_pins[i]].name;
		if (!group)
			return -EINVAL;

		ret = pinctrl_utils_add_map_mux(pctldev, map, reserved_maps,
						num_maps, group, function);
		if (ret)
			return ret;
	}

	return 0;
}

static int ur_legacy_add_pinconf_maps(struct pinctrl_dev *pctldev,
				      struct pinctrl_map **map,
				      unsigned int *reserved_maps,
				      unsigned int *num_maps,
				      const struct ur_legacy_prop_data *prop)
{
	struct ur_pinctrl *pctldata = pinctrl_dev_get_drvdata(pctldev);

	for (u32 i = 0; i < prop->num_pins; i++) {
		unsigned long *configs = NULL;
		unsigned int num_configs = 0;
		const char *group;
		int ret;

		ret = ur_legacy_conf_to_configs(pctldev, prop->pin_vals[i].conf,
						&configs, &num_configs);
		if (ret)
			goto err;

		group = pctldata->match_data->pins[prop->group_pins[i]].name;
		if (!group) {
			ret = -EINVAL;
			goto err;
		}

		ret = pinctrl_utils_add_map_configs(pctldev, map, reserved_maps,
						    num_maps, group, configs,
						    num_configs,
						    PIN_MAP_TYPE_CONFIGS_PIN);
err:
		kfree(configs);
		if (ret)
			return ret;
	}

	return 0;
}

static int ur_legacy_dt_node_to_map(struct pinctrl_dev *pctldev,
				    struct device_node *np,
				    struct pinctrl_map **map,
				    unsigned int *num_maps)
{
	struct ur_legacy_prop_data conf_prop = {};
	struct ur_legacy_prop_data mux_prop = {};
	struct pinctrl_map *new_map = NULL;
	unsigned int reserved_maps = 0;
	unsigned int total_maps = 0;
	bool conf_present = false;
	bool mux_present = false;
	unsigned int map_num = 0;
	int ret;

	if (of_property_present(np, "pinctrl-pins"))
		mux_present = true;
	if (of_property_present(np, "pinconf-pins"))
		conf_present = true;
	if (!mux_present && !conf_present)
		return -EINVAL;

	if (mux_present) {
		ret = ur_legacy_parse_prop(pctldev, np, "pinctrl-pins", &mux_prop);
		if (ret)
			goto err;
		total_maps += mux_prop.num_pins;
	}

	if (conf_present) {
		ret = ur_legacy_parse_prop(pctldev, np, "pinconf-pins", &conf_prop);
		if (ret)
			goto err;
		total_maps += conf_prop.num_pins;
	}

	ret = pinctrl_utils_reserve_map(pctldev, &new_map, &reserved_maps,
					&map_num, total_maps);
	if (ret)
		goto err;

	if (mux_present) {
		ret = ur_legacy_add_mux_maps(pctldev, &new_map, &reserved_maps,
					     &map_num, &mux_prop);
		if (ret)
			goto err;
	}

	if (conf_present) {
		ret = ur_legacy_add_pinconf_maps(pctldev, &new_map, &reserved_maps,
						 &map_num, &conf_prop);
		if (ret)
			goto err;
	}

	*map = new_map;
	*num_maps = map_num;

	return 0;

err:
	pinctrl_utils_free_map(pctldev, new_map, map_num);
	return ret;
}

static int ur_generic_dt_node_to_map(struct pinctrl_dev *pctldev,
				     struct device_node *np_config,
				     struct pinctrl_map **map,
				     unsigned int *num_maps)
{
	return pinconf_generic_dt_node_to_map(pctldev, np_config, map, num_maps,
					      PIN_MAP_TYPE_INVALID);
}

static int ur_dt_node_to_map(struct pinctrl_dev *pctldev,
			     struct device_node *np,
			     struct pinctrl_map **map,
			     unsigned int *num_maps)
{
	bool legacy = of_property_present(np, "pinctrl-pins") ||
		      of_property_present(np, "pinconf-pins");
	bool generic = of_property_present(np, "pins");

	if (legacy && generic) {
		dev_err(pctldev->dev,
			"%pOF: mixed legacy and generic pinctrl properties are not supported\n",
			np);
		return -EINVAL;
	}

	if (generic)
		return ur_generic_dt_node_to_map(pctldev, np, map, num_maps);

	if (legacy)
		return ur_legacy_dt_node_to_map(pctldev, np, map, num_maps);

	return -EINVAL;
}

static void ur_dt_free_map(struct pinctrl_dev *pctldev,
			   struct pinctrl_map *map,
			   unsigned int num_maps)
{
	pinctrl_utils_free_map(pctldev, map, num_maps);
}

static void ur_pin_dbg_show(struct pinctrl_dev *pctldev,
			    struct seq_file *s, unsigned int offset)
{
	seq_printf(s, "%s", dev_name(pctldev->dev));
}

static const struct pinctrl_ops ur_pinctrl_ops = {
	.get_groups_count = pinctrl_generic_get_group_count,
	.get_group_name = pinctrl_generic_get_group_name,
	.get_group_pins = pinctrl_generic_get_group_pins,
	.dt_node_to_map = ur_dt_node_to_map,
	.dt_free_map = ur_dt_free_map,
	.pin_dbg_show = ur_pin_dbg_show,
};

static int ur_gpio_request_enable(struct pinctrl_dev *pctldev,
				  struct pinctrl_gpio_range *range,
				  unsigned int offset)
{
	struct ur_pinctrl *pctldata = pinctrl_dev_get_drvdata(pctldev);

	(void)range;

	return ur_set_pin_mux_by_num(pctldata, offset, UR_FUNC_DEF);
}

static int ur_set_mux(struct pinctrl_dev *pctldev, unsigned int func_selector,
		      unsigned int group_selector)
{
	struct ur_pinctrl *pctldata = pinctrl_dev_get_drvdata(pctldev);
	const struct ur_function_desc *desc;
	const struct function_desc *func;
	const unsigned int *pins;
	unsigned int npins;
	int ret;

	func = pinmux_generic_get_function(pctldev, func_selector);
	if (!func || !func->data)
		return -EINVAL;

	desc = func->data;
	ret = pinctrl_generic_get_group_pins(pctldev, group_selector, &pins, &npins);
	if (ret)
		return ret;

	for (u32 i = 0; i < npins; i++) {
		ret = ur_set_pin_mux_by_num(pctldata, pins[i], desc->mode);
		if (ret)
			return ret;
	}

	return 0;
}

static const struct pinmux_ops ur_pinmux_ops = {
	.get_functions_count = pinmux_generic_get_function_count,
	.get_function_name = pinmux_generic_get_function_name,
	.get_function_groups = pinmux_generic_get_function_groups,
	.function_is_gpio = pinmux_generic_function_is_gpio,
	.set_mux = ur_set_mux,
	.gpio_request_enable = ur_gpio_request_enable,
	.strict = true,
};

static int ur_pin_config_get(struct pinctrl_dev *pctldev,
			     unsigned int pin,
			     unsigned long *config)
{
	struct ur_pinctrl *pctldata = pinctrl_dev_get_drvdata(pctldev);

	return ur_hw_to_config(config, ur_read_pin_conf(pctldata, pin));
}

static int ur_pin_config_set(struct pinctrl_dev *pctldev,
			     unsigned int pin,
			     unsigned long *configs,
			     unsigned int num_configs)
{
	struct ur_pinctrl *pctldata = pinctrl_dev_get_drvdata(pctldev);
	u32 conf = ur_read_pin_conf(pctldata, pin);
	int ret;

	for (u32 i = 0; i < num_configs; i++) {
		ret = ur_config_to_hw(configs[i], &conf);
		if (ret)
			return ret;
	}

	return ur_write_pin_conf(pctldata, pin, conf);
}

static int ur_pin_config_group_get(struct pinctrl_dev *pctldev,
				   unsigned int selector,
				   unsigned long *config)
{
	const unsigned int *pins;
	unsigned int npins;
	int ret;

	ret = pinctrl_generic_get_group_pins(pctldev, selector, &pins, &npins);
	if (ret || !npins)
		return ret ?: -EINVAL;

	return ur_pin_config_get(pctldev, pins[0], config);
}

static int ur_pin_config_group_set(struct pinctrl_dev *pctldev,
				   unsigned int selector,
				   unsigned long *configs,
				   unsigned int num_configs)
{
	const unsigned int *pins;
	unsigned int npins;
	int ret;

	ret = pinctrl_generic_get_group_pins(pctldev, selector, &pins, &npins);
	if (ret)
		return ret;

	for (u32 i = 0; i < npins; i++) {
		ret = ur_pin_config_set(pctldev, pins[i], configs, num_configs);
		if (ret)
			return ret;
	}

	return 0;
}

static const struct pinconf_ops ur_pinconf_ops = {
	.pin_config_get = ur_pin_config_get,
	.pin_config_set = ur_pin_config_set,
	.pin_config_group_get = ur_pin_config_group_get,
	.pin_config_group_set = ur_pin_config_group_set,
#ifdef CONFIG_GENERIC_PINCONF
	.is_generic = true,
#endif
};

static int ur_add_pin_groups(struct ur_pinctrl *pctldata)
{
	for (u32 i = 0; i < pctldata->match_data->npins; i++) {
		int ret;

		pctldata->group_names[i] = pctldata->match_data->pins[i].name;
		pctldata->group_pins[i] = pctldata->match_data->pins[i].number;

		ret = pinctrl_generic_add_group(pctldata->pctl_dev, pctldata->group_names[i],
						&pctldata->group_pins[i], 1, NULL);
		if (ret < 0)
			return dev_err_probe(pctldata->dev, ret,
					     "failed to add pin group %s\n",
					     pctldata->group_names[i]);
	}

	return 0;
}

static int ur_add_functions(struct ur_pinctrl *pctldata)
{
	for (u32 i = 0; i < pctldata->match_data->num_functions; i++) {
		const struct ur_function_desc *desc = &pctldata->match_data->functions[i];
		struct pinfunction func = desc->gpio ?
			PINCTRL_GPIO_PINFUNCTION(desc->name, pctldata->group_names,
						 pctldata->match_data->npins) :
			PINCTRL_PINFUNCTION(desc->name, pctldata->group_names,
					    pctldata->match_data->npins);
		int ret;

		ret = pinmux_generic_add_pinfunction(pctldata->pctl_dev, &func, (void *)desc);
		if (ret < 0)
			return dev_err_probe(pctldata->dev, ret,
					     "failed to add function %s\n",
					     desc->name);
	}

	return 0;
}

int ur_pinctrl_probe(struct platform_device *pdev)
{
	const struct ur_pinctrl_match_data *match_data;
	struct ur_pinctrl *pctldata;
	struct pinctrl_desc *desc;
	int ret;

	match_data = of_device_get_match_data(&pdev->dev);
	if (!match_data)
		return -ENODEV;

	desc = devm_kzalloc(&pdev->dev, sizeof(*desc), GFP_KERNEL);
	if (!desc)
		return -ENOMEM;

	pctldata = devm_kzalloc(&pdev->dev, sizeof(*pctldata), GFP_KERNEL);
	if (!pctldata)
		return -ENOMEM;

	pctldata->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(pctldata->base))
		return PTR_ERR(pctldata->base);
	pctldata->dev = &pdev->dev;
	pctldata->match_data = match_data;
	pctldata->group_names = devm_kcalloc(&pdev->dev, match_data->npins,
					     sizeof(*pctldata->group_names), GFP_KERNEL);
	if (!pctldata->group_names)
		return -ENOMEM;

	pctldata->group_pins = devm_kcalloc(&pdev->dev, match_data->npins,
					    sizeof(*pctldata->group_pins), GFP_KERNEL);
	if (!pctldata->group_pins)
		return -ENOMEM;

	raw_spin_lock_init(&pctldata->lock);

	desc->name = dev_name(&pdev->dev);
	desc->owner = THIS_MODULE;
	desc->pins = match_data->pins;
	desc->npins = match_data->npins;
	desc->pctlops = &ur_pinctrl_ops;
	desc->pmxops = &ur_pinmux_ops;
	desc->confops = &ur_pinconf_ops;

	ret = devm_pinctrl_register_and_init(&pdev->dev, desc, pctldata, &pctldata->pctl_dev);
	if (ret)
		return dev_err_probe(&pdev->dev, ret, "failed to register pinctrl\n");

	ret = ur_add_pin_groups(pctldata);
	if (ret)
		return ret;

	ret = ur_add_functions(pctldata);
	if (ret)
		return ret;

	platform_set_drvdata(pdev, pctldata);

	return pinctrl_enable(pctldata->pctl_dev);
}
EXPORT_SYMBOL_GPL(ur_pinctrl_probe);

MODULE_DESCRIPTION("UltraRISC pinctrl core driver");
MODULE_LICENSE("GPL");
