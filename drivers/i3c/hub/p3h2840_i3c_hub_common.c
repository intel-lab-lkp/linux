// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2025 NXP
 * This P3H2x4x driver file implements functions for Hub probe and DT parsing.
 */

#include "p3h2840_i3c_hub.h"

/* LDO voltage DT settings */
#define P3H2x4x_DT_LDO_VOLT_1_0V                1000
#define P3H2x4x_DT_LDO_VOLT_1_1V		1100
#define P3H2x4x_DT_LDO_VOLT_1_2V		1200
#define P3H2x4x_DT_LDO_VOLT_1_8V		1800

/* target port pull-up settings */
#define P3H2x4x_DT_TP_PULLUP_250R		250
#define P3H2x4x_DT_TP_PULLUP_500R		500
#define P3H2x4x_DT_TP_PULLUP_1000R		1000
#define P3H2x4x_DT_TP_PULLUP_2000R		2000

/*  IO strenght settings */
#define P3H2x4x_DT_IO_STRENGTH_20_OHM		20
#define P3H2x4x_DT_IO_STRENGTH_30_OHM		30
#define P3H2x4x_DT_IO_STRENGTH_40_OHM		40
#define P3H2x4x_DT_IO_STRENGTH_50_OHM		50

/* target port mode settings */
static const struct p3h2x4x_setting tp_mode_settings[] = {
	{ "i3c",		P3H2x4x_TP_MODE_I3C },
	{ "smbus",		P3H2x4x_TP_MODE_SMBUS },
	{ "gpio",		P3H2x4x_TP_MODE_GPIO },
	{ "i2c",		P3H2x4x_TP_MODE_I2C },
};

static const struct i3c_ibi_setup p3h2x4x_ibireq = {
		.handler = p3h2x4x_ibi_handler,
		.max_payload_len = P3H2x4x_MAX_PAYLOAD_LEN,
		.num_slots = P3H2x4x_NUM_SLOTS,
};

static const struct regmap_config p3h2x4x_regmap_config = {
		.reg_bits = P3H2x4x_REG_BITS,
		.val_bits = P3H2x4x_VAL_BITS,
};

static void p3h2x4x_of_get_dt_setting(struct device *dev,
				      const struct device_node *node,
				      const char *setting_name,
				      const struct p3h2x4x_setting settings[],
				      const int settings_count,
				      int *setting_value)
{
	const char *sval;
	int ret;
	int i;

	ret = of_property_read_string(node, setting_name, &sval);
	if (ret) {
		if (ret != -EINVAL)
			dev_warn(dev, "No setting or invalid setting for %s, err=%i\n",
				 setting_name, ret);
		return;
	}

	for (i = 0; i < settings_count; ++i) {
		const struct p3h2x4x_setting *const setting = &settings[i];

		if (!strcmp(setting->name, sval)) {
			*setting_value = setting->value;
			return;
		}
	}
	dev_warn(dev, "Unknown setting for %s\n", setting_name);
}

static u8 p3h2x4x_ldo_dt_to_reg(int dt_value)
{
	switch (dt_value) {
	case P3H2x4x_DT_LDO_VOLT_1_8V:
		return P3H2x4x_LDO_VOLT_1_8V;
	case P3H2x4x_DT_LDO_VOLT_1_2V:
		return P3H2x4x_LDO_VOLT_1_2V;
	case P3H2x4x_DT_LDO_VOLT_1_1V:
		return P3H2x4x_LDO_VOLT_1_1V;
	default:
		return P3H2x4x_LDO_VOLT_1_0V;
	}
}

static u8 p3h2x4x_pullup_dt_to_reg(int dt_value)
{
	switch (dt_value) {
	case P3H2x4x_DT_TP_PULLUP_2000R:
		return P3H2x4x_TP_PULLUP_2000R;
	case P3H2x4x_DT_TP_PULLUP_1000R:
		return P3H2x4x_TP_PULLUP_1000R;
	case P3H2x4x_DT_TP_PULLUP_250R:
		return P3H2x4x_TP_PULLUP_250R;
	default:
		return P3H2x4x_TP_PULLUP_500R;
	}
}

static u8 p3h2x4x_io_strength_dt_to_reg(int dt_value)
{
	switch (dt_value) {
	case P3H2x4x_DT_IO_STRENGTH_50_OHM:
		return P3H2x4x_IO_STRENGTH_50_OHM;
	case P3H2x4x_DT_IO_STRENGTH_40_OHM:
		return P3H2x4x_IO_STRENGTH_40_OHM;
	case P3H2x4x_DT_IO_STRENGTH_30_OHM:
		return P3H2x4x_IO_STRENGTH_30_OHM;
	default:
		return P3H2x4x_IO_STRENGTH_20_OHM;
	}
}

