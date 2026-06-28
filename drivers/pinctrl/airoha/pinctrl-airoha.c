// SPDX-License-Identifier: GPL-2.0-only
/*
 * Author: Lorenzo Bianconi <lorenzo@kernel.org>
 * Author: Benjamin Larsson <benjamin.larsson@genexis.eu>
 * Author: Markus Gothe <markus.gothe@genexis.eu>
 */

#include "airoha-common.h"

#define airoha_pinctrl_get_pullup_conf(pinctrl, pin, val)			\
	airoha_pinctrl_get_conf((pinctrl), AIROHA_PINCTRL_CONFS_PULLUP,		\
				(pin), (val))
#define airoha_pinctrl_get_pulldown_conf(pinctrl, pin, val)			\
	airoha_pinctrl_get_conf((pinctrl), AIROHA_PINCTRL_CONFS_PULLDOWN,	\
				(pin), (val))
#define airoha_pinctrl_get_drive_e2_conf(pinctrl, pin, val)			\
	airoha_pinctrl_get_conf((pinctrl), AIROHA_PINCTRL_CONFS_DRIVE_E2,	\
				(pin), (val))
#define airoha_pinctrl_get_drive_e4_conf(pinctrl, pin, val)			\
	airoha_pinctrl_get_conf((pinctrl), AIROHA_PINCTRL_CONFS_DRIVE_E4,	\
				(pin), (val))
#define airoha_pinctrl_get_pcie_rst_od_conf(pinctrl, pin, val)			\
	airoha_pinctrl_get_conf((pinctrl), AIROHA_PINCTRL_CONFS_PCIE_RST_OD,	\
				(pin), (val))
#define airoha_pinctrl_set_pullup_conf(pinctrl, pin, val)			\
	airoha_pinctrl_set_conf((pinctrl), AIROHA_PINCTRL_CONFS_PULLUP,		\
				(pin), (val))
#define airoha_pinctrl_set_pulldown_conf(pinctrl, pin, val)			\
	airoha_pinctrl_set_conf((pinctrl), AIROHA_PINCTRL_CONFS_PULLDOWN,	\
				(pin), (val))
#define airoha_pinctrl_set_drive_e2_conf(pinctrl, pin, val)			\
	airoha_pinctrl_set_conf((pinctrl), AIROHA_PINCTRL_CONFS_DRIVE_E2,	\
				(pin), (val))
#define airoha_pinctrl_set_drive_e4_conf(pinctrl, pin, val)			\
	airoha_pinctrl_set_conf((pinctrl), AIROHA_PINCTRL_CONFS_DRIVE_E4,	\
				(pin), (val))
#define airoha_pinctrl_set_pcie_rst_od_conf(pinctrl, pin, val)			\
	airoha_pinctrl_set_conf((pinctrl), AIROHA_PINCTRL_CONFS_PCIE_RST_OD,	\
				(pin), (val))

static const u32 gpio_data_regs[] = {
	REG_GPIO_DATA,
	REG_GPIO_DATA1
};

static const u32 gpio_out_regs[] = {
	REG_GPIO_OE,
	REG_GPIO_OE1
};

static const u32 gpio_dir_regs[] = {
	REG_GPIO_CTRL,
	REG_GPIO_CTRL1,
	REG_GPIO_CTRL2,
	REG_GPIO_CTRL3
};

static const u32 irq_status_regs[] = {
	REG_GPIO_INT,
	REG_GPIO_INT1
};

static const u32 irq_level_regs[] = {
	REG_GPIO_INT_LEVEL,
	REG_GPIO_INT_LEVEL1,
	REG_GPIO_INT_LEVEL2,
	REG_GPIO_INT_LEVEL3
};

static const u32 irq_edge_regs[] = {
	REG_GPIO_INT_EDGE,
	REG_GPIO_INT_EDGE1,
	REG_GPIO_INT_EDGE2,
	REG_GPIO_INT_EDGE3
};

