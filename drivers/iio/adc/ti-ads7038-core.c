// SPDX-License-Identifier: GPL-2.0-or-later
/* This driver supports TI 12Bit ADC devices
 *
 *	 - ADS7038 with SPI interface
 *
 * Copyright (C) 2023 SYS TEC electronic AG
 * Author: Andre Werner <andre.werner@systec-electronic.com>
 */
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/fwnode.h>
#include <linux/iio/iio.h>
#include <linux/iio/types.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/property.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/sysfs.h>

#include "ti-ads7038.h"

#define ADS7038_AVERAGE_OUTPUT_BITS	16

#define ADS7038_V_CHAN(_chan, _addr)				\
	{							\
	.type = IIO_VOLTAGE,					\
	.indexed = 1,						\
	.address = _addr,					\
	.channel = _chan,					\
	.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),		\
	.info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SCALE),	\
	.scan_index = _addr,					\
	.scan_type = {						\
		.sign = 's',					\
		.realbits = 12,					\
		.storagebits = 16,				\
		.shift = 4,					\
		.endianness = IIO_CPU,				\
	},							\
	.datasheet_name = "AIN"#_chan,				\
	}

static const struct iio_chan_spec ads7038_channels[] = {
	ADS7038_V_CHAN(0, AIN0), ADS7038_V_CHAN(1, AIN1),
	ADS7038_V_CHAN(2, AIN2), ADS7038_V_CHAN(3, AIN3),
	ADS7038_V_CHAN(4, AIN4), ADS7038_V_CHAN(5, AIN5),
	ADS7038_V_CHAN(6, AIN6), ADS7038_V_CHAN(7, AIN7),
};

static ssize_t ads7038_crc_show(struct device *dev, struct device_attribute *attr,
				char *buf)
{
	const struct iio_dev *indio_dev = dev_get_drvdata(dev);
	struct ads7038_data *data = iio_priv(indio_dev);

	return sysfs_emit(buf, "%s\n", data->crc_enabled ? "enabled (not implement)" : "disabled");
}

static ssize_t ads7038_status_show(struct device *dev, struct device_attribute *attr,
				   char *buf)
{
	const struct iio_dev *indio_dev = dev_get_drvdata(dev);
	struct ads7038_data *data = iio_priv(indio_dev);

	return sysfs_emit(buf, "0x%01X\n", data->status_appended ? data->current_status : 0xFF);
}

static ssize_t ads7038_chanid_show(struct device *dev, struct device_attribute *attr,
				   char *buf)
{
	const struct iio_dev *indio_dev = dev_get_drvdata(dev);
	struct ads7038_data *data = iio_priv(indio_dev);

	return sysfs_emit(buf, "%d\n", data->latest_chanid);
}

static ssize_t ads7038_cycletime_show(struct device *dev, struct device_attribute *attr,
				      char *buf)
{
	const struct iio_dev *indio_dev = dev_get_drvdata(dev);
	struct ads7038_data *data = iio_priv(indio_dev);

	return sysfs_emit(buf, "%d us\n", data->measure_cycle_time_us);
}

static ssize_t ads7038_calibrate_store(struct device *dev, struct device_attribute *attr,
				       const char *buf, size_t count)
{
	const struct iio_dev *indio_dev = dev_get_drvdata(dev);
	struct ads7038_data *data = iio_priv(indio_dev);
	int ret;
	unsigned int regval, loop_cnt = 10;

	mutex_lock(&data->lock);
	ret = regmap_read(data->regmap, ADS7038_GENERAL_CFG_REG, &regval);
	if (ret < 0)
		goto error;

	regval |= ADS7038_GENERAL_CFG_CAL;

	ret = regmap_write(data->regmap, ADS7038_GENERAL_CFG_REG, regval);
	if (ret < 0)
		goto error;

	do {
		ret = regmap_read(data->regmap, ADS7038_GENERAL_CFG_REG, &regval);
		if (ret < 0)
			goto error;
		--loop_cnt;
	} while ((regval & ADS7038_GENERAL_CFG_CAL) && (loop_cnt < 0));

	if (loop_cnt)
		ret = count;
	else
		ret = -EIO;
error:
	mutex_unlock(&data->lock);
	return ret;
}

static DEVICE_ATTR_RO(ads7038_crc);
static DEVICE_ATTR_RO(ads7038_status);
static DEVICE_ATTR_RO(ads7038_chanid);
static DEVICE_ATTR_RO(ads7038_cycletime);
static DEVICE_ATTR_WO(ads7038_calibrate);