static int p3h2x4x_configure_pullup(struct device *dev)
{
	struct p3h2x4x *priv = dev_get_drvdata(dev);
	u8 mask = 0, value = 0;

	if (priv->settings.tp0145_pullup != P3H2x4x_TP_PULLUP_NOT_SET) {
		mask |= P3H2x4x_TP0145_PULLUP_CONF_MASK;
		value |= P3H2x4x_TP0145_PULLUP_CONF(p3h2x4x_pullup_dt_to_reg
						    (priv->settings.tp0145_pullup));
	}

	if (priv->settings.tp2367_pullup != P3H2x4x_TP_PULLUP_NOT_SET) {
		mask |= P3H2x4x_TP2367_PULLUP_CONF_MASK;
		value |= P3H2x4x_TP2367_PULLUP_CONF(p3h2x4x_pullup_dt_to_reg
						    (priv->settings.tp2367_pullup));
	}

	return regmap_update_bits(priv->regmap, P3H2x4x_LDO_AND_PULLUP_CONF,
				  mask, value);
}

static int p3h2x4x_configure_ldo(struct device *dev)
{
	struct p3h2x4x *priv = dev_get_drvdata(dev);
	u8 ldo_config_mask = 0, ldo_config_val = 0;
	u8 ldo_disable_mask = 0, ldo_en_val = 0;
	u32 reg_val;
	int ret;
	u8 val;

	/* Enable or Disable LDO's. If there is no DT entry - disable LDO for safety reasons */
	if (priv->settings.cp0_ldo_en)
		ldo_en_val |= P3H2x4x_CP0_EN_LDO;
	if (priv->settings.cp1_ldo_en)
		ldo_en_val |= P3H2x4x_CP1_EN_LDO;
	if (priv->settings.tp0145_ldo_en)
		ldo_en_val |= P3H2x4x_TP0145_EN_LDO;
	if (priv->settings.tp2367_ldo_en)
		ldo_en_val |= P3H2x4x_TP2367_EN_LDO;

	/* Get current LDOs configuration */
	ret = regmap_read(priv->regmap, P3H2x4x_VCCIO_LDO_CONF, &reg_val);
	if (ret)
		return ret;

	/*
	 * LDOs Voltage level (Skip if not defined in the DT)
	 * Set the mask only if there is a change from current value
	 */
	if (priv->settings.cp0_ldo_volt != P3H2x4x_DT_LDO_VOLT_NOT_SET) {
		val = P3H2x4x_CP0_VCCIO_LDO_VOLTAGE(p3h2x4x_ldo_dt_to_reg
						    (priv->settings.cp0_ldo_volt));
		if ((reg_val & P3H2x4x_CP0_VCCIO_LDO_VOLTAGE_MASK) != val) {
			ldo_config_mask |= P3H2x4x_CP0_VCCIO_LDO_VOLTAGE_MASK;
			ldo_config_val |= val;

			ldo_disable_mask |= P3H2x4x_CP0_EN_LDO;
		}
	}
	if (priv->settings.cp1_ldo_volt != P3H2x4x_DT_LDO_VOLT_NOT_SET) {
		val = P3H2x4x_CP1_VCCIO_LDO_VOLTAGE(p3h2x4x_ldo_dt_to_reg
						    (priv->settings.cp1_ldo_volt));
		if ((reg_val & P3H2x4x_CP1_VCCIO_LDO_VOLTAGE_MASK) != val) {
			ldo_config_mask |= P3H2x4x_CP1_VCCIO_LDO_VOLTAGE_MASK;
			ldo_config_val |= val;

			ldo_disable_mask |= P3H2x4x_CP1_EN_LDO;
		}
	}
	if (priv->settings.tp0145_ldo_volt != P3H2x4x_DT_LDO_VOLT_NOT_SET) {
		val = P3H2x4x_TP0145_VCCIO_LDO_VOLTAGE(p3h2x4x_ldo_dt_to_reg
						       (priv->settings.tp0145_ldo_volt));
		if ((reg_val & P3H2x4x_TP0145_VCCIO_LDO_VOLTAGE_MASK) != val) {
			ldo_config_mask |= P3H2x4x_TP0145_VCCIO_LDO_VOLTAGE_MASK;
			ldo_config_val |= val;

			ldo_disable_mask |= P3H2x4x_TP0145_EN_LDO;
		}
	}
	if (priv->settings.tp2367_ldo_volt != P3H2x4x_DT_LDO_VOLT_NOT_SET) {
		val = P3H2x4x_TP2367_VCCIO_LDO_VOLTAGE(p3h2x4x_ldo_dt_to_reg
						       (priv->settings.tp2367_ldo_volt));
		if ((reg_val & P3H2x4x_TP2367_VCCIO_LDO_VOLTAGE_MASK) != val) {
			ldo_config_mask |= P3H2x4x_TP2367_VCCIO_LDO_VOLTAGE_MASK;
			ldo_config_val |= val;

			ldo_disable_mask |= P3H2x4x_TP2367_EN_LDO;
		}
	}

	/*
	 * Update LDO voltage configuration only if value is changed from already existing register
	 * value. It is a good practice to disable the LDO's before making any voltage changes.
	 * Presence of config mask indicates voltage change to be applied.
	 */
	if (ldo_config_mask) {
		/* Disable LDO's before making voltage changes */
		ret = regmap_clear_bits(priv->regmap,
					P3H2x4x_LDO_AND_PULLUP_CONF,
					ldo_disable_mask);
		if (ret)
			return ret;

		/* Update the LDOs configuration */
		ret = regmap_update_bits(priv->regmap, P3H2x4x_VCCIO_LDO_CONF,
					 ldo_config_mask, ldo_config_val);
		if (ret)
			return ret;
	}

	/* Update the LDOs Enable/disable register. This will enable only LDOs enabled in DT */
	return regmap_update_bits(priv->regmap, P3H2x4x_LDO_AND_PULLUP_CONF,
				  P3H2x4x_LDO_ENABLE_DISABLE_MASK, ldo_en_val);
}