static int airoha_convert_pin_to_reg_offset(struct pinctrl_dev *pctrl_dev,
					    struct pinctrl_gpio_range *range,
					    int pin)
{
	if (!range)
		range = pinctrl_find_gpio_range_from_pin_nolock(pctrl_dev,
								pin);
	if (!range)
		return -EINVAL;

	return pin - range->pin_base;
}

/* gpio callbacks */
static int airoha_gpio_set(struct gpio_chip *chip, unsigned int gpio,
			   int value)
{
	struct airoha_pinctrl *pinctrl = gpiochip_get_data(chip);
	u32 offset = gpio % AIROHA_PIN_BANK_SIZE;
	u8 index = gpio / AIROHA_PIN_BANK_SIZE;

	return regmap_update_bits(pinctrl->regmap,
				  pinctrl->gpiochip.data[index],
				  BIT(offset), value ? BIT(offset) : 0);
}

static int airoha_gpio_get(struct gpio_chip *chip, unsigned int gpio)
{
	struct airoha_pinctrl *pinctrl = gpiochip_get_data(chip);
	u32 val, pin = gpio % AIROHA_PIN_BANK_SIZE;
	u8 index = gpio / AIROHA_PIN_BANK_SIZE;
	int err;

	err = regmap_read(pinctrl->regmap,
			  pinctrl->gpiochip.data[index], &val);

	return err ? err : !!(val & BIT(pin));
}

static int airoha_gpio_direction_output(struct gpio_chip *chip,
					unsigned int gpio, int value)
{
	int err;

	err = pinctrl_gpio_direction_output(chip, gpio);
	if (err)
		return err;

	return airoha_gpio_set(chip, gpio, value);
}

/* irq callbacks */
static void airoha_irq_unmask(struct irq_data *data)
{
	u8 offset = data->hwirq % AIROHA_REG_GPIOCTRL_NUM_PIN;
	u8 index = data->hwirq / AIROHA_REG_GPIOCTRL_NUM_PIN;
	u32 mask = GENMASK(2 * offset + 1, 2 * offset);
	struct airoha_pinctrl_gpiochip *gpiochip;
	struct airoha_pinctrl *pinctrl;
	u32 val = BIT(2 * offset);

	gpiochip = irq_data_get_irq_chip_data(data);
	if (WARN_ON_ONCE(data->hwirq >= ARRAY_SIZE(gpiochip->irq_type)))
		return;

	pinctrl = container_of(gpiochip, struct airoha_pinctrl, gpiochip);
	switch (gpiochip->irq_type[data->hwirq]) {
	case IRQ_TYPE_LEVEL_LOW:
		val = val << 1;
		fallthrough;
	case IRQ_TYPE_LEVEL_HIGH:
		regmap_update_bits(pinctrl->regmap, gpiochip->level[index],
				   mask, val);
		break;
	case IRQ_TYPE_EDGE_FALLING:
		val = val << 1;
		fallthrough;
	case IRQ_TYPE_EDGE_RISING:
		regmap_update_bits(pinctrl->regmap, gpiochip->edge[index],
				   mask, val);
		break;
	case IRQ_TYPE_EDGE_BOTH:
		regmap_set_bits(pinctrl->regmap, gpiochip->edge[index], mask);
		break;
	default:
		break;
	}
}

static void airoha_irq_mask(struct irq_data *data)
{
	u8 offset = data->hwirq % AIROHA_REG_GPIOCTRL_NUM_PIN;
	u8 index = data->hwirq / AIROHA_REG_GPIOCTRL_NUM_PIN;
	u32 mask = GENMASK(2 * offset + 1, 2 * offset);
	struct airoha_pinctrl_gpiochip *gpiochip;
	struct airoha_pinctrl *pinctrl;

	gpiochip = irq_data_get_irq_chip_data(data);
	pinctrl = container_of(gpiochip, struct airoha_pinctrl, gpiochip);

	regmap_clear_bits(pinctrl->regmap, gpiochip->level[index], mask);
	regmap_clear_bits(pinctrl->regmap, gpiochip->edge[index], mask);
}

