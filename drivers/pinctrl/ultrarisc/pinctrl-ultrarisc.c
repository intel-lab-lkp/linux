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

static const struct ur_port_desc *ur_get_pin_port(struct pinctrl_dev *pctldev,
						  unsigned int pin)
{
	const struct pin_desc *desc = pin_desc_get(pctldev, pin);

	if (!desc || !desc->drv_data)
		return NULL;

	return desc->drv_data;
}

static u32 ur_get_pin_conf_offset(const struct ur_port_desc *port_desc, u32 pin)
{
	return port_desc->conf_offset +
	       (pin / UR_CONF_PIN_PER_REG) * sizeof(u32);
}

static u32 ur_read_pin_conf(struct ur_pinctrl *pctrl, unsigned int pin)
{
	const struct ur_port_desc *port_desc;
	struct ur_pin_val pin_val;
	u32 reg_offset;
	u32 shift;
	u32 conf;
	u32 mask;

	port_desc = ur_get_pin_port(pctrl->pctl_dev, pin);
	if (!port_desc)
		return 0;

	pin_val.port_desc = port_desc;
	pin_val.pin = pin - port_desc->pin_base;
	reg_offset = ur_get_pin_conf_offset(pin_val.port_desc, pin_val.pin);
	shift = (pin_val.pin % UR_CONF_PIN_PER_REG) * UR_CONF_BIT_PER_PIN;
	mask = GENMASK(UR_CONF_BIT_PER_PIN - 1, 0) << shift;
	conf = field_get(mask, readl_relaxed(pctrl->base + reg_offset));

	return conf;
}

static int ur_write_pin_conf(struct ur_pinctrl *pctrl, unsigned int pin, u32 conf)
{
	const struct ur_port_desc *port_desc;
	struct ur_pin_val pin_val;
	unsigned long flags;
	void __iomem *reg;
	u32 reg_offset;
	u32 shift;
	u32 mask;
	u32 val;

	port_desc = ur_get_pin_port(pctrl->pctl_dev, pin);
	if (!port_desc)
		return -EINVAL;

	pin_val.port_desc = port_desc;
	pin_val.pin = pin - port_desc->pin_base;
	reg_offset = ur_get_pin_conf_offset(pin_val.port_desc, pin_val.pin);
	reg = pctrl->base + reg_offset;
	shift = (pin_val.pin % UR_CONF_PIN_PER_REG) * UR_CONF_BIT_PER_PIN;
	mask = GENMASK(UR_CONF_BIT_PER_PIN - 1, 0) << shift;

	raw_spin_lock_irqsave(&pctrl->lock, flags);
	val = readl_relaxed(reg);
	val = (val & ~mask) | field_prep(mask, conf);
	writel_relaxed(val, reg);
	raw_spin_unlock_irqrestore(&pctrl->lock, flags);

	return 0;
}

static int ur_set_pin_mux(struct ur_pinctrl *pctrl, struct ur_pin_val *pin_val)
{
	void __iomem *reg = pctrl->base + pin_val->port_desc->func_offset;
	unsigned long flags;
	u32 val;

	if (WARN_ON(pin_val->pin >= UR_MAX_PINS_PER_PORT))
		return -EINVAL;

	raw_spin_lock_irqsave(&pctrl->lock, flags);
	val = readl_relaxed(reg);
	val &= ~((UR_FUNC_0 | UR_FUNC_1) << pin_val->pin);
	val |= pin_val->mode << pin_val->pin;
	writel_relaxed(val, reg);
	raw_spin_unlock_irqrestore(&pctrl->lock, flags);

	return 0;
}

