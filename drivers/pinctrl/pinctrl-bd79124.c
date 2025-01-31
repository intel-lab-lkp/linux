// SPDX-License-Identifier: GPL-2.0-only
/*
 * ROHM BD79124 ADC / GPO pinmux.
 *
 * Copyright (c) 2025, ROHM Semiconductor.
 *
 */

#include <linux/device.h>
#include <linux/err.h>
#include <linux/gpio/driver.h>
#include <linux/module.h>
#include <linux/mod_devicetable.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/types.h>

#include <linux/pinctrl/pinctrl.h>
#include <linux/pinctrl/pinmux.h>
#include <linux/mfd/rohm-bd79124.h>
#include "pinctrl-utils.h"

/*
 * The driver expects pins top have 1 to 1 mapping to the groups.
 * Eg, pin 'ID 0' is AIN0, and can be directly mapped to the group "ain0", which
 * also uses group ID 0. The driver mix and match the pin and group IDs. This
 * works because we don't have any specific multi-pin groups. If I knew typical
 * use cases better I might've been able to create some funtionally meaningful
 * groups - but as I don't, I just decided to create per-pin groups for toggling
 * and individual pin to ADC-input or GPO mode. I believe this gives the
 * flexibility for generic use-cases.
 *
 * If this is a false assumption and special groups are needed, then the pin <=>
 * group mapping in this driver must be reworked. Meanwhile just keep the pin
 * and group IDs matching!
 */
static const struct pinctrl_pin_desc bd79124_pins[] = {
	PINCTRL_PIN(0, "ain0"),
	PINCTRL_PIN(1, "ain1"),
	PINCTRL_PIN(2, "ain2"),
	PINCTRL_PIN(3, "ain3"),
	PINCTRL_PIN(4, "ain4"),
	PINCTRL_PIN(5, "ain5"),
	PINCTRL_PIN(6, "ain6"),
	PINCTRL_PIN(7, "ain7"),
};

static const char * const bd79124_pin_groups[] = {
	"ain0",
	"ain1",
	"ain2",
	"ain3",
	"ain4",
	"ain5",
	"ain6",
	"ain7",
};

static int bd79124_get_groups_count(struct pinctrl_dev *pcdev)
{
	return ARRAY_SIZE(bd79124_pin_groups);
}

static const char *bd79124_get_group_name(struct pinctrl_dev *pctldev,
					   unsigned int group)
{
	return bd79124_pin_groups[group];
}

enum {
	BD79124_FUNC_GPO,
	BD79124_FUNC_ADC,
	BD79124_FUNC_AMOUNT
};

static const char * const bd79124_functions[BD79124_FUNC_AMOUNT] = {
	[BD79124_FUNC_GPO] = "gpo",
	[BD79124_FUNC_ADC] = "adc",
};

struct bd79124_mux_data {
	struct device *dev;
	struct regmap *map;
	struct pinctrl_dev *pcdev;
	struct gpio_chip gc;
};

static int bd79124_pmx_get_functions_count(struct pinctrl_dev *pcdev)
{
	return BD79124_FUNC_AMOUNT;
}

static const char *bd79124_pmx_get_function_name(struct pinctrl_dev *pcdev,
						  unsigned int selector)
{
	return bd79124_functions[selector];
}

static int bd79124_pmx_get_function_groups(struct pinctrl_dev *pcdev,
		unsigned int selector,
		const char * const **groups,
		unsigned int * const num_groups)
{
	*groups = &bd79124_pin_groups[0];
	*num_groups = ARRAY_SIZE(bd79124_pin_groups);

	return 0;
}

static int bd79124_pmx_set(struct pinctrl_dev *pcdev, unsigned int func,
			   unsigned int group)
{
	struct bd79124_mux_data *d = pinctrl_dev_get_drvdata(pcdev);

	/* We use 1 to 1 mapping for grp <=> pin */
	if (func == BD79124_FUNC_GPO)
		return regmap_set_bits(d->map, BD79124_REG_PINCFG, BIT(group));

	return regmap_clear_bits(d->map, BD79124_REG_PINCFG, BIT(group));
}

/*
 * Check that the pinmux has set this pin as GPO before allowing it to be used.
 * NOTE: There is no locking in the pinctrl driver to ensure the pin _stays_
 * appropriately muxed. It is the responsibility of the device using this GPO
 * (or ADC) to reserve the pin from the pinmux.
 */
static bool bd79124_is_gpo(struct bd79124_mux_data *d, unsigned int offset)
{
	int ret, val;

	ret = regmap_read(d->map, BD79124_REG_PINCFG, &val);
	/*
	 * If read fails, don't allow setting GPO value as we don't know if
	 * pin is used as AIN. (In which case we might upset the device being
	 * measured - although I suppose the BD79124 would ignore the set value
	 * if pin is used as AIN - but better safe than sorry, right?
	 */
	if (ret)
		return 0;

	return (val & BIT(offset));
}