static struct attribute *ads7038_dev_attrs[] = {
	&dev_attr_ads7038_crc.attr,
	&dev_attr_ads7038_status.attr,
	&dev_attr_ads7038_chanid.attr,
	&dev_attr_ads7038_cycletime.attr,
	&dev_attr_ads7038_calibrate.attr,
	NULL,
};
ATTRIBUTE_GROUPS(ads7038_dev);

static int ads7038_regulator_config(struct iio_dev *const indio_dev,
				    struct ads7038_data *const data)
{
	struct regulator *reg;
	int ret;

	mutex_lock(&data->lock);
	ret = regulator_register_supply_alias(&indio_dev->dev, "avdd", data->dev, "avdd");
	if (ret < 0)
		goto error;

	reg = devm_regulator_get(&indio_dev->dev, "avdd");
	if (IS_ERR(reg)) {
		dev_dbg(&indio_dev->dev, "Failed to get regulator \"AVDD\"\n");
		ret = PTR_ERR(reg);
		goto error;
	}

	ret = regulator_enable(reg);
	if (ret < 0) {
		dev_dbg(&indio_dev->dev, "Failed to enable regulator \"AVDD\"\n");
		goto error;
	}

	data->avdd_reg = reg;

	ret = regulator_register_supply_alias(&indio_dev->dev, "dvdd", data->dev, "dvdd");
	if (ret < 0)
		goto error;

	reg = devm_regulator_get(&indio_dev->dev, "dvdd");
	if (IS_ERR(reg)) {
		dev_dbg(&indio_dev->dev, "Failed to get regulator \"DVDD\"\n");
		ret = PTR_ERR(reg);
		goto error;
	}

	ret = regulator_enable(reg);
	if (ret < 0) {
		dev_dbg(&indio_dev->dev, "Failed to enable regulator \"DVDD\"\n");
		goto error;
	}

	data->dvdd_reg = reg;

error:
	mutex_unlock(&data->lock);
	return 0;
}

static int ads7038_enable_status(struct ads7038_data *const data, const bool enable)
{
	int ret;
	struct regmap *map = data->regmap;
	unsigned int regval, stat_enable;

	if (enable)
		stat_enable = ADS7038_DATA_CFG_APPEND_STATUS_STATUS;
	else
		stat_enable =  ADS7038_DATA_CFG_APPEND_STATUS_NO;

	mutex_lock(&data->lock);
	ret = regmap_read(map, ADS7038_DATA_CFG_REG, &regval);
	if (ret < 0)
		goto error;

	regval = (ret & ~ADS7038_DATA_CFG_APPEND_STATUS) | stat_enable;

	ret = regmap_write(map, ADS7038_DATA_CFG_REG, regval);
	if (ret < 0)
		goto error;

	data->status_appended = enable;

error:
	mutex_unlock(&data->lock);
	return ret;
}

static int ads7038_config_average(struct ads7038_data *const data, const u32 ov_ratio)
{
	int ret;
	struct regmap *map = data->regmap;
	unsigned int regval;

	if (ov_ratio > ADS7038_OSR_CFG_OSR_128)
		return -EINVAL;

	mutex_lock(&data->lock);
	ret = regmap_read(map, ADS7038_OSR_CFG_REG, &regval);
	if (ret < 0)
		goto error;

	regval = (ret & ~ADS7038_OSR_CFG_OSR) | ov_ratio;

	ret = regmap_write(map, ADS7038_OSR_CFG_REG, regval);
	if (ret < 0)
		goto error;

	data->measure_cycle_time_us = (1 << ov_ratio) * ADS7038_CYCLE_TIME_US;
	if (ov_ratio == ADS7038_OSR_CFG_OSR_NO)
		data->avaraging_enabled = false;
	else
		data->avaraging_enabled = true;

error:
	mutex_unlock(&data->lock);
	return ret;
}

static int ads7038_set_mode_manual(struct device const *dev, struct regmap *const map)
{
	int ret;
	const unsigned int regs[] = { ADS7038_OPMODE_CFG_REG,
		ADS7038_SEQUENCE_CFG_REG
	};
	unsigned int reg_values[2] = { 0 };
	unsigned int idx;

	/* Registers need to be read first to adapt configuration bits. */
	for (idx = 0; idx < ARRAY_SIZE(regs); ++idx) {
		ret = regmap_read(map, regs[idx], &reg_values[idx]);
		if (ret < 0) {
			dev_dbg(dev,
				"Cannot read value from register %02X.\n",
				regs[idx]);
			break;
		}
	}

	if (ret < 0)
		goto out;

	reg_values[0] &= ~ADS7038_OPMODE_CFG_CONV_MODE;
	reg_values[0] |= ADS7038_OPMODE_CFG_CONV_MODE_MANUAL;
	reg_values[0] &= ~ADS7038_SEQUENCE_CFG_SEQ_MODE;
	reg_values[0] |= ADS7038_SEQUENCE_CFG_SEQ_MODE_MANUAL;

	for (idx = 0; idx < ARRAY_SIZE(regs); ++idx) {
		ret = regmap_write(map, regs[idx], reg_values[idx]);
		if (ret < 0) {
			dev_dbg(dev,
				"Cannot write value to register %02X.\n",
				regs[idx]);
			break;
		}
	}
out:
	return ret;
}