static int ur_set_pin_mux_by_num(struct ur_pinctrl *pctrl, unsigned int pin, u32 mode)
{
	const struct ur_port_desc *port_desc = ur_get_pin_port(pctrl->pctl_dev, pin);
	struct ur_pin_val pin_val = { .mode = mode };

	if (!port_desc)
		return -EINVAL;

	if (mode != UR_FUNC_DEFAULT && !(port_desc->supported_modes & mode))
		return -EINVAL;

	pin_val.port_desc = port_desc;
	pin_val.pin = pin - port_desc->pin_base;

	return ur_set_pin_mux(pctrl, &pin_val);
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
		return -EINVAL;
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
	case PIN_CONFIG_PERSIST_STATE:
		/*
		 * For PIN_CONFIG_PERSIST_STATE, gpiolib only treats
		 * -ENOTSUPP as an optional unsupported result.
		 * Do not use -EOPNOTSUPP here.
		 */
		return -ENOTSUPP;
	default:
		return -EOPNOTSUPP;
	}
}

static const struct pinctrl_ops ur_pinctrl_ops = {
	.get_groups_count = pinctrl_generic_get_group_count,
	.get_group_name = pinctrl_generic_get_group_name,
	.get_group_pins = pinctrl_generic_get_group_pins,
	.dt_node_to_map = pinconf_generic_dt_node_to_map_all,
	.dt_free_map = pinconf_generic_dt_free_map,
};

static int ur_gpio_request_enable(struct pinctrl_dev *pctldev,
				  struct pinctrl_gpio_range *range,
				  unsigned int offset)
{
	struct ur_pinctrl *pctrl = pinctrl_dev_get_drvdata(pctldev);
	const struct ur_port_desc *port_desc;

	(void)range;

	port_desc = ur_get_pin_port(pctldev, offset);
	if (!port_desc || !port_desc->supports_gpio)
		return -EINVAL;

	return ur_set_pin_mux_by_num(pctrl, offset, UR_FUNC_DEFAULT);
}

static int ur_set_mux(struct pinctrl_dev *pctldev, unsigned int func_selector,
		      unsigned int group_selector)
{
	struct ur_pinctrl *pctrl = pinctrl_dev_get_drvdata(pctldev);
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
		if (!(desc->valid_pins & BIT_ULL(pins[i])))
			return -EINVAL;

		ret = ur_set_pin_mux_by_num(pctrl, pins[i], desc->mode);
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
	struct ur_pinctrl *pctrl = pinctrl_dev_get_drvdata(pctldev);

	return ur_hw_to_config(config, ur_read_pin_conf(pctrl, pin));
}

static int ur_pin_config_set(struct pinctrl_dev *pctldev,
			     unsigned int pin,
			     unsigned long *configs,
			     unsigned int num_configs)
{
	struct ur_pinctrl *pctrl = pinctrl_dev_get_drvdata(pctldev);
	u32 conf = ur_read_pin_conf(pctrl, pin);
	int ret;

	for (u32 i = 0; i < num_configs; i++) {
		ret = ur_config_to_hw(configs[i], &conf);
		if (ret)
			return ret;
	}

	return ur_write_pin_conf(pctrl, pin, conf);
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
	.pin_config_config_dbg_show = pinconf_generic_dump_config,
#endif
};

static int ur_add_pin_groups(struct ur_pinctrl *pctrl)
{
	for (u32 i = 0; i < pctrl->data->npins; i++) {
		int ret;

		pctrl->group_names[i] = pctrl->data->pins[i].name;
		pctrl->group_pins[i] = pctrl->data->pins[i].number;

		ret = pinctrl_generic_add_group(pctrl->pctl_dev, pctrl->group_names[i],
						&pctrl->group_pins[i], 1, NULL);
		if (ret < 0)
			return dev_err_probe(pctrl->dev, ret,
					     "failed to add pin group %s\n",
					     pctrl->group_names[i]);
	}

	return 0;
}

