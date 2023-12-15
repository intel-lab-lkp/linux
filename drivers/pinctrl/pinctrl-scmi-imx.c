// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2023 NXP
 */

#include <linux/device.h>
#include <linux/err.h>
#include <linux/of.h>
#include <linux/slab.h>

#include <linux/pinctrl/pinconf.h>
#include <linux/pinctrl/pinconf-generic.h>
#include <linux/pinctrl/pinctrl.h>
#include <linux/pinctrl/pinmux.h>

#include "pinctrl-scmi.h"
#include "pinctrl-utils.h"
#include "core.h"
#include "pinconf.h"

/* SCMI pin control types, aligned with SCMI firmware */
#define IMX_SCMI_PIN_TYPE_MUX		192
#define IMX_SCMI_PIN_TYPE_CONFIG	193
#define IMX_SCMI_PIN_TYPE_DAISY_ID	194
#define IMX_SCMI_PIN_TYPE_DAISY_CFG	195

#define IMX_SCMI_NO_PAD_CTL		BIT(31)
#define IMX_SCMI_PAD_SION		BIT(30)
#define IMX_SCMI_IOMUXC_CONFIG_SION	BIT(4)

#define IMX_SCMI_NUM_CFG	4
#define IMX_SCMI_PIN_SIZE	24

int pinctrl_scmi_imx_dt_node_to_map(struct pinctrl_dev *pctldev, struct device_node *np,
				    struct pinctrl_map **map, unsigned int *num_maps)
{
	struct pinctrl_map *new_map;
	const __be32 *list;
	unsigned long *configs = NULL;
	unsigned long cfg[IMX_SCMI_NUM_CFG];
	int map_num, size, pin_size, pin_id, num_pins;
	int mux_reg, conf_reg, input_reg, mux_val, conf_val, input_val;
	int i, j;
	uint32_t ncfg;
	static uint32_t daisy_off;

	if (!daisy_off) {
		if (of_machine_is_compatible("fsl,imx95"))
			daisy_off = 0x408;
		else
			dev_err(pctldev->dev, "platform not support scmi pinctrl\n");
	}

	list = of_get_property(np, "fsl,pins", &size);
	if (!list) {
		dev_err(pctldev->dev, "no fsl,pins property in node %pOF\n", np);
		return -EINVAL;
	}

	pin_size = IMX_SCMI_PIN_SIZE;

	if (!size || size % pin_size) {
		dev_err(pctldev->dev, "Invalid fsl,pins or pins property in node %pOF\n", np);
		return -EINVAL;
	}

	num_pins = size / pin_size;
	map_num = num_pins;

	new_map = kmalloc_array(map_num, sizeof(struct pinctrl_map),
				GFP_KERNEL);
	if (!new_map)
		return -ENOMEM;

	*map = new_map;
	*num_maps = map_num;

	/* create config map */
	for (i = 0; i < num_pins; i++) {
		j = 0;
		ncfg = IMX_SCMI_NUM_CFG;
		mux_reg = be32_to_cpu(*list++);
		conf_reg = be32_to_cpu(*list++);
		input_reg = be32_to_cpu(*list++);
		mux_val = be32_to_cpu(*list++);
		input_val = be32_to_cpu(*list++);
		conf_val = be32_to_cpu(*list++);
		if (conf_val & IMX_SCMI_PAD_SION)
			mux_val |= IMX_SCMI_IOMUXC_CONFIG_SION;

		pin_id = mux_reg / 4;

		cfg[j++] = pinconf_to_config_packed(IMX_SCMI_PIN_TYPE_MUX, mux_val);

		if (!conf_reg || (conf_val & IMX_SCMI_NO_PAD_CTL))
			ncfg--;
		else
			cfg[j++] = pinconf_to_config_packed(IMX_SCMI_PIN_TYPE_CONFIG, conf_val);

		if (!input_reg) {
			ncfg -= 2;
		} else {
			cfg[j++] = pinconf_to_config_packed(IMX_SCMI_PIN_TYPE_DAISY_ID,
							    (input_reg - daisy_off) / 4);
			cfg[j++] = pinconf_to_config_packed(IMX_SCMI_PIN_TYPE_DAISY_CFG, input_val);
		}

		configs = kmemdup(cfg, ncfg * sizeof(unsigned long), GFP_KERNEL);

		new_map[i].type = PIN_MAP_TYPE_CONFIGS_PIN;
		new_map[i].data.configs.group_or_pin = pin_get_name(pctldev, pin_id);
		new_map[i].data.configs.configs = configs;
		new_map[i].data.configs.num_configs = ncfg;
	}

	return 0;
}