static int p3h2x4x_configure_io_strength(struct device *dev)
{
	struct p3h2x4x *priv = dev_get_drvdata(dev);
	u8 mask_all = 0, val_all = 0;
	u32 reg_val;
	int ret;
	u8 val;

	ret = regmap_read(priv->regmap, P3H2x4x_IO_STRENGTH, &reg_val);
	if (ret)
		return ret;

	if (priv->settings.cp0_io_strength != P3H2x4x_IO_STRENGTH_NOT_SET) {
		val = P3H2x4x_CP0_IO_STRENGTH(p3h2x4x_io_strength_dt_to_reg
					      (priv->settings.cp0_io_strength));
		mask_all |= P3H2x4x_CP0_IO_STRENGTH_MASK;
		val_all |= val;
	}
	if (priv->settings.cp1_io_strength != P3H2x4x_IO_STRENGTH_NOT_SET) {
		val = P3H2x4x_CP1_IO_STRENGTH(p3h2x4x_io_strength_dt_to_reg
					      (priv->settings.cp1_io_strength));
		mask_all |= P3H2x4x_CP1_IO_STRENGTH_MASK;
		val_all |= val;
	}
	if (priv->settings.tp0145_io_strength != P3H2x4x_IO_STRENGTH_NOT_SET) {
		val = P3H2x4x_TP0145_IO_STRENGTH(p3h2x4x_io_strength_dt_to_reg
						 (priv->settings.tp0145_io_strength));
		mask_all |= P3H2x4x_TP0145_IO_STRENGTH_MASK;
		val_all |= val;
	}
	if (priv->settings.tp2367_io_strength != P3H2x4x_IO_STRENGTH_NOT_SET) {
		val = P3H2x4x_TP2367_IO_STRENGTH(p3h2x4x_io_strength_dt_to_reg
						 (priv->settings.tp2367_io_strength));
		mask_all |= P3H2x4x_TP2367_IO_STRENGTH_MASK;
		val_all |= val;
	}

	/* Set IO strength if required */
	return regmap_update_bits(priv->regmap, P3H2x4x_IO_STRENGTH, mask_all, val_all);
}