static int ur_collect_function_groups(struct ur_pinctrl *pctrl,
				      const struct ur_function_desc *desc,
				      const char ***groups,
				      u32 *num_groups)
{
	const char **func_groups;
	u32 count = 0;

	for (u32 i = 0; i < pctrl->data->npins; i++) {
		if (desc->valid_pins & BIT_ULL(pctrl->group_pins[i]))
			count++;
	}

	if (!count) {
		*groups = NULL;
		*num_groups = 0;
		return 0;
	}

	func_groups = devm_kcalloc(pctrl->dev, count, sizeof(*func_groups),
				   GFP_KERNEL);
	if (!func_groups)
		return -ENOMEM;

	*num_groups = 0;
	for (u32 i = 0; i < pctrl->data->npins; i++) {
		if (desc->valid_pins & BIT_ULL(pctrl->group_pins[i]))
			func_groups[(*num_groups)++] = pctrl->group_names[i];
	}

	*groups = func_groups;

	return 0;
}

static int ur_add_functions(struct ur_pinctrl *pctrl)
{
	for (u32 i = 0; i < pctrl->data->num_functions; i++) {
		const struct ur_function_desc *desc = &pctrl->data->functions[i];
		const char **func_groups;
		struct pinfunction func;
		u32 num_groups = 0;
		int ret;

		ret = ur_collect_function_groups(pctrl, desc, &func_groups,
						 &num_groups);
		if (ret)
			return ret;

		if (!num_groups)
			continue;

		func = desc->gpio ?
			PINCTRL_GPIO_PINFUNCTION(desc->name, func_groups, num_groups) :
			PINCTRL_PINFUNCTION(desc->name, func_groups, num_groups);

		ret = pinmux_generic_add_pinfunction(pctrl->pctl_dev, &func, (void *)desc);
		if (ret < 0)
			return dev_err_probe(pctrl->dev, ret,
					     "failed to add function %s\n",
					     desc->name);
	}

	return 0;
}

int ur_pinctrl_probe(struct platform_device *pdev,
		     const struct ur_pinctrl_data *data)
{
	struct pinctrl_desc *desc;
	struct ur_pinctrl *pctrl;
	int ret;

	if (!data)
		return -ENODEV;

	desc = devm_kzalloc(&pdev->dev, sizeof(*desc), GFP_KERNEL);
	if (!desc)
		return -ENOMEM;

	pctrl = devm_kzalloc(&pdev->dev, sizeof(*pctrl), GFP_KERNEL);
	if (!pctrl)
		return -ENOMEM;

	pctrl->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(pctrl->base))
		return PTR_ERR(pctrl->base);
	pctrl->dev = &pdev->dev;
	pctrl->data = data;
	pctrl->group_names = devm_kcalloc(&pdev->dev, data->npins,
					     sizeof(*pctrl->group_names), GFP_KERNEL);
	if (!pctrl->group_names)
		return -ENOMEM;

	pctrl->group_pins = devm_kcalloc(&pdev->dev, data->npins,
					    sizeof(*pctrl->group_pins), GFP_KERNEL);
	if (!pctrl->group_pins)
		return -ENOMEM;

	raw_spin_lock_init(&pctrl->lock);

	desc->name = dev_name(&pdev->dev);
	desc->owner = THIS_MODULE;
	desc->pins = data->pins;
	desc->npins = data->npins;
	desc->pctlops = &ur_pinctrl_ops;
	desc->pmxops = &ur_pinmux_ops;
	desc->confops = &ur_pinconf_ops;

	ret = devm_pinctrl_register_and_init(&pdev->dev, desc, pctrl, &pctrl->pctl_dev);
	if (ret)
		return dev_err_probe(&pdev->dev, ret, "failed to register pinctrl\n");

	ret = ur_add_pin_groups(pctrl);
	if (ret)
		return ret;

	ret = ur_add_functions(pctrl);
	if (ret)
		return ret;

	platform_set_drvdata(pdev, pctrl);

	return pinctrl_enable(pctrl->pctl_dev);
}
EXPORT_SYMBOL_GPL(ur_pinctrl_probe);

MODULE_DESCRIPTION("UltraRISC pinctrl core driver");
MODULE_LICENSE("GPL");