static int ads7038_read_raw(struct iio_dev *indio_dev,
			    struct iio_chan_spec const *chan,
		     int *val, int *val2,
		     long mask)
{
	unsigned int ret;
	struct ads7038_ch_meas_result tmp_val;
	struct ads7038_data *data = iio_priv(indio_dev);

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		ret = data->read_channel_raw(indio_dev, chan->channel, &tmp_val);

		if (ret < 0) {
			dev_err(&indio_dev->dev, "Read channel returned with error %d", ret);
			return ret;
		}

		*val = tmp_val.raw;

		mutex_lock(&data->lock);
		if (data->status_appended) {
			if (tmp_val.status_valid)
				data->current_status = tmp_val.status;
			else
				dev_warn(&indio_dev->dev, "No valid status reported by device.\n");
		}
		data->latest_chanid = chan->channel;
		mutex_unlock(&data->lock);

		return IIO_VAL_INT;
	case IIO_CHAN_INFO_SCALE:
		mutex_lock(&data->lock);
		ret = regulator_get_voltage(data->avdd_reg);
		mutex_unlock(&data->lock);
		if (ret < 0)
			return ret;

		*val = ret / 1000;	/* uV -> mV */
		if (data->avaraging_enabled)
			*val2 = 1 << ADS7038_AVERAGE_OUTPUT_BITS;
		else
			*val2 = 1 << chan->scan_type.realbits;

		return IIO_VAL_FRACTIONAL;
	default:
		return -EINVAL;
	}
}

static const struct iio_info ads7038_iio_info = {
	.read_raw = ads7038_read_raw,
};

int ads7038_common_probe(struct device *parent, read_channel_raw_cb read_ch_raw_cb,
			 struct regmap *const regmap,
			 const char *name, int irq)
{
	struct ads7038_data *data;
	struct iio_dev *indio_dev;
	u32 prop_value;
	int ret;

	indio_dev = devm_iio_device_alloc(parent, sizeof(struct ads7038_data));
	if (!indio_dev)
		return -ENOMEM;

	indio_dev->name = name;
	indio_dev->channels = ads7038_channels;
	indio_dev->num_channels = ARRAY_SIZE(ads7038_channels);
	indio_dev->info = &ads7038_iio_info;
	indio_dev->modes = INDIO_DIRECT_MODE;

	data = iio_priv(indio_dev);
	mutex_init(&data->lock);
	data->dev = parent;
	data->read_channel_raw = read_ch_raw_cb;
	data->regmap = regmap;

	ret = ads7038_regulator_config(indio_dev, data);
	if (ret < 0)
		return ret;

	ret = ads7038_enable_status(data, device_property_present(parent, "status-enabled"));
	if (ret < 0) {
		dev_dbg(&indio_dev->dev,
			"Error while configuring appending of chip status (ret = %d)\n",
			ret);
		return ret;
	}

	ret = device_property_read_u32(parent, "average-samples", &prop_value);
	if (ret == 0) {
		ret = ads7038_config_average(data, prop_value);
		if (ret < 0) {
			dev_dbg(&indio_dev->dev,
				"Error while configuring appending of chip status (ret = %d)\n",
				ret);
			return ret;
		}
	}

	data->crc_enabled = device_property_present(parent, "crc-enabled");

	/* Configure read manual mode for single read of channel value */
	ret = ads7038_set_mode_manual(&indio_dev->dev, data->regmap);
	if (ret < 0)
		return ret;

	data->func_mode = MAN;

	/* Link general device driver with IIO device driver data */
	dev_set_drvdata(parent, indio_dev);

	devm_iio_device_register(data->dev, indio_dev);

	ret = devm_device_add_groups(&indio_dev->dev, ads7038_dev_groups);
	if (ret < 0)
		return ret;

	return 0;
}
EXPORT_SYMBOL_NS_GPL(ads7038_common_probe, IIO_ADS7038);

MODULE_AUTHOR("Andre Werner <andre.werner@systec-electronic.com>");
MODULE_DESCRIPTION("ADS7038 and ADS7138 core driver");
MODULE_LICENSE("GPL");