static int p3h2x4x_configure_tp(struct device *dev)
{
	struct p3h2x4x *priv = dev_get_drvdata(dev);
	u8 pullup_mask = 0, pullup_val = 0;
	u8 smbus_mask = 0, smbus_val = 0;
	u8 gpio_mask = 0, gpio_val = 0;
	u8 i3c_mask = 0, i3c_val = 0;
	u8 ibi_mask = 0, ibi_val = 0;
	u8 i2c_mask = 0, i2c_val = 0;
	int ret;
	int i;

	/* TBD: Read type of HUB from register P3H2x4x_DEV_INFO_0 to learn target ports count. */
	for (i = 0; i < P3H2x4x_TP_MAX_COUNT; ++i) {
		if (priv->settings.tp[i].mode != P3H2x4x_TP_MODE_NOT_SET) {
			i3c_mask |= P3H2x4x_TPn_NET_CON(i);
			smbus_mask |= P3H2x4x_TPn_SMBUS_MODE_EN(i);
			gpio_mask |= P3H2x4x_TPn_GPIO_MODE_EN(i);
			i2c_mask |= P3H2x4x_TPn_I2C_MODE_EN(i);

			if (priv->settings.tp[i].mode == P3H2x4x_TP_MODE_I3C)
				i3c_val |= P3H2x4x_TPn_NET_CON(i);
			else if (priv->settings.tp[i].mode == P3H2x4x_TP_MODE_SMBUS)
				smbus_val |= P3H2x4x_TPn_SMBUS_MODE_EN(i);
			else if (priv->settings.tp[i].mode == P3H2x4x_TP_MODE_GPIO)
				gpio_val |= P3H2x4x_TPn_GPIO_MODE_EN(i);
			else if (priv->settings.tp[i].mode == P3H2x4x_TP_MODE_I2C)
				i2c_val |= P3H2x4x_TPn_I2C_MODE_EN(i);
		}
		if (priv->settings.tp[i].pullup_en) {
			pullup_mask |= P3H2x4x_TPn_PULLUP_EN(i);
			pullup_val |= P3H2x4x_TPn_PULLUP_EN(i);
		}

		if (priv->settings.tp[i].ibi_en) {
			ibi_mask |= P3H2x4x_TPn_IBI_EN(i);
			ibi_val |= P3H2x4x_TPn_IBI_EN(i);
		}
	}

	ret = regmap_update_bits(priv->regmap, P3H2x4x_TP_IO_MODE_CONF, (smbus_mask | i2c_mask),
				 (smbus_val | i2c_val));
	if (ret)
		return ret;

	ret = regmap_update_bits(priv->regmap, P3H2x4x_TP_PULLUP_EN, pullup_mask, pullup_val);
	if (ret)
		return ret;

	ret = regmap_update_bits(priv->regmap, P3H2x4x_TP_SMBUS_AGNT_IBI_CONFIG, ibi_mask, ibi_val);
	if (ret)
		return ret;
	priv->tp_ibi_mask = ibi_val;

	ret = regmap_update_bits(priv->regmap, P3H2x4x_TP_SMBUS_AGNT_EN, smbus_mask, smbus_val);
	if (ret)
		return ret;

	ret = regmap_update_bits(priv->regmap, P3H2x4x_TP_GPIO_MODE_EN, gpio_mask, gpio_val);
	if (ret)
		return ret;

	/* Request for HUB Network connection in case any TP is configured in I3C mode */
	if ((i3c_val) || (i2c_val)) {
		ret = regmap_write(priv->regmap, P3H2x4x_CP_MUX_SET,
				   P3H2x4x_CONTROLLER_PORT_MUX_REQ);
		if (ret)
			return ret;
	}

	/* Enable TP here in case TP was configured */
	ret = regmap_update_bits(priv->regmap, P3H2x4x_TP_ENABLE,
				 i3c_mask | smbus_mask | gpio_mask | i2c_mask,
				 i3c_val | smbus_val | gpio_val | i2c_val);
	if (ret)
		return ret;

	return 0;
}

static int p3h2x4x_configure_smbus_local_dev(struct device *dev)
{
	struct p3h2x4x *priv = dev_get_drvdata(dev);
	u8 target_buffer_page, hub_tp;
	int ret = 0;

	for (hub_tp = 0; hub_tp < P3H2x4x_TP_MAX_COUNT; hub_tp++) {
		if (priv->tp_bus[hub_tp].local_dev_count &&
		    priv->settings.tp[hub_tp].mode == P3H2x4x_TP_MODE_SMBUS) {
			target_buffer_page = P3H2x4x_TARGET_AGENT_LOCAL_DEV + 4 * hub_tp;
			ret = regmap_write(priv->regmap, P3H2x4x_PAGE_PTR, target_buffer_page);
			if (ret) {
				dev_err(dev, "Failed to configure local device settings\n");
				break;
			}

			ret = regmap_bulk_write(priv->regmap,
						P3H2x4x_CONTROLLER_AGENT_BUFF,
						priv->tp_bus[hub_tp].local_dev_list,
						priv->tp_bus[hub_tp].local_dev_count);
			if (ret) {
				dev_err(dev, "Failed to add local devices\n");
				break;
			}
		}
	}
	regmap_write(priv->regmap, P3H2x4x_PAGE_PTR, 0x00);
	return ret;
}

static int p3h2x4x_configure_hw(struct device *dev)
{
	int ret;

	ret = p3h2x4x_configure_ldo(dev);
	if (ret)
		return ret;

	ret = p3h2x4x_configure_io_strength(dev);
	if (ret)
		return ret;

	ret = p3h2x4x_configure_pullup(dev);
	if (ret)
		return ret;

	ret = p3h2x4x_configure_smbus_local_dev(dev);
	if (ret)
		return ret;

	return p3h2x4x_configure_tp(dev);
}

static void p3h2x4x_of_get_tp_dt_conf(struct device *dev,
				      const struct device_node *node)
{
	struct p3h2x4x *priv = dev_get_drvdata(dev);
	struct device_node *dev_node;
	u64 tp_port;

	for_each_available_child_of_node(node, dev_node) {
		if (!dev_node->name || of_node_cmp(dev_node->name, "target-port"))
			continue;

		if (of_property_read_reg(dev_node, 0, &tp_port, NULL))
			continue;

		if (tp_port < P3H2x4x_TP_MAX_COUNT) {
			priv->tp_bus[tp_port].dt_available = true;
			priv->tp_bus[tp_port].of_node = dev_node;
			priv->tp_bus[tp_port].tp_mask = BIT(tp_port);
			priv->tp_bus[tp_port].priv = priv;
			priv->tp_bus[tp_port].tp_port = tp_port;
		}
	}
}

