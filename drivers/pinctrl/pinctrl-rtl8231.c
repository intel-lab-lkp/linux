// SPDX-License-Identifier: GPL-2.0-only

#include <linux/bitfield.h>
#include <linux/gpio/driver.h>
#include <linux/gpio/regmap.h>
#include <linux/module.h>
#include <linux/pinctrl/pinconf.h>
#include <linux/pinctrl/pinctrl.h>
#include <linux/pinctrl/pinmux.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>

#include "core.h"
#include "pinmux.h"
#include <linux/mfd/rtl8231.h>

#define RTL8231_NUM_GPIOS		37
#define RTL8231_DEBOUNCE_USEC		100000
#define RTL8231_DEBOUNCE_MIN_OFFSET	31

struct rtl8231_pin_ctrl {
	struct regmap *map;
};

/*
 * Pin controller functionality
 */
enum rtl8231_pin_function {
	RTL8231_PIN_FUNCTION_GPIO = BIT(0),
	RTL8231_PIN_FUNCTION_LED = BIT(1),
	RTL8231_PIN_FUNCTION_PWM = BIT(2),
};

struct rtl8231_function_info {
	enum rtl8231_pin_function flag;
	const char *name;
};

#define RTL8231_FUNCTION(_name, _flag)	\
((struct rtl8231_function_info) {	\
		.flag = (_flag),	\
		.name = (_name),	\
	})

static const struct rtl8231_function_info rtl8231_pin_functions[] = {
	RTL8231_FUNCTION("gpio", RTL8231_PIN_FUNCTION_GPIO),
	RTL8231_FUNCTION("led", RTL8231_PIN_FUNCTION_LED),
	RTL8231_FUNCTION("pwm", RTL8231_PIN_FUNCTION_PWM),
};

struct rtl8231_pin_desc {
	enum rtl8231_pin_function functions:8;
	u8 reg;
	u8 offset;
	u8 gpio_function_value;
};

#define RTL8231_PIN_DESC(_num, _func, _reg, _fld, _val)			\
	[(_num)] = ((struct rtl8231_pin_desc) {				\
		.functions = RTL8231_PIN_FUNCTION_GPIO | (_func),	\
		.reg = (_reg),						\
		.offset = (_fld),					\
		.gpio_function_value = (_val),				\
	})
#define RTL8231_GPIO_PIN_DESC(_num, _reg, _fld)			\
	RTL8231_PIN_DESC(_num, 0, _reg, _fld, RTL8231_PIN_MODE_GPIO)
#define RTL8231_LED_PIN_DESC(_num, _reg, _fld)			\
	RTL8231_PIN_DESC(_num, RTL8231_PIN_FUNCTION_LED, _reg, _fld, RTL8231_PIN_MODE_GPIO)
#define RTL8231_PWM_PIN_DESC(_num, _reg, _fld)			\
	RTL8231_PIN_DESC(_num, RTL8231_PIN_FUNCTION_PWM, _reg, _fld, 0)

/*
 * All pins have a GPIO/LED mux bit, but the bits for pins 35/36 are read-only. Use this bit
 * for the GPIO-only pin instead of a placeholder, so the rest of the logic can stay generic.
 */