static int airoha_irq_type(struct irq_data *data, unsigned int type)
{
	struct airoha_pinctrl_gpiochip *gpiochip;

	gpiochip = irq_data_get_irq_chip_data(data);
	if (data->hwirq >= ARRAY_SIZE(gpiochip->irq_type))
		return -EINVAL;

	if (type == IRQ_TYPE_PROBE) {
		if (gpiochip->irq_type[data->hwirq])
			return 0;

		type = IRQ_TYPE_EDGE_RISING | IRQ_TYPE_EDGE_FALLING;
	}
	gpiochip->irq_type[data->hwirq] = type & IRQ_TYPE_SENSE_MASK;

	return 0;
}

static irqreturn_t airoha_irq_handler(int irq, void *data)
{
	struct airoha_pinctrl *pinctrl = data;
	bool handled = false;
	int i;

	for (i = 0; i < ARRAY_SIZE(irq_status_regs); i++) {
		struct gpio_irq_chip *girq = &pinctrl->gpiochip.chip.irq;
		u32 regmap;
		unsigned long status;
		int irq;

		if (regmap_read(pinctrl->regmap, pinctrl->gpiochip.status[i],
				&regmap))
			continue;

		status = regmap;
		for_each_set_bit(irq, &status, AIROHA_PIN_BANK_SIZE) {
			u32 offset = irq + i * AIROHA_PIN_BANK_SIZE;

			generic_handle_irq(irq_find_mapping(girq->domain,
							    offset));
			regmap_write(pinctrl->regmap,
				     pinctrl->gpiochip.status[i], BIT(irq));
		}
		handled |= !!status;
	}

	return handled ? IRQ_HANDLED : IRQ_NONE;
}

static int airoha_pinctrl_add_gpiochip(struct airoha_pinctrl *pinctrl,
					struct platform_device *pdev)
{
	struct airoha_pinctrl_gpiochip *chip = &pinctrl->gpiochip;
	struct irq_chip *irq_chip = &pinctrl->gpio_irq_chip;
	struct gpio_chip *gc = &chip->chip;
	struct gpio_irq_chip *girq = &gc->irq;
	struct device *dev = &pdev->dev;
	int irq, err;

	chip->data = gpio_data_regs;
	chip->dir = gpio_dir_regs;
	chip->out = gpio_out_regs;
	chip->status = irq_status_regs;
	chip->level = irq_level_regs;
	chip->edge = irq_edge_regs;

	gc->parent = dev;
	gc->label = dev_name(dev);
	gc->request = gpiochip_generic_request;
	gc->free = gpiochip_generic_free;
	gc->direction_input = pinctrl_gpio_direction_input;
	gc->direction_output = airoha_gpio_direction_output;
	gc->set = airoha_gpio_set;
	gc->get = airoha_gpio_get;
	gc->base = -1;
	gc->ngpio = AIROHA_NUM_PINS;

	irq_chip->name = "airoha-gpio-irq";
	irq_chip->irq_unmask = airoha_irq_unmask;
	irq_chip->irq_mask = airoha_irq_mask;
	irq_chip->irq_mask_ack = airoha_irq_mask;
	irq_chip->irq_set_type = airoha_irq_type;
	irq_chip->flags = IRQCHIP_SET_TYPE_MASKED | IRQCHIP_IMMUTABLE;

	girq->default_type = IRQ_TYPE_NONE;
	girq->handler = handle_simple_irq;
	gpio_irq_chip_set_chip(girq, irq_chip);

	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;

	err = devm_request_irq(dev, irq, airoha_irq_handler, IRQF_SHARED,
				dev_name(dev), pinctrl);
	if (err)
		return err;

	return devm_gpiochip_add_data(dev, gc, pinctrl);
}