/* return true when backend node exist */
static bool p3h2x4x_is_backend_node_exist(int port, struct p3h2x4x *priv, u32 addr)
{
	struct smbus_device *backend = NULL;

	list_for_each_entry(backend,
			    &priv->tp_bus[port].tp_device_entry, list) {
		if (backend->addr == addr)
			return true;
	}
	return false;
}

static int p3h2x4x_read_backend_from_dts(struct device_node *i3c_node_target,
					 struct p3h2x4x *priv)
{
	struct device_node *tp_node;
	const char *compatible;
	u64 tp_port, addr_dts;
	int ret;

	struct smbus_device *backend;

	if (of_property_read_reg(i3c_node_target, 0, &tp_port, NULL))
		return -EINVAL;

	if (tp_port >= P3H2x4x_TP_MAX_COUNT || tp_port < 0)
		return -ERANGE;

	INIT_LIST_HEAD(&priv->tp_bus[tp_port].tp_device_entry);

	if (priv->settings.tp[tp_port].mode == P3H2x4x_TP_MODE_I3C)
		return 0;

	for_each_available_child_of_node(i3c_node_target, tp_node) {
		ret = of_property_read_reg(tp_node, 0, &addr_dts, NULL);
		if (ret)
			return ret;

		if (p3h2x4x_is_backend_node_exist(tp_port, priv, addr_dts))
			continue;

		ret = of_property_read_string(tp_node, "compatible", &compatible);
		if (ret)
			return ret;

		backend = kzalloc(sizeof(*backend), GFP_KERNEL);
		if (!backend)
			return -ENOMEM;

		backend->addr = addr_dts;
		backend->compatible = compatible;
		backend->tp_device_dt_node = tp_node;
		backend->client = NULL;

		list_add(&backend->list,
			 &priv->tp_bus[tp_port].tp_device_entry);
	}

	return 0;
}

static void p3h2x4x_parse_dt_tp(struct device *dev,
				const struct device_node *i3c_node_hub,
				struct p3h2x4x *priv)
{
	struct device_node *i3c_node_target;
	int ret;

	for_each_available_child_of_node(i3c_node_hub, i3c_node_target) {
		if (of_node_cmp(i3c_node_target->name, "target-port") == 0) {
			ret = p3h2x4x_read_backend_from_dts(i3c_node_target, priv);
			if (ret)
				dev_err(dev, "DTS entry invalid - error %d", ret);
		}
	}
}

static int p3h2x4x_get_tp_local_device_dt_setting(struct device *dev,
						  const struct device_node *node, u32 id)
{
	struct p3h2x4x *priv = dev_get_drvdata(dev);
	int ret;

	ret = of_property_read_variable_u8_array(node, "local_dev", priv->tp_bus[id].local_dev_list,
						 sizeof(u8), P3H2x4x_TP_LOCAL_DEV);
	if (ret > 0 && ret <= P3H2x4x_TP_LOCAL_DEV)
		priv->tp_bus[id].local_dev_count = ret;
	else if (ret == -EOVERFLOW)
		dev_warn(dev,
			 "local Devices list is out of range or invalid\n");

	return ret;
}

static void p3h2x4x_get_tp_of_get_setting(struct device *dev,
					  const struct device_node *node,
					  struct tp_setting tp_setting[])
{
	struct device_node *tp_node;
	u64 id;

	for_each_available_child_of_node(node, tp_node) {
		if (!tp_node->name || of_node_cmp(tp_node->name, "target-port"))
			continue;

		if (of_property_read_reg(tp_node, 0, &id, NULL))
			continue;

		if (id >= P3H2x4x_TP_MAX_COUNT) {
			dev_warn(dev, "Invalid target port index found in DT: %lli\n", id);
			continue;
		}
		p3h2x4x_of_get_dt_setting(dev, tp_node, "mode", tp_mode_settings,
					  ARRAY_SIZE(tp_mode_settings),
					  &tp_setting[id].mode);

		tp_setting[id].pullup_en =
					of_property_read_bool(tp_node, "pullup-enable");
		tp_setting[id].ibi_en =
					of_property_read_bool(tp_node, "ibi-enable");
		tp_setting[id].always_enable =
					of_property_read_bool(tp_node, "always-enable");

		p3h2x4x_get_tp_local_device_dt_setting(dev, tp_node, id);
	}
}