static const struct rtl8231_pin_desc rtl8231_pin_data[RTL8231_NUM_GPIOS] = {
	RTL8231_LED_PIN_DESC(0, RTL8231_REG_PIN_MODE0, 0),
	RTL8231_LED_PIN_DESC(1, RTL8231_REG_PIN_MODE0, 1),
	RTL8231_LED_PIN_DESC(2, RTL8231_REG_PIN_MODE0, 2),
	RTL8231_LED_PIN_DESC(3, RTL8231_REG_PIN_MODE0, 3),
	RTL8231_LED_PIN_DESC(4, RTL8231_REG_PIN_MODE0, 4),
	RTL8231_LED_PIN_DESC(5, RTL8231_REG_PIN_MODE0, 5),
	RTL8231_LED_PIN_DESC(6, RTL8231_REG_PIN_MODE0, 6),
	RTL8231_LED_PIN_DESC(7, RTL8231_REG_PIN_MODE0, 7),
	RTL8231_LED_PIN_DESC(8, RTL8231_REG_PIN_MODE0, 8),
	RTL8231_LED_PIN_DESC(9, RTL8231_REG_PIN_MODE0, 9),
	RTL8231_LED_PIN_DESC(10, RTL8231_REG_PIN_MODE0, 10),
	RTL8231_LED_PIN_DESC(11, RTL8231_REG_PIN_MODE0, 11),
	RTL8231_LED_PIN_DESC(12, RTL8231_REG_PIN_MODE0, 12),
	RTL8231_LED_PIN_DESC(13, RTL8231_REG_PIN_MODE0, 13),
	RTL8231_LED_PIN_DESC(14, RTL8231_REG_PIN_MODE0, 14),
	RTL8231_LED_PIN_DESC(15, RTL8231_REG_PIN_MODE0, 15),
	RTL8231_LED_PIN_DESC(16, RTL8231_REG_PIN_MODE1, 0),
	RTL8231_LED_PIN_DESC(17, RTL8231_REG_PIN_MODE1, 1),
	RTL8231_LED_PIN_DESC(18, RTL8231_REG_PIN_MODE1, 2),
	RTL8231_LED_PIN_DESC(19, RTL8231_REG_PIN_MODE1, 3),
	RTL8231_LED_PIN_DESC(20, RTL8231_REG_PIN_MODE1, 4),
	RTL8231_LED_PIN_DESC(21, RTL8231_REG_PIN_MODE1, 5),
	RTL8231_LED_PIN_DESC(22, RTL8231_REG_PIN_MODE1, 6),
	RTL8231_LED_PIN_DESC(23, RTL8231_REG_PIN_MODE1, 7),
	RTL8231_LED_PIN_DESC(24, RTL8231_REG_PIN_MODE1, 8),
	RTL8231_LED_PIN_DESC(25, RTL8231_REG_PIN_MODE1, 9),
	RTL8231_LED_PIN_DESC(26, RTL8231_REG_PIN_MODE1, 10),
	RTL8231_LED_PIN_DESC(27, RTL8231_REG_PIN_MODE1, 11),
	RTL8231_LED_PIN_DESC(28, RTL8231_REG_PIN_MODE1, 12),
	RTL8231_LED_PIN_DESC(29, RTL8231_REG_PIN_MODE1, 13),
	RTL8231_LED_PIN_DESC(30, RTL8231_REG_PIN_MODE1, 14),
	RTL8231_LED_PIN_DESC(31, RTL8231_REG_PIN_MODE1, 15),
	RTL8231_LED_PIN_DESC(32, RTL8231_REG_PIN_HI_CFG, 0),
	RTL8231_LED_PIN_DESC(33, RTL8231_REG_PIN_HI_CFG, 1),
	RTL8231_LED_PIN_DESC(34, RTL8231_REG_PIN_HI_CFG, 2),
	RTL8231_PWM_PIN_DESC(35, RTL8231_REG_FUNC1, 3),
	RTL8231_GPIO_PIN_DESC(36, RTL8231_REG_PIN_HI_CFG, 4),
};
static const unsigned int PWM_PIN = 35;

#define RTL8231_PIN(_num)					\
	((struct pinctrl_pin_desc) {				\
		.number = (_num),				\
		.name = "gpio" #_num,				\
		.drv_data = (void *) &rtl8231_pin_data[(_num)]	\
	})

static const struct pinctrl_pin_desc rtl8231_pins[RTL8231_NUM_GPIOS] = {
	RTL8231_PIN(0),
	RTL8231_PIN(1),
	RTL8231_PIN(2),
	RTL8231_PIN(3),
	RTL8231_PIN(4),
	RTL8231_PIN(5),
	RTL8231_PIN(6),
	RTL8231_PIN(7),
	RTL8231_PIN(8),
	RTL8231_PIN(9),
	RTL8231_PIN(10),
	RTL8231_PIN(11),
	RTL8231_PIN(12),
	RTL8231_PIN(13),
	RTL8231_PIN(14),
	RTL8231_PIN(15),
	RTL8231_PIN(16),
	RTL8231_PIN(17),
	RTL8231_PIN(18),
	RTL8231_PIN(19),
	RTL8231_PIN(20),
	RTL8231_PIN(21),
	RTL8231_PIN(22),
	RTL8231_PIN(23),
	RTL8231_PIN(24),
	RTL8231_PIN(25),
	RTL8231_PIN(26),
	RTL8231_PIN(27),
	RTL8231_PIN(28),
	RTL8231_PIN(29),
	RTL8231_PIN(30),
	RTL8231_PIN(31),
	RTL8231_PIN(32),
	RTL8231_PIN(33),
	RTL8231_PIN(34),
	RTL8231_PIN(35),
	RTL8231_PIN(36),
};

static int rtl8231_get_groups_count(struct pinctrl_dev *pctldev)
{
	return ARRAY_SIZE(rtl8231_pins);
}