/* pinmux callbacks */
static int airoha_pinmux_set_mux(struct pinctrl_dev *pctrl_dev,
				 unsigned int selector,
				 unsigned int group)
{
	struct airoha_pinctrl *pinctrl = pinctrl_dev_get_drvdata(pctrl_dev);
	const struct airoha_pinctrl_func *func;
	const struct function_desc *desc;
	struct group_desc *grp;
	int i;

	desc = pinmux_generic_get_function(pctrl_dev, selector);
	if (!desc)
		return -EINVAL;

	grp = pinctrl_generic_get_group(pctrl_dev, group);
	if (!grp)
		return -EINVAL;

	dev_dbg(pctrl_dev->dev, "enable function %s group %s\n",
		desc->func->name, grp->grp.name);

	func = desc->data;
	for (i = 0; i < func->group_size; i++) {
		const struct airoha_pinctrl_func_group *group;
		int j;

		group = &func->groups[i];
		if (strcmp(group->name, grp->grp.name))
			continue;

		for (j = 0; j < group->regmap_size; j++) {
			switch (group->regmap[j].mux) {
			case AIROHA_FUNC_PWM_EXT_MUX:
			case AIROHA_FUNC_PWM_MUX:
				regmap_update_bits(pinctrl->regmap,
						   group->regmap[j].offset,
						   group->regmap[j].mask,
						   group->regmap[j].val);
				break;
			default:
				regmap_update_bits(pinctrl->chip_scu,
						   group->regmap[j].offset,
						   group->regmap[j].mask,
						   group->regmap[j].val);
				break;
			}
		}
		return 0;
	}

	return -EINVAL;
}

static int airoha_pinmux_set_direction(struct pinctrl_dev *pctrl_dev,
					struct pinctrl_gpio_range *range,
					unsigned int p, bool input)
{
	struct airoha_pinctrl *pinctrl = pinctrl_dev_get_drvdata(pctrl_dev);
	u32 mask, index;
	int err, pin;

	pin = airoha_convert_pin_to_reg_offset(pctrl_dev, range, p);
	if (pin < 0)
		return pin;

	/* set output enable */
	mask = BIT(pin % AIROHA_PIN_BANK_SIZE);
	index = pin / AIROHA_PIN_BANK_SIZE;
	err = regmap_update_bits(pinctrl->regmap, pinctrl->gpiochip.out[index],
				 mask, !input ? mask : 0);
	if (err)
		return err;

	/* set direction */
	mask = BIT(2 * (pin % AIROHA_REG_GPIOCTRL_NUM_PIN));
	index = pin / AIROHA_REG_GPIOCTRL_NUM_PIN;
	return regmap_update_bits(pinctrl->regmap,
				  pinctrl->gpiochip.dir[index], mask,
				  !input ? mask : 0);
}

static const struct pinmux_ops airoha_pmxops = {
	.get_functions_count = pinmux_generic_get_function_count,
	.get_function_name = pinmux_generic_get_function_name,
	.get_function_groups = pinmux_generic_get_function_groups,
	.gpio_set_direction = airoha_pinmux_set_direction,
	.set_mux = airoha_pinmux_set_mux,
	.strict = true,
};

/* pinconf callbacks */
static const struct airoha_pinctrl_reg *
airoha_pinctrl_get_conf_reg(const struct airoha_pinctrl_conf *conf,
			    int conf_size, int pin)
{
	int i;

	for (i = 0; i < conf_size; i++) {
		if (conf[i].pin == pin)
			return &conf[i].reg;
	}

	return NULL;
}

static int airoha_pinctrl_get_conf(struct airoha_pinctrl *pinctrl,
				   enum airoha_pinctrl_confs_type conf_type,
				   int pin, u32 *val)
{
	const struct airoha_pinctrl_confs_info *confs_info;
	const struct airoha_pinctrl_reg *reg;

	confs_info = &pinctrl->confs_info[conf_type];

	reg = airoha_pinctrl_get_conf_reg(confs_info->confs,
					  confs_info->num_confs,
					  pin);
	if (!reg)
		return -EINVAL;

	if (regmap_read(pinctrl->chip_scu, reg->offset, val))
		return -EINVAL;

	*val = (*val & reg->mask) >> __ffs(reg->mask);

	return 0;
}

