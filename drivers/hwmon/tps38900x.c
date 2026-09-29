// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Driver for Texas Instruments TPS389004/TPS389006/TPS389008 i2c voltage
 * supervisor and monitor.
 *
 * Author: Luna Hart <lhart.opensource@witekio.com>
 *
 * Datasheet: https://www.ti.com/lit/ds/symlink/tps389006.pdf
 * Datasheet: https://www.ti.com/lit/ds/symlink/tps389006-q1.pdf
 */

#include <linux/bits.h>
#include <linux/hwmon.h>
#include <linux/i2c.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/property.h>
#include <linux/regmap.h>

#define TPS38900X_MAX_CHANNELS 8
#define TPS38900X_BANK_LEN 0x100

// Registers, stored over 2 banks of 256 bytes.
#define TPS38900X_VRANGE_MULT 0x1f
#define TPS38900X_MON_LVL(ch) (0x40 + (ch))
#define TPS38900X_BANK_SEL 0xf0
#define TPS38900X_MON_CH_EN 0x11e

#define TPS38900X_LSB_MV 5
#define TPS38900X_OFFSET_MV 200

struct tps38900x_data {
	struct regmap *regmap;
	const char *label[TPS38900X_MAX_CHANNELS];
	unsigned int num_channels;
	// bitmasks of channel states
	u8 channel_present;
	u8 channel_scale_4x;
};

static umode_t tps38900x_is_visible(const void *data,
				    enum hwmon_sensor_types type, u32 attr,
				    int channel)
{
	const struct tps38900x_data *priv = data;

	if (!(priv->channel_present & BIT(channel)))
		return 0;

	switch (attr) {
	case hwmon_in_input:
		return 0444;
	case hwmon_in_enable:
		return 0644;
	case hwmon_in_label:
		return priv->label[channel] ? 0444 : 0;
	}

	return 0;
}

static int tps38900x_read(struct device *dev, enum hwmon_sensor_types type,
			  u32 attr, int channel, long *val)
{
	struct tps38900x_data *priv = dev_get_drvdata(dev);
	unsigned int regval;
	int ret;
	int val_scale;

	switch (attr) {
	case hwmon_in_input:
		ret = regmap_read(priv->regmap, TPS38900X_MON_CH_EN, &regval);
		if (ret)
			return ret;

		if (!(regval & BIT(channel)))
			return -ENODATA;

		ret = regmap_read(priv->regmap, TPS38900X_MON_LVL(channel),
				  &regval);
		if (ret)
			return ret;

		val_scale = priv->channel_scale_4x & BIT(channel) ? 4 : 1;
		*val = (regval * TPS38900X_LSB_MV + TPS38900X_OFFSET_MV) *
		       val_scale;
		return 0;
	case hwmon_in_enable:
		ret = regmap_read(priv->regmap, TPS38900X_MON_CH_EN, &regval);
		if (ret)
			return ret;

		*val = !!(regval & BIT(channel));
		return 0;
	}

	return -EOPNOTSUPP;
}

static int tps38900x_write(struct device *dev, enum hwmon_sensor_types type,
			   u32 attr, int channel, long enable)
{
	struct tps38900x_data *priv = dev_get_drvdata(dev);

	switch (attr) {
	case hwmon_in_enable:
		if (enable != 0 && enable != 1)
			return -EINVAL;

		return regmap_update_bits(priv->regmap, TPS38900X_MON_CH_EN,
					  BIT(channel),
					  enable ? BIT(channel) : 0);
	}

	return -EOPNOTSUPP;
}

static int tps38900x_read_string(struct device *dev,
				 enum hwmon_sensor_types type, u32 attr,
				 int channel, const char **str)
{
	struct tps38900x_data *priv = dev_get_drvdata(dev);

	switch (attr) {
	case hwmon_in_label:
		*str = priv->label[channel];
		return 0;
	}

	return -EOPNOTSUPP;
}

static const struct hwmon_ops tps38900x_hwmon_ops = {
	.is_visible = tps38900x_is_visible,
	.read = tps38900x_read,
	.write = tps38900x_write,
	.read_string = tps38900x_read_string,
};

#define TPS38900X_IN_CONFIG (HWMON_I_INPUT | HWMON_I_ENABLE | HWMON_I_LABEL)

static const struct hwmon_channel_info *const tps38900x_info[] = {
	HWMON_CHANNEL_INFO(in, TPS38900X_IN_CONFIG, TPS38900X_IN_CONFIG,
			   TPS38900X_IN_CONFIG, TPS38900X_IN_CONFIG,
			   TPS38900X_IN_CONFIG, TPS38900X_IN_CONFIG,
			   TPS38900X_IN_CONFIG, TPS38900X_IN_CONFIG),
	NULL
};