static void p3h2x4x_of_get_p3h2x4x_conf(struct device *dev,
					const struct device_node *node)
{
	struct p3h2x4x *priv = dev_get_drvdata(dev);

	priv->settings.cp0_ldo_en =
				of_property_read_bool(node, "cp0-ldo-enable");
	priv->settings.cp1_ldo_en =
				of_property_read_bool(node, "cp1-ldo-enable");
	priv->settings.tp0145_ldo_en =
				of_property_read_bool(node, "tp0145-ldo-enable");
	priv->settings.tp2367_ldo_en =
				of_property_read_bool(node, "tp2367-ldo-enable");

	of_property_read_u32(node, "cp0-ldo-microvolt", &priv->settings.cp0_ldo_volt);
	of_property_read_u32(node, "cp1-ldo-microvolt", &priv->settings.cp1_ldo_volt);
	of_property_read_u32(node, "tp0145-ldo-microvolt", &priv->settings.tp0145_ldo_volt);
	of_property_read_u32(node, "tp2367-ldo-microvolt", &priv->settings.tp2367_ldo_volt);
	of_property_read_u32(node, "tp0145-pullup-ohm", &priv->settings.tp0145_pullup);
	of_property_read_u32(node, "tp2367-pullup-ohm", &priv->settings.tp2367_pullup);
	of_property_read_u32(node, "cp0-io-strength-ohm", &priv->settings.cp0_io_strength);
	of_property_read_u32(node, "cp1-io-strength-ohm", &priv->settings.cp1_io_strength);
	of_property_read_u32(node, "tp0145-io-strength-ohm", &priv->settings.tp0145_io_strength);
	of_property_read_u32(node, "tp2367-io-strength-ohm", &priv->settings.tp2367_io_strength);

	p3h2x4x_get_tp_of_get_setting(dev, node, priv->settings.tp);
}

static void p3h2x4x_of_default_configuration(struct device *dev)
{
	struct p3h2x4x *priv = dev_get_drvdata(dev);
	int tp_count;

	priv->settings.cp0_ldo_volt = P3H2x4x_DT_LDO_VOLT_NOT_SET;
	priv->settings.cp1_ldo_volt = P3H2x4x_DT_LDO_VOLT_NOT_SET;
	priv->settings.tp0145_ldo_volt = P3H2x4x_DT_LDO_VOLT_NOT_SET;
	priv->settings.tp2367_ldo_volt = P3H2x4x_DT_LDO_VOLT_NOT_SET;
	priv->settings.tp0145_pullup = P3H2x4x_TP_PULLUP_NOT_SET;
	priv->settings.tp2367_pullup = P3H2x4x_TP_PULLUP_NOT_SET;
	priv->settings.cp0_io_strength = P3H2x4x_IO_STRENGTH_NOT_SET;
	priv->settings.cp1_io_strength = P3H2x4x_IO_STRENGTH_NOT_SET;
	priv->settings.tp0145_io_strength = P3H2x4x_IO_STRENGTH_NOT_SET;
	priv->settings.tp2367_io_strength = P3H2x4x_IO_STRENGTH_NOT_SET;

	for (tp_count = 0; tp_count < P3H2x4x_TP_MAX_COUNT; ++tp_count)
		priv->settings.tp[tp_count].mode =  P3H2x4x_TP_MODE_NOT_SET;
}