static int airoha_pinctrl_set_conf(struct airoha_pinctrl *pinctrl,
				   enum airoha_pinctrl_confs_type conf_type,
				   int pin, u32 val)
{
	const struct airoha_pinctrl_confs_info *confs_info;
	const struct airoha_pinctrl_reg *reg = NULL;

	confs_info = &pinctrl->confs_info[conf_type];

	reg = airoha_pinctrl_get_conf_reg(confs_info->confs,
					  confs_info->num_confs,
					  pin);
	if (!reg)
		return -EINVAL;


	if (regmap_update_bits(pinctrl->chip_scu, reg->offset, reg->mask,
				val << __ffs(reg->mask)))
		return -EINVAL;

	return 0;
}

static int airoha_pinconf_get_direction(struct pinctrl_dev *pctrl_dev, u32 p)
{
	struct airoha_pinctrl *pinctrl = pinctrl_dev_get_drvdata(pctrl_dev);
	u32 val, mask;
	int err, pin;
	u8 index;

	pin = airoha_convert_pin_to_reg_offset(pctrl_dev, NULL, p);
	if (pin < 0)
		return pin;

	index = pin / AIROHA_REG_GPIOCTRL_NUM_PIN;
	err = regmap_read(pinctrl->regmap, pinctrl->gpiochip.dir[index], &val);
	if (err)
		return err;

	mask = BIT(2 * (pin % AIROHA_REG_GPIOCTRL_NUM_PIN));
	return val & mask ? PIN_CONFIG_OUTPUT_ENABLE : PIN_CONFIG_INPUT_ENABLE;
}

static int airoha_pinconf_get(struct pinctrl_dev *pctrl_dev,
			      unsigned int pin, unsigned long *config)
{
	struct airoha_pinctrl *pinctrl = pinctrl_dev_get_drvdata(pctrl_dev);
	enum pin_config_param param = pinconf_to_config_param(*config);
	u32 arg;

	switch (param) {
	case PIN_CONFIG_BIAS_PULL_DOWN:
	case PIN_CONFIG_BIAS_DISABLE:
	case PIN_CONFIG_BIAS_PULL_UP: {
		u32 pull_up, pull_down;

		if (airoha_pinctrl_get_pullup_conf(pinctrl, pin, &pull_up) ||
		    airoha_pinctrl_get_pulldown_conf(pinctrl, pin, &pull_down))
			return -EINVAL;

		if (param == PIN_CONFIG_BIAS_PULL_UP &&
		    !(pull_up && !pull_down))
			return -EINVAL;
		else if (param == PIN_CONFIG_BIAS_PULL_DOWN &&
			 !(pull_down && !pull_up))
			return -EINVAL;
		else if (pull_up || pull_down)
			return -EINVAL;

		arg = 1;
		break;
	}
	case PIN_CONFIG_DRIVE_STRENGTH: {
		u32 e2, e4;

		if (airoha_pinctrl_get_drive_e2_conf(pinctrl, pin, &e2) ||
		    airoha_pinctrl_get_drive_e4_conf(pinctrl, pin, &e4))
			return -EINVAL;

		arg = e4 << 1 | e2;
		break;
	}
	case PIN_CONFIG_DRIVE_OPEN_DRAIN:
		if (airoha_pinctrl_get_pcie_rst_od_conf(pinctrl, pin, &arg))
			return -EINVAL;
		break;
	case PIN_CONFIG_OUTPUT_ENABLE:
	case PIN_CONFIG_INPUT_ENABLE:
		arg = airoha_pinconf_get_direction(pctrl_dev, pin);
		if (arg != param)
			return -EINVAL;

		arg = 1;
		break;
	default:
		return -ENOTSUPP;
	}

	*config = pinconf_to_config_packed(param, arg);

	return 0;
}

static int airoha_pinconf_set_pin_value(struct pinctrl_dev *pctrl_dev,
					unsigned int p, bool value)
{
	struct airoha_pinctrl *pinctrl = pinctrl_dev_get_drvdata(pctrl_dev);
	int pin;

	pin = airoha_convert_pin_to_reg_offset(pctrl_dev, NULL, p);
	if (pin < 0)
		return pin;

	return airoha_gpio_set(&pinctrl->gpiochip.chip, pin, value);
}