static const char *rtl8231_get_group_name(struct pinctrl_dev *pctldev, unsigned int selector)
{
	return rtl8231_pins[selector].name;
}

static int rtl8231_get_group_pins(struct pinctrl_dev *pctldev, unsigned int selector,
	const unsigned int **pins, unsigned int *num_pins)
{
	if (selector >= ARRAY_SIZE(rtl8231_pins))
		return -EINVAL;

	*pins = &rtl8231_pins[selector].number;
	*num_pins = 1;

	return 0;
}

static const struct pinctrl_ops rtl8231_pinctrl_ops = {
	.get_groups_count = rtl8231_get_groups_count,
	.get_group_name = rtl8231_get_group_name,
	.get_group_pins = rtl8231_get_group_pins,
	.dt_node_to_map = pinconf_generic_dt_node_to_map_all,
	.dt_free_map = pinconf_generic_dt_free_map,
};

static int rtl8231_set_mux(struct pinctrl_dev *pctldev, unsigned int func_selector,
	unsigned int group_selector)
{
	const struct function_desc *func = pinmux_generic_get_function(pctldev, func_selector);
	const struct rtl8231_pin_desc *desc = rtl8231_pins[group_selector].drv_data;
	const struct rtl8231_pin_ctrl *ctrl = pinctrl_dev_get_drvdata(pctldev);
	enum rtl8231_pin_function func_flag = (uintptr_t) func->data;
	unsigned int function_mask;
	unsigned int gpio_function;

	if (!(desc->functions & func_flag))
		return -EINVAL;

	function_mask = BIT(desc->offset);
	gpio_function = desc->gpio_function_value << desc->offset;

	if (func_flag == RTL8231_PIN_FUNCTION_GPIO)
		return regmap_update_bits(ctrl->map, desc->reg, function_mask, gpio_function);
	else
		return regmap_update_bits(ctrl->map, desc->reg, function_mask, ~gpio_function);
}

static int rtl8231_gpio_request_enable(struct pinctrl_dev *pctldev,
	struct pinctrl_gpio_range *range, unsigned int offset)
{
	const struct rtl8231_pin_desc *desc = rtl8231_pins[offset].drv_data;
	const struct rtl8231_pin_ctrl *ctrl = pinctrl_dev_get_drvdata(pctldev);
	unsigned int function_mask;
	unsigned int gpio_function;

	function_mask = BIT(desc->offset);
	gpio_function = desc->gpio_function_value << desc->offset;

	return regmap_update_bits(ctrl->map, desc->reg, function_mask, gpio_function);
}

static const struct pinmux_ops rtl8231_pinmux_ops = {
	.get_functions_count = pinmux_generic_get_function_count,
	.get_function_name = pinmux_generic_get_function_name,
	.get_function_groups = pinmux_generic_get_function_groups,
	.function_is_gpio = pinmux_generic_function_is_gpio,
	.set_mux = rtl8231_set_mux,
	.gpio_request_enable = rtl8231_gpio_request_enable,
	.strict = true,
};

static int rtl8231_pin_config_get(struct pinctrl_dev *pctldev, unsigned int offset,
	unsigned long *config)
{
	const struct rtl8231_pin_ctrl *ctrl = pinctrl_dev_get_drvdata(pctldev);
	unsigned int param = pinconf_to_config_param(*config);
	unsigned int arg;
	int err;
	int v;

	switch (param) {
	case PIN_CONFIG_INPUT_DEBOUNCE:
		if (offset < RTL8231_DEBOUNCE_MIN_OFFSET)
			return -EINVAL;

		err = regmap_read(ctrl->map, RTL8231_REG_FUNC1, &v);
		if (err)
			return err;

		v = FIELD_GET(RTL8231_FUNC1_DEBOUNCE_MASK, v);
		if (v & BIT(offset - RTL8231_DEBOUNCE_MIN_OFFSET))
			arg = RTL8231_DEBOUNCE_USEC;
		else
			arg = 0;
		break;
	default:
		return -ENOTSUPP;
	}

	*config = pinconf_to_config_packed(param, arg);

	return 0;
}