static const struct hwmon_chip_info tps38900x_chip_info = {
	.ops = &tps38900x_hwmon_ops,
	.info = tps38900x_info,
};

static const struct regmap_range_cfg tps38900x_bank_range = {
	.name = "tps38900x-mem-bank",
	.range_min = 0x00,
	.range_max = (2 * TPS38900X_BANK_LEN) - 1,
	.selector_reg = TPS38900X_BANK_SEL,
	.selector_mask = BIT(0),
	.window_start = 0,
	.window_len = TPS38900X_BANK_LEN,
};

static bool tps38900x_volatile_reg(struct device *dev, unsigned int reg)
{
	// ADC results region is volatile
	if (reg >= TPS38900X_MON_LVL(0) &&
	    reg < TPS38900X_MON_LVL(TPS38900X_MAX_CHANNELS))
		return true;

	return false;
}

static const struct regmap_config tps38900x_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = 2 * TPS38900X_BANK_LEN - 1,
	.ranges = &tps38900x_bank_range,
	.num_ranges = 1,
	.volatile_reg = tps38900x_volatile_reg,
	.cache_type = REGCACHE_MAPLE,
};

static int tps38900x_parse_dt_channels(struct device *dev,
				       struct tps38900x_data *priv)
{
	u32 reg;
	int ret;

	device_for_each_child_node_scoped(dev, child) {
		ret = fwnode_property_read_u32(child, "reg", &reg);
		if (ret)
			return dev_err_probe(dev, ret,
					     "missing reg property\n");

		if (reg >= priv->num_channels)
			return dev_err_probe(dev, -EINVAL,
					     "reg out of range: %u (max %u)\n",
					     reg, priv->num_channels);

		priv->channel_present |= BIT(reg);
		if (fwnode_property_read_bool(child, "ti,4x-scaling"))
			priv->channel_scale_4x |= BIT(reg);

		fwnode_property_read_string(child, "label", &priv->label[reg]);
	}

	// When no channels are explicitly described in the DT, expose all of them.
	if (!priv->channel_present)
		priv->channel_present = GENMASK(priv->num_channels - 1, 0);

	return 0;
}

static int tps38900x_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct tps38900x_data *priv;
	struct device *hwmon_dev;
	int ret;

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_SMBUS_BYTE_DATA))
		return -ENODEV;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->num_channels = (uintptr_t)i2c_get_match_data(client);

	priv->regmap = devm_regmap_init_i2c(client, &tps38900x_regmap_config);
	if (IS_ERR(priv->regmap))
		return dev_err_probe(dev, PTR_ERR(priv->regmap),
				     "failed to allocate register map\n");

	ret = tps38900x_parse_dt_channels(dev, priv);
	if (ret)
		return ret;

	ret = regmap_write(priv->regmap, TPS38900X_VRANGE_MULT,
			   priv->channel_scale_4x);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to write voltage range\n");

	hwmon_dev = devm_hwmon_device_register_with_info(dev, client->name, priv,
							 &tps38900x_chip_info, NULL);

	return PTR_ERR_OR_ZERO(hwmon_dev);
}

static const struct i2c_device_id tps38900x_id[] = {
	{ .name = "tps389004", .driver_data = 4 },
	{ .name = "tps389006", .driver_data = 6 },
	{ .name = "tps389008", .driver_data = 8 },
	{}
};
MODULE_DEVICE_TABLE(i2c, tps38900x_id);

static const struct of_device_id tps38900x_of_match[] = {
	{ .compatible = "ti,tps389004", .data = (void *)4 },
	{ .compatible = "ti,tps389006", .data = (void *)6 },
	{ .compatible = "ti,tps389008", .data = (void *)8 },
	{}
};
MODULE_DEVICE_TABLE(of, tps38900x_of_match);

static struct i2c_driver tps38900x_driver = {
	.driver = {
		.name = "tps38900x",
		.of_match_table = tps38900x_of_match,
	},
	.probe = tps38900x_probe,
	.id_table = tps38900x_id,
};
module_i2c_driver(tps38900x_driver);

MODULE_AUTHOR("Luna Hart <lhart.opensource@witekio.com>");
MODULE_DESCRIPTION("TI TPS389004/TPS389006/TPS389008 voltage monitor driver");
MODULE_LICENSE("GPL");