static int p3h2x4x_device_probe_i3c(struct i3c_device *i3cdev)
{
	struct device_node *node __free(device_node) = NULL;
	struct device *dev = &i3cdev->dev;
	struct regmap *regmap;
	struct p3h2x4x *priv;
	int ret, i;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->i3cdev = i3cdev;
	priv->driving_master = i3c_dev_get_master(i3cdev->desc);
	i3cdev_set_drvdata(i3cdev, priv);
	p3h2x4x_of_default_configuration(dev);
	regmap = devm_regmap_init_i3c(i3cdev, &p3h2x4x_regmap_config);
	if (IS_ERR(regmap)) {
		ret = PTR_ERR(regmap);
		return dev_err_probe(dev, ret, "Failed to register I3C HUB regmap\n");
	}
	priv->regmap = regmap;
	priv->is_p3h2x4x_in_i3c = true;

	mutex_init(&priv->etx_mutex);

	for (i = 0; i < P3H2x4x_TP_MAX_COUNT; i++)
		mutex_init(&priv->tp_bus[i].port_mutex);

	/* get hub node from DT */
	node = of_get_child_by_name(dev->parent->of_node, "hub");
	if (!node) {
		dev_dbg(dev, "No DT entry - running with hardware defaults.\n");
	} else {
		p3h2x4x_of_get_p3h2x4x_conf(dev, node);
		p3h2x4x_of_get_tp_dt_conf(dev, node);
		/* Parse DTS to find backend device on the SMBus target mode */
		p3h2x4x_parse_dt_tp(dev, node, priv);
	}

	/* Unlock access to protected registers */
	ret = regmap_write(priv->regmap, P3H2x4x_DEV_REG_PROTECTION_CODE,
			   P3H2x4x_REGISTERS_UNLOCK_CODE);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to unlock HUB's protected registers\n");

	ret = p3h2x4x_configure_hw(dev);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to configure the HUB\n");

	/* Register logic for native SMBus ports */
	for (i = 0; i < P3H2x4x_TP_MAX_COUNT; i++) {
		if (priv->settings.tp[i].mode == P3H2x4x_TP_MODE_SMBUS)
			ret = p3h2x4x_tp_smbus_algo(priv, i);
	}

	/* Register logic for native vertual I3C ports */
	for (i = 0; i < P3H2x4x_TP_MAX_COUNT; i++) {
		if (priv->settings.tp[i].mode == P3H2x4x_TP_MODE_I3C &&
		    !priv->settings.tp[i].always_enable)
			ret = p3h2x4x_tp_i3c_algo(priv, i);
	}

	for (i = 0; i < P3H2x4x_TP_MAX_COUNT; ++i) {
		if (priv->tp_bus[i].dt_available) {
			if (priv->settings.tp[i].always_enable)
				priv->tp_always_enable_mask =
						(priv->tp_always_enable_mask |  BIT(i));

			if (priv->settings.tp[i].mode == P3H2x4x_TP_MODE_I3C)
				priv->tp_i3c_bus_mask = (priv->tp_i3c_bus_mask |  BIT(i));
		}
	}

	ret = regmap_write(priv->regmap, P3H2x4x_TP_NET_CON_CONF, priv->tp_always_enable_mask);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to open Target Port(s)\n");

	ret = i3c_master_do_daa(priv->driving_master);
	if (ret)
		dev_warn(dev, "Failed to run DAA\n");

	/*
	 * holding SDA low when both SMBus Target Agent received data buffers are full.
	 * This feature can be used as a flow-control mechanism for MCTP applications to
	 * avoid MCTP transmitters on Target Ports time out when the SMBus agent buffers
	 * are not serviced in time by upstream controller and only receives write message
	 * from its downstream ports.
	 */
	ret = regmap_update_bits(priv->regmap, P3H2x4x_ONCHIP_TD_AND_SMBUS_AGNT_CONF,
				 P3H2x4x_TARGET_AGENT_DFT_IBI_CONF_MASK,
				 P3H2x4x_TARGET_AGENT_DFT_IBI_CONF);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to P3H2x4x_ONCHIP_TD_AND_SMBUS_AGNT_CONF\n");

	ret = i3c_device_request_ibi(i3cdev, &p3h2x4x_ibireq);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to request IBI\n");

	ret = i3c_device_enable_ibi(i3cdev);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to Enable IBI\n");

	ret = p3h2x4x_tp_add_downstream_device(priv);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to add backend device\n");

	/* Lock access to protected registers */
	ret = regmap_write(priv->regmap, P3H2x4x_DEV_REG_PROTECTION_CODE,
			   P3H2x4x_REGISTERS_LOCK_CODE);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to lock HUB's protected registers\n");

	return 0;
}

static void p3h2x4x_device_remove_i3c(struct i3c_device *i3cdev)
{
	struct p3h2x4x *priv = i3cdev_get_drvdata(i3cdev);
	struct i3c_master_controller *tp_controller;
	struct smbus_device *backend = NULL;
	struct i2c_adapter *tp_adap;
	int i;

	for (i = 0; i < P3H2x4x_TP_MAX_COUNT; i++) {
		tp_adap = &priv->tp_bus[i].smbus_port_adapter;
		tp_controller = &priv->tp_bus[i].i3c_port_controller;

		if (priv->tp_bus[i].is_registered) {
			if (priv->settings.tp[i].mode == P3H2x4x_TP_MODE_SMBUS) {
				list_for_each_entry(backend,
						    &priv->tp_bus[i].tp_device_entry,
						    list) {
					i2c_unregister_device(backend->client);
					kfree(backend);
				}
				i2c_del_adapter(tp_adap);
			} else if (priv->settings.tp[i].mode == P3H2x4x_TP_MODE_I3C) {
				i3c_master_unregister(tp_controller);
			}
		}
	}

	i3c_device_disable_ibi(i3cdev);
	i3c_device_free_ibi(i3cdev);

	mutex_destroy(&priv->etx_mutex);
	for (i = 0; i < P3H2x4x_TP_MAX_COUNT; i++)
		mutex_destroy(&priv->tp_bus[i].port_mutex);
}