static int airoha_pinconf_set(struct pinctrl_dev *pctrl_dev,
			      unsigned int pin, unsigned long *configs,
			      unsigned int num_configs)
{
	struct airoha_pinctrl *pinctrl = pinctrl_dev_get_drvdata(pctrl_dev);
	int i;

	for (i = 0; i < num_configs; i++) {
		u32 param = pinconf_to_config_param(configs[i]);
		u32 arg = pinconf_to_config_argument(configs[i]);

		switch (param) {
		case PIN_CONFIG_BIAS_DISABLE:
			airoha_pinctrl_set_pulldown_conf(pinctrl, pin, 0);
			airoha_pinctrl_set_pullup_conf(pinctrl, pin, 0);
			break;
		case PIN_CONFIG_BIAS_PULL_UP:
			airoha_pinctrl_set_pulldown_conf(pinctrl, pin, 0);
			airoha_pinctrl_set_pullup_conf(pinctrl, pin, 1);
			break;
		case PIN_CONFIG_BIAS_PULL_DOWN:
			airoha_pinctrl_set_pulldown_conf(pinctrl, pin, 1);
			airoha_pinctrl_set_pullup_conf(pinctrl, pin, 0);
			break;
		case PIN_CONFIG_DRIVE_STRENGTH: {
			u32 e2 = 0, e4 = 0;

			switch (arg) {
			case MTK_DRIVE_2mA:
				break;
			case MTK_DRIVE_4mA:
				e2 = 1;
				break;
			case MTK_DRIVE_6mA:
				e4 = 1;
				break;
			case MTK_DRIVE_8mA:
				e2 = 1;
				e4 = 1;
				break;
			default:
				return -EINVAL;
			}

			airoha_pinctrl_set_drive_e2_conf(pinctrl, pin, e2);
			airoha_pinctrl_set_drive_e4_conf(pinctrl, pin, e4);
			break;
		}
		case PIN_CONFIG_DRIVE_OPEN_DRAIN:
			airoha_pinctrl_set_pcie_rst_od_conf(pinctrl, pin, !!arg);
			break;
		case PIN_CONFIG_OUTPUT_ENABLE:
		case PIN_CONFIG_INPUT_ENABLE:
		case PIN_CONFIG_LEVEL: {
			bool input = param == PIN_CONFIG_INPUT_ENABLE;
			int err;

			err = airoha_pinmux_set_direction(pctrl_dev, NULL, pin,
							  input);
			if (err)
				return err;

			if (param == PIN_CONFIG_LEVEL) {
				err = airoha_pinconf_set_pin_value(pctrl_dev,
								   pin, !!arg);
				if (err)
					return err;
			}
			break;
		}
		default:
			return -ENOTSUPP;
		}
	}

	return 0;
}

static int airoha_pinconf_group_get(struct pinctrl_dev *pctrl_dev,
				    unsigned int group, unsigned long *config)
{
	struct airoha_pinctrl *pinctrl = pinctrl_dev_get_drvdata(pctrl_dev);
	unsigned long cur_config = 0;
	int i;

	for (i = 0; i < pinctrl->grps[group].npins; i++) {
		if (airoha_pinconf_get(pctrl_dev,
					pinctrl->grps[group].pins[i],
					config))
			return -ENOTSUPP;

		if (i && cur_config != *config)
			return -ENOTSUPP;

		cur_config = *config;
	}

	return 0;
}

static int airoha_pinconf_group_set(struct pinctrl_dev *pctrl_dev,
				    unsigned int group, unsigned long *configs,
				    unsigned int num_configs)
{
	struct airoha_pinctrl *pinctrl = pinctrl_dev_get_drvdata(pctrl_dev);
	int i;

	for (i = 0; i < pinctrl->grps[group].npins; i++) {
		int err;

		err = airoha_pinconf_set(pctrl_dev,
					 pinctrl->grps[group].pins[i],
					 configs, num_configs);
		if (err)
			return err;
	}

	return 0;
}