static int rtl8231_pin_config_set(struct pinctrl_dev *pctldev, unsigned int offset,
	unsigned long *configs, unsigned int num_configs)
{
	const struct rtl8231_pin_ctrl *ctrl = pinctrl_dev_get_drvdata(pctldev);
	unsigned int param, arg;
	unsigned int pin_mask;
	int err;
	int i;

	for (i = 0; i < num_configs; i++) {
		param = pinconf_to_config_param(configs[i]);
		arg = pinconf_to_config_argument(configs[i]);

		switch (param) {
		case PIN_CONFIG_INPUT_DEBOUNCE:
			if (offset < RTL8231_DEBOUNCE_MIN_OFFSET)
				return -EINVAL;

			pin_mask = FIELD_PREP(RTL8231_FUNC1_DEBOUNCE_MASK,
				BIT(offset - RTL8231_DEBOUNCE_MIN_OFFSET));

			switch (arg) {
			case 0:
				err = regmap_update_bits(ctrl->map, RTL8231_REG_FUNC1,
					pin_mask, 0);
				break;
			case RTL8231_DEBOUNCE_USEC:
				err = regmap_update_bits(ctrl->map, RTL8231_REG_FUNC1,
					pin_mask, pin_mask);
				break;
			default:
				return -EINVAL;
			}

			break;
		default:
			return -ENOTSUPP;
		}
	}

	return err;
}

static const struct pinconf_ops rtl8231_pinconf_ops = {
	.is_generic = true,
	.pin_config_get = rtl8231_pin_config_get,
	.pin_config_set = rtl8231_pin_config_set,
};

static int rtl8231_pinctrl_init_functions(struct pinctrl_dev *pctl,
	const struct pinctrl_desc *pctl_desc)
{
	enum rtl8231_pin_function flag;
	struct pinfunction func;
	const char **groups;
	unsigned int f_idx;
	const char *name;
	unsigned int pin;
	int num_groups;
	int err;

	for (f_idx = 0; f_idx < ARRAY_SIZE(rtl8231_pin_functions); f_idx++) {
		name = rtl8231_pin_functions[f_idx].name;
		flag = rtl8231_pin_functions[f_idx].flag;

		for (pin = 0, num_groups = 0; pin < pctl_desc->npins; pin++)
			if (rtl8231_pin_data[pin].functions & flag)
				num_groups++;

		groups = devm_kcalloc(pctl->dev, num_groups, sizeof(*groups), GFP_KERNEL);
		if (!groups)
			return -ENOMEM;

		for (pin = 0, num_groups = 0; pin < pctl_desc->npins; pin++)
			if (rtl8231_pin_data[pin].functions & flag)
				groups[num_groups++] = rtl8231_pins[pin].name;

		func = PINCTRL_PINFUNCTION(name, groups, num_groups);
		if (flag == RTL8231_PIN_FUNCTION_GPIO)
			func.flags |= PINFUNCTION_FLAG_GPIO;

		err = pinmux_generic_add_pinfunction(pctl, &func, (void *) ((uintptr_t) flag));
		if (err < 0)
			return err;
	}

	return 0;
}

struct pin_field_info {
	const struct reg_field gpio_dir;
	const struct reg_field mode;
};

static const struct pin_field_info pin_fields[] = {
	{
		.gpio_dir = REG_FIELD(RTL8231_REG_GPIO_DIR0, 0, 15),
		.mode = REG_FIELD(RTL8231_REG_PIN_MODE0, 0, 15),
	},
	{
		.gpio_dir = REG_FIELD(RTL8231_REG_GPIO_DIR1, 0, 15),
		.mode = REG_FIELD(RTL8231_REG_PIN_MODE1, 0, 15),
	},
	{
		.gpio_dir = REG_FIELD(RTL8231_REG_PIN_HI_CFG, 5, 9),
		.mode = REG_FIELD(RTL8231_REG_PIN_HI_CFG, 0, 4),
	},
};

static int rtl8231_configure_safe(struct device *dev, struct regmap *map)
{
	struct regmap_field *field_mode;
	struct regmap_field *field_dir;
	unsigned int is_input;
	unsigned int is_gpio;
	int err;

	for (unsigned int i = 0; i < ARRAY_SIZE(pin_fields); i++) {
		field_dir = devm_regmap_field_alloc(dev, map, pin_fields[i].gpio_dir);
		if (IS_ERR(field_dir))
			return PTR_ERR(field_dir);

		field_mode = devm_regmap_field_alloc(dev, map, pin_fields[i].mode);
		if (IS_ERR(field_mode))
			return PTR_ERR(field_mode);

		err = regmap_field_read(field_dir, &is_input);
		if (err)
			return err;

		err = regmap_field_read(field_mode, &is_gpio);
		if (err)
			return err;

		/* Enable field for PWM (on GPIO35) is in another register */
		if (pin_fields[i].mode.reg == RTL8231_REG_PIN_HI_CFG) {
			err = regmap_test_bits(map, rtl8231_pin_data[PWM_PIN].reg,
					BIT(rtl8231_pin_data[PWM_PIN].offset));
			if (err < 0)
				return err;

			if (err)
				is_gpio &= ~BIT(PWM_PIN % RTL8231_BITS_VAL);
		}

		/*
		 * Set every pin that is not muxed as a GPIO to gpio-in. That
		 * way the pin will be high impedance when it is muxed to GPIO,
		 * preventing unwanted glitches.
		 * The pin muxes are left as-is, so there are no signal changes.
		 */
		regmap_field_write(field_dir, is_input | ~is_gpio);

		devm_regmap_field_free(dev, field_dir);
		devm_regmap_field_free(dev, field_mode);
	}

	return 0;
}