static int bd79124gpo_direction_get(struct gpio_chip *gc, unsigned int offset)
{
	struct bd79124_mux_data *d = gpiochip_get_data(gc);

	if (!bd79124_is_gpo(d, offset))
		return -EINVAL;

	return GPIO_LINE_DIRECTION_OUT;
}

static void bd79124gpo_set(struct gpio_chip *gc, unsigned int offset, int value)
{
	struct bd79124_mux_data *d = gpiochip_get_data(gc);

	if (!bd79124_is_gpo(d, offset)) {
		dev_dbg(d->dev, "Bad GPO mux mode\n");
		return;
	}

	if (value)
		regmap_set_bits(d->map, BD79124_REG_GPO_VAL, BIT(offset));

	regmap_clear_bits(d->map, BD79124_REG_GPO_VAL, BIT(offset));
}

static void bd79124gpo_set_multiple(struct gpio_chip *gc, unsigned long *mask,
				   unsigned long *bits)
{
	int ret, val;
	struct bd79124_mux_data *d = gpiochip_get_data(gc);

	/* Ensure all GPIOs in 'mask' are set to be GPIOs */
	ret = regmap_read(d->map, BD79124_REG_PINCFG, &val);
	if (ret)
		return;

	if ((val & *mask) != *mask) {
		dev_dbg(d->dev, "Invalid mux config. Can't set value.\n");
		/* Do not set value for pins configured as ADC inputs */
		*mask &= val;
	}

	regmap_update_bits(d->map, BD79124_REG_GPO_VAL, *mask, *bits);
}

/* Template for GPIO chip */
static const struct gpio_chip bd79124gpo_chip = {
	.label			= "bd79124-gpo",
	.get_direction		= bd79124gpo_direction_get,
	.set			= bd79124gpo_set,
	.set_multiple		= bd79124gpo_set_multiple,
	.can_sleep		= true,
	.ngpio			= 8,
	.base			= -1,
};

static const struct pinmux_ops bd79124_pmxops = {
	.get_functions_count = bd79124_pmx_get_functions_count,
	.get_function_name = bd79124_pmx_get_function_name,
	.get_function_groups = bd79124_pmx_get_function_groups,
	.set_mux = bd79124_pmx_set,
};

static const struct pinctrl_ops bd79124_pctlops = {
	.get_groups_count = bd79124_get_groups_count,
	.get_group_name = bd79124_get_group_name,
	.dt_node_to_map = pinconf_generic_dt_node_to_map_all,
	.dt_free_map = pinctrl_utils_free_map,
};

static const struct pinctrl_desc bd79124_pdesc = {
	.name = "bd79124-pinctrl",
	.pins = &bd79124_pins[0],
	.npins = ARRAY_SIZE(bd79124_pins),
	.pmxops = &bd79124_pmxops,
	.pctlops = &bd79124_pctlops,
};

static int bd79124_probe(struct platform_device *pdev)
{
	struct bd79124_mux_data *d;
	int ret;

	d = devm_kzalloc(&pdev->dev, sizeof(*d), GFP_KERNEL);
	if (!d)
		return -ENOMEM;

	d->dev = &pdev->dev;
	d->map = dev_get_regmap(d->dev->parent, NULL);
	if (!d->map)
		return dev_err_probe(d->dev, -ENODEV, "No regmap\n");

	d->gc = bd79124gpo_chip;

	ret = devm_pinctrl_register_and_init(d->dev->parent,
			(struct pinctrl_desc *)&bd79124_pdesc, d, &d->pcdev);
	if (ret)
		return dev_err_probe(d->dev, ret, "pincontrol registration failed\n");
	ret = pinctrl_enable(d->pcdev);
	if (ret)
		return dev_err_probe(d->dev, ret, "pincontrol enabling failed\n");

	ret = devm_gpiochip_add_data(d->dev, &d->gc, d);
	if (ret)
		return dev_err_probe(d->dev, ret, "gpio init Failed\n");

	return 0;
}

static const struct platform_device_id bd79124_mux_id[] = {
	{ "bd79124-pinmux", },
	{ }
};
MODULE_DEVICE_TABLE(platform, bd79124_mux_id);

static struct platform_driver bd79124_mux_driver = {
	.driver = {
		.name = "bd79124-pinmux",
		/*
		 * Probing explicitly requires a few millisecond of sleep.
		 * Enabling the VDD regulator may include ramp up rates.
		 */
		.probe_type = PROBE_PREFER_ASYNCHRONOUS,
	},
	.probe = bd79124_probe,
	.id_table = bd79124_mux_id,
};
module_platform_driver(bd79124_mux_driver);

MODULE_AUTHOR("Matti Vaittinen <mazziesaccount@gmail.com>");
MODULE_DESCRIPTION("Pinmux/GPO Driver for ROHM BD79124");
MODULE_LICENSE("GPL");