static int p3h2x4x_device_probe_i2c(struct i2c_client *client)
{
	struct device_node *node __free(device_node) = NULL;
	struct device *dev = &client->dev;
	struct regmap *regmap;
	struct p3h2x4x *priv;
	int ret, i;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->i2cdev = client;
	i2c_set_clientdata(client, priv);

	p3h2x4x_of_default_configuration(dev);

	regmap = devm_regmap_init_i2c(client, &p3h2x4x_regmap_config);
	if (IS_ERR(regmap)) {
		ret = PTR_ERR(regmap);
		return dev_err_probe(dev, ret, "Failed to register I3C HUB regmap\n");
	}
	priv->regmap = regmap;
	priv->is_p3h2x4x_in_i3c = false;

	mutex_init(&priv->etx_mutex);

	/* Register logic for native SMBus ports */
	for (i = 0; i < P3H2x4x_TP_MAX_COUNT; i++)
		mutex_init(&priv->tp_bus[i].port_mutex);

	/* get hub node from DT */
	node = of_get_child_by_name(dev->parent->of_node, "hub");
	if (!node) {
		dev_dbg(dev, "No DT entry - running with hardware defaults.\n");
	} else {
		p3h2x4x_of_get_p3h2x4x_conf(dev, node);
		p3h2x4x_of_get_tp_dt_conf(dev, node);
		/* Parse DTS to find backend device on the SMBus target mode */
		p3h2x4x_parse_dt_tp(dev, node, priv);
	}

	/* Unlock access to protected registers */
	ret = regmap_write(priv->regmap, P3H2x4x_DEV_REG_PROTECTION_CODE,
			   P3H2x4x_REGISTERS_UNLOCK_CODE);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to unlock HUB's protected registers\n");

	ret = p3h2x4x_configure_hw(dev);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to configure the HUB\n");

	/* Register logic for native SMBus ports */
	for (i = 0; i < P3H2x4x_TP_MAX_COUNT; i++) {
		if (priv->settings.tp[i].mode == P3H2x4x_TP_MODE_SMBUS)
			ret = p3h2x4x_tp_smbus_algo(priv, i);
	}

	ret = p3h2x4x_tp_add_downstream_device(priv);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to add backend device\n");

	for (i = 0; i < P3H2x4x_TP_MAX_COUNT; ++i) {
		if (priv->tp_bus[i].dt_available) {
			if (priv->settings.tp[i].always_enable)
				priv->tp_always_enable_mask =
							(priv->tp_always_enable_mask |  BIT(i));
		}
	}

	ret = regmap_write(priv->regmap, P3H2x4x_TP_NET_CON_CONF, priv->tp_always_enable_mask);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to open Target Port(s)\n");

	/* Lock access to protected registers */
	ret = regmap_write(priv->regmap, P3H2x4x_DEV_REG_PROTECTION_CODE,
			   P3H2x4x_REGISTERS_LOCK_CODE);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to lock HUB's protected registers\n");

	return 0;
}

static void p3h2x4x_device_remove_i2c(struct i2c_client *client)
{
	struct p3h2x4x *priv = i2c_get_clientdata(client);
	struct smbus_device *backend = NULL;
	struct i2c_adapter *tp_adap;
	int i;

	for (i = 0; i < P3H2x4x_TP_MAX_COUNT; i++) {
		tp_adap = &priv->tp_bus[i].smbus_port_adapter;
		if (priv->tp_bus[i].is_registered) {
			list_for_each_entry(backend, &priv->tp_bus[i].tp_device_entry, list) {
				i2c_unregister_device(backend->client);
				kfree(backend);
			}
			i2c_del_adapter(tp_adap);
		}
	}

	mutex_destroy(&priv->etx_mutex);
	for (i = 0; i < P3H2x4x_TP_MAX_COUNT; i++)
		mutex_destroy(&priv->tp_bus[i].port_mutex);
}

/* p3h2x4x ids (i3c) */
static const struct i3c_device_id p3h2x4x_i3c_ids[] = {
	I3C_CLASS(I3C_DCR_HUB, NULL),
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(i3c, p3h2x4x_i3c_ids);

/* p3h2x4x ids (i2c) */
static const struct i2c_device_id p3h2x4x_i2c_id_table[] = {
	{ "nxp-i3c-hub" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(i2c, p3h2x4x_i2c_id_table);

static const struct of_device_id  p3h2x4x_i2c_of_match[] = {
	{
		.compatible = "nxp,p3h2x4x",
	},
	{ /* sentinel */ }
};

MODULE_DEVICE_TABLE(of, p3h2x4x_i2c_of_match);
static struct i3c_driver p3h2x4x_i3c = {
	.driver = {
		.name = "p3h2x4x_i3c_drv",
	},
	.id_table = p3h2x4x_i3c_ids,
	.probe = p3h2x4x_device_probe_i3c,
	.remove = p3h2x4x_device_remove_i3c,
};

static struct i2c_driver p3h2x4x_i2c = {
	.driver = {
		.name = "p3h2x4x_i2c_drv",
		.of_match_table = p3h2x4x_i2c_of_match,
	},
	.probe =  p3h2x4x_device_probe_i2c,
	.remove = p3h2x4x_device_remove_i2c,
	.id_table = p3h2x4x_i2c_id_table,
};

module_i3c_i2c_driver(p3h2x4x_i3c, &p3h2x4x_i2c);

MODULE_AUTHOR("Aman Kumar Pandey <aman.kumarpandey@nxp.com>");
MODULE_AUTHOR("vikash Bansal <vikash.bansal@nxp.com>");
MODULE_DESCRIPTION("P3H2x4x I3C HUB driver");
MODULE_LICENSE("GPL");