static const struct pinctrl_desc rtl8231_pctl_desc = {
	.name = "rtl8231-pinctrl",
	.owner = THIS_MODULE,
	.confops = &rtl8231_pinconf_ops,
	.pctlops = &rtl8231_pinctrl_ops,
	.pmxops = &rtl8231_pinmux_ops,
	.npins = ARRAY_SIZE(rtl8231_pins),
	.pins = rtl8231_pins,
};

static int rtl8231_pinctrl_init(struct device *dev, struct rtl8231_pin_ctrl *ctrl)
{
	struct pinctrl_dev *pctldev;
	int err;

	err = devm_pinctrl_register_and_init(dev->parent, &rtl8231_pctl_desc, ctrl, &pctldev);
	if (err) {
		dev_err(dev, "failed to register pin controller\n");
		return err;
	}

	err = rtl8231_pinctrl_init_functions(pctldev, &rtl8231_pctl_desc);
	if (err)
		return err;

	err = pinctrl_enable(pctldev);
	if (err)
		dev_err(dev, "failed to enable pin controller\n");

	return err;
}

/*
 * GPIO controller functionality
 */
static int rtl8231_gpio_reg_mask_xlate(struct gpio_regmap *gpio, unsigned int base,
	unsigned int offset, unsigned int *reg, unsigned int *mask)
{
	unsigned int pin_mask = BIT(offset % RTL8231_BITS_VAL);

	if (base == RTL8231_REG_GPIO_DATA0 || offset < 32) {
		*reg = base + offset / RTL8231_BITS_VAL;
		*mask = pin_mask;
	} else if (base == RTL8231_REG_GPIO_DIR0) {
		*reg = RTL8231_REG_PIN_HI_CFG;
		*mask = FIELD_PREP(RTL8231_PIN_HI_CFG_DIR_MASK, pin_mask);
	} else {
		return -EINVAL;
	}

	return 0;
}

static int rtl8231_pinctrl_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct rtl8231_pin_ctrl *ctrl;
	struct gpio_regmap_config gpio_cfg = {};
	int err;

	ctrl = devm_kzalloc(dev, sizeof(*ctrl), GFP_KERNEL);
	if (!ctrl)
		return -ENOMEM;

	ctrl->map = dev_get_regmap(dev->parent, NULL);
	if (!ctrl->map)
		return -ENODEV;

	err = rtl8231_configure_safe(dev, ctrl->map);
	if (err)
		return err;

	err = rtl8231_pinctrl_init(dev, ctrl);
	if (err)
		return err;

	gpio_cfg.regmap = ctrl->map;
	gpio_cfg.parent = dev->parent;
	gpio_cfg.ngpio = RTL8231_NUM_GPIOS;
	gpio_cfg.ngpio_per_reg = RTL8231_BITS_VAL;

	gpio_cfg.reg_dat_base = GPIO_REGMAP_ADDR(RTL8231_REG_GPIO_DATA0);
	gpio_cfg.reg_set_base = GPIO_REGMAP_ADDR(RTL8231_REG_GPIO_DATA0);
	gpio_cfg.reg_dir_in_base = GPIO_REGMAP_ADDR(RTL8231_REG_GPIO_DIR0);

	gpio_cfg.reg_mask_xlate = rtl8231_gpio_reg_mask_xlate;

	return PTR_ERR_OR_ZERO(devm_gpio_regmap_register(dev, &gpio_cfg));
}

static struct platform_driver rtl8231_pinctrl_driver = {
	.driver = {
		.name = "rtl8231-pinctrl",
	},
	.probe = rtl8231_pinctrl_probe,
};
module_platform_driver(rtl8231_pinctrl_driver);

MODULE_AUTHOR("Sander Vanheule <sander@svanheule.net>");
MODULE_DESCRIPTION("Realtek RTL8231 pin control and GPIO support");
MODULE_LICENSE("GPL");