static const struct pinconf_ops airoha_confops = {
	.is_generic = true,
	.pin_config_get = airoha_pinconf_get,
	.pin_config_set = airoha_pinconf_set,
	.pin_config_group_get = airoha_pinconf_group_get,
	.pin_config_group_set = airoha_pinconf_group_set,
	.pin_config_config_dbg_show = pinconf_generic_dump_config,
};

static const struct pinctrl_ops airoha_pctlops = {
	.get_groups_count = pinctrl_generic_get_group_count,
	.get_group_name = pinctrl_generic_get_group_name,
	.get_group_pins = pinctrl_generic_get_group_pins,
	.dt_node_to_map = pinconf_generic_dt_node_to_map_all,
	.dt_free_map = pinconf_generic_dt_free_map,
};

int airoha_pinctrl_probe(struct platform_device *pdev)
{
	const struct airoha_pinctrl_match_data *data;
	struct device *dev = &pdev->dev;
	struct airoha_pinctrl *pinctrl;
	struct regmap *map;
	int err, i;

	data = device_get_match_data(dev);
	if (!data)
		return -EINVAL;

	pinctrl = devm_kzalloc(dev, sizeof(*pinctrl), GFP_KERNEL);
	if (!pinctrl)
		return -ENOMEM;

	pinctrl->regmap = device_node_to_regmap(dev->parent->of_node);
	if (IS_ERR(pinctrl->regmap))
		return PTR_ERR(pinctrl->regmap);

	map = syscon_regmap_lookup_by_phandle(pdev->dev.of_node, "airoha,chip-scu");
	if (IS_ERR_OR_NULL(map)) {
		map = syscon_regmap_lookup_by_compatible(data->chip_scu_compatible);
		if (IS_ERR(map))
			return PTR_ERR(map);
	}

	pinctrl->chip_scu = map;

	/* Init pinctrl desc struct */
	pinctrl->desc.name = data->pinctrl_name;
	pinctrl->desc.owner = data->pinctrl_owner;
	pinctrl->desc.pctlops = &airoha_pctlops;
	pinctrl->desc.pmxops = &airoha_pmxops;
	pinctrl->desc.confops = &airoha_confops;
	pinctrl->desc.pins = data->pins;
	pinctrl->desc.npins = data->num_pins;

	err = devm_pinctrl_register_and_init(dev, &pinctrl->desc,
					     pinctrl, &pinctrl->ctrl);
	if (err)
		return err;

	/* build pin groups */
	for (i = 0; i < data->num_grps; i++) {
		const struct pingroup *grp = &data->grps[i];

		err = pinctrl_generic_add_group(pinctrl->ctrl, grp->name,
						grp->pins, grp->npins,
						(void *)grp);
		if (err < 0) {
			dev_err(&pdev->dev, "Failed to register group %s\n",
				grp->name);
			return err;
		}
	}

	/* build functions */
	for (i = 0; i < data->num_funcs; i++) {
		const struct airoha_pinctrl_func *func;

		func = &data->funcs[i];
		err = pinmux_generic_add_pinfunction(pinctrl->ctrl,
						     &func->desc,
						     (void *)func);
		if (err < 0) {
			dev_err(dev, "Failed to register function %s\n",
				func->desc.name);
			return err;
		}
	}

	pinctrl->grps = data->grps;
	pinctrl->funcs = data->funcs;
	pinctrl->confs_info = data->confs_info;

	err = pinctrl_enable(pinctrl->ctrl);
	if (err)
		return err;

	/* build gpio-chip */
	return airoha_pinctrl_add_gpiochip(pinctrl, pdev);
}
EXPORT_SYMBOL_GPL(airoha_pinctrl_probe);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Lorenzo Bianconi <lorenzo@kernel.org>");
MODULE_AUTHOR("Benjamin Larsson <benjamin.larsson@genexis.eu>");
MODULE_AUTHOR("Markus Gothe <markus.gothe@genexis.eu>");
MODULE_DESCRIPTION("Pinctrl common driver for Airoha SoC");
