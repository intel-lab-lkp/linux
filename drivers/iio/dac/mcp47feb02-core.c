// SPDX-License-Identifier: GPL-2.0+
/*
 * IIO driver for MCP47FEB02 Multi-Channel DAC with I2C interface
 *
 * Copyright (C) 2026 Microchip Technology Inc. and its subsidiaries
 *
 * Author: Ariana Lazar <ariana.lazar@microchip.com>
 *
 * Datasheet links for devices with I2C interface:
 * [MCP47FEBxx] https://ww1.microchip.com/downloads/aemDocuments/documents/OTH/ProductDocuments/DataSheets/20005375A.pdf
 * [MCP47FVBxx] https://ww1.microchip.com/downloads/aemDocuments/documents/OTH/ProductDocuments/DataSheets/20005405A.pdf
 * [MCP47FxBx4/8] https://ww1.microchip.com/downloads/aemDocuments/documents/MSLD/ProductDocuments/DataSheets/MCP47FXBX48-Data-Sheet-DS200006368A.pdf
 */
#include <linux/array_size.h>
#include <linux/bitops.h>
#include <linux/bits.h>
#include <linux/cleanup.h>
#include <linux/device.h>
#include <linux/export.h>
#include <linux/kstrtox.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/property.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/types.h>
#include <linux/units.h>

#include <linux/iio/iio.h>
#include <linux/iio/sysfs.h>

#include "mcp47feb02.h"

static const char * const mcp47feb02_powerdown_modes[] = {
	"1kohm_to_gnd",
	"100kohm_to_gnd",
	"open_circuit",
};

/* For devices with nonvolatile memory */
static const struct regmap_range mcp47feb02_readable_ranges[] = {
	regmap_reg_range(MCP47FEB02_DAC0_REG_ADDR, MCP47FEB02_WIPERLOCK_STATUS_REG_ADDR),
	regmap_reg_range(MCP47FEB02_NV_DAC0_REG_ADDR, MCP47FEB02_NV_GAIN_CTRL_I2C_SLAVE_REG_ADDR),
};

static const struct regmap_range mcp47feb02_writable_ranges[] = {
	regmap_reg_range(MCP47FEB02_DAC0_REG_ADDR, MCP47FEB02_WIPERLOCK_STATUS_REG_ADDR),
	regmap_reg_range(MCP47FEB02_NV_DAC0_REG_ADDR, MCP47FEB02_NV_GAIN_CTRL_I2C_SLAVE_REG_ADDR),
};

static const struct regmap_range mcp47feb02_volatile_ranges[] = {
	regmap_reg_range(MCP47FEB02_DAC0_REG_ADDR, MCP47FEB02_WIPERLOCK_STATUS_REG_ADDR),
	regmap_reg_range(MCP47FEB02_NV_DAC0_REG_ADDR, MCP47FEB02_NV_GAIN_CTRL_I2C_SLAVE_REG_ADDR),
};

static const struct regmap_access_table mcp47feb02_readable_table = {
	.yes_ranges = mcp47feb02_readable_ranges,
	.n_yes_ranges = ARRAY_SIZE(mcp47feb02_readable_ranges),
};

static const struct regmap_access_table mcp47feb02_writable_table = {
	.yes_ranges = mcp47feb02_writable_ranges,
	.n_yes_ranges = ARRAY_SIZE(mcp47feb02_writable_ranges),
};

static const struct regmap_access_table mcp47feb02_volatile_table = {
	.yes_ranges = mcp47feb02_volatile_ranges,
	.n_yes_ranges = ARRAY_SIZE(mcp47feb02_volatile_ranges),
};

const struct regmap_config mcp47feb02_regmap_config = {
	.name = "mcp47feb02_regmap",
	.reg_bits = 8,
	.val_bits = 16,
	.rd_table = &mcp47feb02_readable_table,
	.wr_table = &mcp47feb02_writable_table,
	.volatile_table = &mcp47feb02_volatile_table,
	.max_register = MCP47FEB02_NV_GAIN_CTRL_I2C_SLAVE_REG_ADDR,
	.read_flag_mask = READFLAG_MASK,
	.cache_type = REGCACHE_MAPLE,
	.val_format_endian = REGMAP_ENDIAN_BIG,
};
EXPORT_SYMBOL_NS_GPL(mcp47feb02_regmap_config, "IIO_MCP47FEB02");

/* For devices without nonvolatile memory */
static const struct regmap_range mcp47fvb02_readable_ranges[] = {
	regmap_reg_range(MCP47FEB02_DAC0_REG_ADDR, MCP47FEB02_WIPERLOCK_STATUS_REG_ADDR),
};

static const struct regmap_range mcp47fvb02_writable_ranges[] = {
	regmap_reg_range(MCP47FEB02_DAC0_REG_ADDR, MCP47FEB02_WIPERLOCK_STATUS_REG_ADDR),
};

static const struct regmap_range mcp47fvb02_volatile_ranges[] = {
	regmap_reg_range(MCP47FEB02_DAC0_REG_ADDR, MCP47FEB02_WIPERLOCK_STATUS_REG_ADDR),
};

static const struct regmap_access_table mcp47fvb02_readable_table = {
	.yes_ranges = mcp47fvb02_readable_ranges,
	.n_yes_ranges = ARRAY_SIZE(mcp47fvb02_readable_ranges),
};

static const struct regmap_access_table mcp47fvb02_writable_table = {
	.yes_ranges = mcp47fvb02_writable_ranges,
	.n_yes_ranges = ARRAY_SIZE(mcp47fvb02_writable_ranges),
};

static const struct regmap_access_table mcp47fvb02_volatile_table = {
	.yes_ranges = mcp47fvb02_volatile_ranges,
	.n_yes_ranges = ARRAY_SIZE(mcp47fvb02_volatile_ranges),
};

const struct regmap_config mcp47fvb02_regmap_config = {
	.name = "mcp47fvb02_regmap",
	.reg_bits = 8,
	.val_bits = 16,
	.rd_table = &mcp47fvb02_readable_table,
	.wr_table = &mcp47fvb02_writable_table,
	.volatile_table = &mcp47fvb02_volatile_table,
	.max_register = MCP47FEB02_WIPERLOCK_STATUS_REG_ADDR,
	.read_flag_mask = READFLAG_MASK,
	.cache_type = REGCACHE_MAPLE,
	.val_format_endian = REGMAP_ENDIAN_BIG,
};
EXPORT_SYMBOL_NS_GPL(mcp47fvb02_regmap_config, "IIO_MCP47FEB02");

static int mcp47feb02_write_to_eeprom(struct mcp47feb02_data *data, unsigned int reg,
				      unsigned int val)
{
	unsigned int eewa_val;
	int ret;

	ret = regmap_read_poll_timeout(data->regmap, MCP47FEB02_GAIN_CTRL_STATUS_REG_ADDR,
				       eewa_val,
				       !(eewa_val & MCP47FEB02_GAIN_BIT_STATUS_EEWA_MASK),
				       1 * USEC_PER_MSEC, 20 * USEC_PER_MSEC);
	if (ret)
		return ret;

	return regmap_write(data->regmap, reg, val);
}

static ssize_t store_eeprom_store(struct device *dev, struct device_attribute *attr,
				  const char *buf, size_t len)
{
	struct mcp47feb02_data *data = iio_priv(dev_to_iio_dev(dev));
	unsigned int i, val, val1, eewa_val;
	bool state;
	int ret;

	ret = kstrtobool(buf, &state);
	if (ret)
		return ret;

	if (!state)
		return len;

	/*
	 * Wait until the currently occurring EEPROM Write Cycle is completed.
	 * Only serial commands to the volatile memory are allowed.
	 */
	guard(mutex)(&data->lock);

	/*
	 * Verify DAC Wiper and DAC Configuration are unlocked. If both are disabled,
	 * writing to EEPROM is available.
	 */
	ret = regmap_read(data->regmap, MCP47FEB02_WIPERLOCK_STATUS_REG_ADDR, &val);
	if (ret)
		return ret;

	if (val) {
		dev_err(dev, "DAC Wiper and DAC Configuration are not unlocked\n");
		return -EINVAL;
	}

	for_each_set_bit(i, &data->active_channels_mask, data->phys_channels) {
		ret = mcp47feb02_write_to_eeprom(data, NV_REG_ADDR(i),
						 data->chdata[i].dac_data);
		if (ret)
			return ret;
	}

	ret = regmap_read(data->regmap, MCP47FEB02_VREF_REG_ADDR, &val);
	if (ret)
		return ret;

	ret = mcp47feb02_write_to_eeprom(data, MCP47FEB02_NV_VREF_REG_ADDR, val);
	if (ret)
		return ret;

	ret = regmap_read(data->regmap, MCP47FEB02_POWER_DOWN_REG_ADDR, &val);
	if (ret)
		return ret;

	ret = mcp47feb02_write_to_eeprom(data, MCP47FEB02_NV_POWER_DOWN_REG_ADDR, val);
	if (ret)
		return ret;

	ret = regmap_read_poll_timeout(data->regmap, MCP47FEB02_GAIN_CTRL_STATUS_REG_ADDR, eewa_val,
				       !(eewa_val & MCP47FEB02_GAIN_BIT_STATUS_EEWA_MASK),
				       1 * USEC_PER_MSEC, 20 * USEC_PER_MSEC);
	if (ret)
		return ret;

	ret = regmap_read(data->regmap, MCP47FEB02_NV_GAIN_CTRL_I2C_SLAVE_REG_ADDR, &val);
	if (ret)
		return ret;

	ret = regmap_read(data->regmap, MCP47FEB02_GAIN_CTRL_STATUS_REG_ADDR, &val1);
	if (ret)
		return ret;

	ret = mcp47feb02_write_to_eeprom(data, MCP47FEB02_NV_GAIN_CTRL_I2C_SLAVE_REG_ADDR,
					 (val1 & MCP47FEB02_GAIN_BITS_MASK) |
					 (val & MCP47FEB02_NV_I2C_SLAVE_ADDR_MASK));
	if (ret)
		return ret;

	return len;
}
static IIO_DEVICE_ATTR_WO(store_eeprom, 0);

static struct attribute *mcp47feb02_attributes[] = {
	&iio_dev_attr_store_eeprom.dev_attr.attr,
	NULL
};

static const struct attribute_group mcp47feb02_attribute_group = {
	.attrs = mcp47feb02_attributes,
};

static int mcp47feb02_suspend(struct device *dev)
{
	struct mcp47feb02_data *data = iio_priv(dev_get_drvdata(dev));
	int ret;
	u8 ch;

	guard(mutex)(&data->lock);

	for_each_set_bit(ch, &data->active_channels_mask, data->phys_channels) {
		u8 pd_mode;

		pd_mode = data->chdata[ch].powerdown_mode + 1;
		ret = regmap_update_bits(data->regmap, MCP47FEB02_POWER_DOWN_REG_ADDR,
					 DAC_CTRL_MASK(ch), DAC_CTRL_VAL(ch, pd_mode));
		if (ret)
			return ret;

		data->chdata[ch].powerdown = true;
	}

	return 0;
}

static int mcp47feb02_resume(struct device *dev)
{
	struct mcp47feb02_data *data = iio_priv(dev_get_drvdata(dev));
	u8 ch;

	guard(mutex)(&data->lock);

	for_each_set_bit(ch, &data->active_channels_mask, data->phys_channels) {
		int ret;

		ret = regmap_update_bits(data->regmap, MCP47FEB02_VREF_REG_ADDR,
					 DAC_CTRL_MASK(ch),
					 DAC_CTRL_VAL(ch, data->chdata[ch].ref_mode));
		if (ret)
			return ret;

		ret = regmap_update_bits(data->regmap, MCP47FEB02_GAIN_CTRL_STATUS_REG_ADDR,
					 DAC_GAIN_MASK(ch),
					 DAC_GAIN_VAL(ch, data->chdata[ch].use_2x_gain));
		if (ret)
			return ret;

		ret = regmap_write(data->regmap, REG_ADDR(ch), data->chdata[ch].dac_data);
		if (ret)
			return ret;

		ret = regmap_update_bits(data->regmap, MCP47FEB02_POWER_DOWN_REG_ADDR,
					 DAC_CTRL_MASK(ch),
					 DAC_CTRL_VAL(ch, MCP47FEB02_NORMAL_OPERATION));
		if (ret)
			return ret;

		data->chdata[ch].powerdown = false;
	}

	return 0;
}

static int mcp47feb02_get_powerdown_mode(struct iio_dev *indio_dev,
					 const struct iio_chan_spec *chan)
{
	struct mcp47feb02_data *data = iio_priv(indio_dev);

	guard(mutex)(&data->lock);

	return data->chdata[chan->address].powerdown_mode;
}

static int mcp47feb02_set_powerdown_mode(struct iio_dev *indio_dev, const struct iio_chan_spec *ch,
					 unsigned int mode)
{
	struct mcp47feb02_data *data = iio_priv(indio_dev);

	guard(mutex)(&data->lock);

	data->chdata[ch->address].powerdown_mode = mode;

	return 0;
}

static ssize_t mcp47feb02_read_powerdown(struct iio_dev *indio_dev, uintptr_t private,
					 const struct iio_chan_spec *ch, char *buf)
{
	struct mcp47feb02_data *data = iio_priv(indio_dev);

	guard(mutex)(&data->lock);

	/* Print if channel is in a power-down mode or not */
	return sysfs_emit(buf, "%d\n", data->chdata[ch->address].powerdown);
}

static ssize_t mcp47feb02_write_powerdown(struct iio_dev *indio_dev, uintptr_t private,
					  const struct iio_chan_spec *ch, const char *buf,
					  size_t len)
{
	struct mcp47feb02_data *data = iio_priv(indio_dev);
	u32 reg = ch->address;
	u8 tmp_pd_mode;
	bool state;
	int ret;

	guard(mutex)(&data->lock);

	ret = kstrtobool(buf, &state);
	if (ret)
		return ret;

	/*
	 * Set the channel to the specified power-down mode. Exiting power-down mode
	 * requires writing normal operation mode (0) to the channel-specific register bits.
	 */
	tmp_pd_mode = state ? (data->chdata[reg].powerdown_mode + 1) : MCP47FEB02_NORMAL_OPERATION;
	ret = regmap_update_bits(data->regmap, MCP47FEB02_POWER_DOWN_REG_ADDR,
				 DAC_CTRL_MASK(reg), DAC_CTRL_VAL(reg, tmp_pd_mode));
	if (ret)
		return ret;

	data->chdata[reg].powerdown = state;

	return len;
}

EXPORT_SIMPLE_DEV_PM_OPS(mcp47feb02_pm_ops, mcp47feb02_suspend, mcp47feb02_resume);

static const struct iio_enum mcp47febxx_powerdown_mode_enum = {
	.items = mcp47feb02_powerdown_modes,
	.num_items = ARRAY_SIZE(mcp47feb02_powerdown_modes),
	.get = mcp47feb02_get_powerdown_mode,
	.set = mcp47feb02_set_powerdown_mode,
};

static const struct iio_chan_spec_ext_info mcp47feb02_ext_info[] = {
	{
		.name = "powerdown",
		.read = mcp47feb02_read_powerdown,
		.write = mcp47feb02_write_powerdown,
		.shared = IIO_SEPARATE,
	},
	IIO_ENUM("powerdown_mode", IIO_SEPARATE, &mcp47febxx_powerdown_mode_enum),
	IIO_ENUM_AVAILABLE("powerdown_mode", IIO_SHARED_BY_TYPE, &mcp47febxx_powerdown_mode_enum),
	{ }
};

static const struct iio_chan_spec mcp47febxx_ch_template = {
	.type = IIO_VOLTAGE,
	.output = 1,
	.indexed = 1,
	.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) | BIT(IIO_CHAN_INFO_SCALE),
	.info_mask_separate_available = BIT(IIO_CHAN_INFO_SCALE),
	.ext_info = mcp47feb02_ext_info,
};

static void mcp47feb02_init_scale(struct mcp47feb02_data *data, enum mcp47feb02_scale scale,
				  int vref_uV, int scale_avail[])
{
	u32 tmp;

	/*
	 * Compute the DAC scale as Vref / 2^resolution using 32-bit arithmetic
	 */
	tmp = (vref_uV * (MILLI / 8)) >> (data->chip_features->resolution - 3);
	scale_avail[scale * 2] = tmp / MICRO;
	scale_avail[scale * 2 + 1] = tmp % MICRO;
}

static int mcp47feb02_init_scales_avail(struct mcp47feb02_data *data, int vdd_uV,
					int vref_uV, int vref1_uV)
{
	int tmp_vref;

	mcp47feb02_init_scale(data, MCP47FEB02_SCALE_VDD, vdd_uV, data->scale_0);

	tmp_vref = data->use_vref ? vref_uV : MCP47FEB02_INTERNAL_BAND_GAP_uV;
	mcp47feb02_init_scale(data, MCP47FEB02_SCALE_GAIN_X1, tmp_vref, data->scale_0);
	mcp47feb02_init_scale(data, MCP47FEB02_SCALE_GAIN_X2, tmp_vref * 2, data->scale_0);

	if (data->phys_channels >= 4) {
		mcp47feb02_init_scale(data, MCP47FEB02_SCALE_VDD, vdd_uV, data->scale_1);
		tmp_vref = data->use_vref1 ? vref1_uV : MCP47FEB02_INTERNAL_BAND_GAP_uV;

		mcp47feb02_init_scale(data, MCP47FEB02_SCALE_GAIN_X1,
				      tmp_vref, data->scale_1);
		mcp47feb02_init_scale(data, MCP47FEB02_SCALE_GAIN_X2,
				      tmp_vref * 2, data->scale_1);
	}

	return 0;
}

static int mcp47feb02_read_avail(struct iio_dev *indio_dev, struct iio_chan_spec const *ch,
				 const int **vals, int *type, int *length, long info)
{
	struct mcp47feb02_data *data = iio_priv(indio_dev);

	switch (info) {
	case IIO_CHAN_INFO_SCALE:
		switch (ch->type) {
		case IIO_VOLTAGE:
			*vals = data->chdata[ch->address].scale_avail;
			*length = 2 * MCP47FEB02_MAX_SCALES_CH;
			*type = IIO_VAL_INT_PLUS_MICRO;
			return IIO_AVAIL_LIST;
		default:
			return -EINVAL;
		}
	default:
		return -EINVAL;
	}
}

static void mcp47feb02_get_scale(int ch, struct mcp47feb02_data *data, int *val, int *val2)
{
	enum mcp47feb02_scale current_scale;
	int *scale;

	if (data->chdata[ch].ref_mode == MCP47FEB02_VREF_VDD)
		current_scale = MCP47FEB02_SCALE_VDD;
	else if (data->chdata[ch].use_2x_gain)
		current_scale = MCP47FEB02_SCALE_GAIN_X2;
	else
		current_scale = MCP47FEB02_SCALE_GAIN_X1;

	scale = data->chdata[ch].scale_avail;
	*val = scale[current_scale * 2];
	*val2 = scale[current_scale * 2 + 1];
}

static int mcp47feb02_check_scale(int val, int val2, int scale[])
{
	unsigned int i;

	for (i = 0; i < MCP47FEB02_MAX_SCALES_CH; i++) {
		if (scale[i * 2] == val && scale[i * 2 + 1] == val2)
			return i;
	}

	return -EINVAL;
}

static void mcp47feb02_regulator_disable(void *d)
{
	struct regulator *reg = (struct regulator *)d;

	if (reg)
		regulator_disable(reg);
}

static bool mcp47feb02_ref_mismatch(struct mcp47feb02_data *data, unsigned int ch)
{
	bool use_vref, use_bandgap;

	if (data->chdata[ch].ref_mode == MCP47FEB02_VREF_VDD)
		return false;

	use_vref = (data->phys_channels >= 4 && (ch % 2)) ?  data->use_vref1 : data->use_vref;
	use_bandgap = (data->chdata[ch].ref_mode == MCP47FEB02_INTERNAL_BAND_GAP);

	return use_vref == use_bandgap;
}

static int mcp47feb02_enable_reg(struct mcp47feb02_data *data, struct regulator *reg, bool *enabled)
{
	struct device *dev = regmap_get_device(data->regmap);
	int ret;

	if (*enabled)
		return 0;

	ret = regulator_enable(reg);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to enable regulator\n");

	ret = devm_add_action_or_reset(dev, mcp47feb02_regulator_disable, reg);
	if (ret)
		return ret;

	*enabled = true;

	return 0;
}

static int mcp47feb02_ch_scale(struct mcp47feb02_data *data, int ch, int scale)
{
	unsigned int tmp_val;
	int ret;

	if (scale == MCP47FEB02_SCALE_VDD) {
		tmp_val = MCP47FEB02_VREF_VDD;
	} else if (data->phys_channels >= 4 && (ch % 2)) {
		if (data->use_vref1) {
			ret = mcp47feb02_enable_reg(data, data->vref1_reg, &data->vref1_enabled);
			if (ret)
				return ret;

			if (data->vref1_buffered)
				tmp_val = MCP47FEB02_EXTERNAL_VREF_BUFFERED;
			else
				tmp_val = MCP47FEB02_EXTERNAL_VREF_UNBUFFERED;
		} else {
			tmp_val = MCP47FEB02_INTERNAL_BAND_GAP;
		}
	} else {
		if (data->use_vref) {
			ret = mcp47feb02_enable_reg(data, data->vref_reg, &data->vref_enabled);
			if (ret)
				return ret;

			if (data->vref_buffered)
				tmp_val = MCP47FEB02_EXTERNAL_VREF_BUFFERED;
			else
				tmp_val = MCP47FEB02_EXTERNAL_VREF_UNBUFFERED;
		} else {
			tmp_val = MCP47FEB02_INTERNAL_BAND_GAP;
		}
	}

	ret = regmap_update_bits(data->regmap, MCP47FEB02_VREF_REG_ADDR,
				 DAC_CTRL_MASK(ch), DAC_CTRL_VAL(ch, tmp_val));
	if (ret)
		return ret;

	data->chdata[ch].ref_mode = tmp_val;
	data->chdata[ch].ref_mismatch = false;

	return 0;
}

/*
 * Setting the scale allows selecting between VDD and the external reference
 * input or the internal band gap.
 * When the DAC voltage reference is configured as Internal Band Gap, the Vref
 * pin is an output, so an external voltage reference cannot be connected to
 * the pin at the same time.
 * If Vref/Vref1 voltage is not available, the Internal Band Gap is used to
 * calculate the scale values.
 */
static int mcp47feb02_set_scale(struct mcp47feb02_data *data, int ch, int scale)
{
	unsigned int tmp_val;
	int ret;

	ret = mcp47feb02_ch_scale(data, ch, scale);
	if (ret)
		return ret;

	if (scale == MCP47FEB02_SCALE_GAIN_X2)
		tmp_val = MCP47FEB02_GAIN_BIT_X2;
	else
		tmp_val = MCP47FEB02_GAIN_BIT_X1;

	ret = regmap_update_bits(data->regmap, MCP47FEB02_GAIN_CTRL_STATUS_REG_ADDR,
				 DAC_GAIN_MASK(ch), DAC_GAIN_VAL(ch, tmp_val));
	if (ret)
		return ret;

	data->chdata[ch].use_2x_gain = tmp_val;

	return 0;
}

static int mcp47feb02_read_raw(struct iio_dev *indio_dev, struct iio_chan_spec const *ch,
			       int *val, int *val2, long mask)
{
	struct mcp47feb02_data *data = iio_priv(indio_dev);
	int ret;

	guard(mutex)(&data->lock);

	/*
	 * The reference mode restored from EEPROM may not match the current
	 * device tree configuration. Access will be allowed after a matching
	 * scale is written by the user.
	 */
	if (data->chdata[ch->address].ref_mismatch)
		return -EBUSY;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		ret = regmap_read(data->regmap, REG_ADDR(ch->address), val);
		if (ret)
			return ret;
		return IIO_VAL_INT;
	case IIO_CHAN_INFO_SCALE:
		mcp47feb02_get_scale(ch->address, data, val, val2);
		return IIO_VAL_INT_PLUS_MICRO;
	default:
		return -EINVAL;
	}
}

static int mcp47feb02_write_raw(struct iio_dev *indio_dev, struct iio_chan_spec const *ch,
				int val, int val2, long mask)
{
	struct mcp47feb02_data *data = iio_priv(indio_dev);
	int ret;

	guard(mutex)(&data->lock);

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		 /*
		  * Allow only scale write operation until reference mismatch is corrected.
		  */
		if (data->chdata[ch->address].ref_mismatch)
			return -EBUSY;

		if (val < 0 || val >= BIT(data->chip_features->resolution))
			return -EINVAL;

		ret = regmap_write(data->regmap, REG_ADDR(ch->address), val);
		if (ret)
			return ret;

		data->chdata[ch->address].dac_data = val;
		return 0;
	case IIO_CHAN_INFO_SCALE:
		ret = mcp47feb02_check_scale(val, val2,  data->chdata[ch->address].scale_avail);
		if (ret < 0)
			return ret;

		return mcp47feb02_set_scale(data, ch->address, ret);
	default:
		return -EINVAL;
	}
}

static int mcp47feb02_read_label(struct iio_dev *indio_dev, struct iio_chan_spec const *ch,
				 char *label)
{
	struct mcp47feb02_data *data = iio_priv(indio_dev);

	return sysfs_emit(label, "%s\n", data->labels[ch->address]);
}

static const struct iio_info mcp47feb02_info = {
	.read_raw = mcp47feb02_read_raw,
	.write_raw = mcp47feb02_write_raw,
	.read_label = mcp47feb02_read_label,
	.read_avail = &mcp47feb02_read_avail,
	.attrs = &mcp47feb02_attribute_group,
};

static const struct iio_info mcp47fvb02_info = {
	.read_raw = mcp47feb02_read_raw,
	.write_raw = mcp47feb02_write_raw,
	.read_label = mcp47feb02_read_label,
	.read_avail = &mcp47feb02_read_avail,
};

static int mcp47feb02_parse_fw(struct iio_dev *indio_dev)
{
	struct iio_chan_spec chanspec = mcp47febxx_ch_template;
	struct mcp47feb02_data *data = iio_priv(indio_dev);
	struct device *dev = regmap_get_device(data->regmap);
	struct iio_chan_spec *channels;
	u32 num_channels;
	u8 chan_idx;

	num_channels = device_get_child_node_count(dev);
	if (num_channels > data->phys_channels)
		return dev_err_probe(dev, -EINVAL, "More channels than the chip supports\n");

	if (num_channels == 0)
		return dev_err_probe(dev, -EINVAL, "No channel specified in the devicetree\n");

	channels = devm_kcalloc(dev, num_channels, sizeof(*channels), GFP_KERNEL);
	if (!channels)
		return -ENOMEM;

	chan_idx = 0;
	device_for_each_child_node_scoped(dev, child) {
		u32 reg;
		int ret;

		ret = fwnode_property_read_u32(child, "reg", &reg);
		if (ret)
			return dev_err_probe(dev, ret, "Invalid channel number\n");

		if (reg >= data->phys_channels)
			return dev_err_probe(dev, -EINVAL,
					     "The index of the channels does not match the chip\n");

		__set_bit(reg, &data->active_channels_mask);

		ret = fwnode_property_read_string(child, "label", &data->labels[reg]);
		if (ret)
			dev_dbg(dev, "%pfw: invalid label\n", child);

		chanspec.address = reg;
		chanspec.channel = reg;
		channels[chan_idx] = chanspec;
		chan_idx++;
	}

	indio_dev->num_channels = num_channels;
	indio_dev->channels = channels;
	indio_dev->modes = INDIO_DIRECT_MODE;

	data->vref_buffered = device_property_read_bool(dev, "microchip,vref-buffered");

	if (data->chip_features->have_ext_vref1)
		data->vref1_buffered = device_property_read_bool(dev, "microchip,vref1-buffered");

	return 0;
}

static int mcp47feb02_init_ctrl_regs(struct mcp47feb02_data *data)
{
	unsigned int i, vref_ch, gain_ch, pd_ch;
	int ret;

	ret = regmap_read(data->regmap, MCP47FEB02_VREF_REG_ADDR, &vref_ch);
	if (ret)
		return ret;

	ret = regmap_read(data->regmap, MCP47FEB02_GAIN_CTRL_STATUS_REG_ADDR, &gain_ch);
	if (ret)
		return ret;

	ret = regmap_read(data->regmap, MCP47FEB02_POWER_DOWN_REG_ADDR, &pd_ch);
	if (ret)
		return ret;

	gain_ch = gain_ch & MCP47FEB02_GAIN_BITS_MASK;
	for_each_set_bit(i, &data->active_channels_mask, data->phys_channels) {
		struct device *dev = regmap_get_device(data->regmap);
		unsigned int pd_tmp;

		data->chdata[i].ref_mode = (vref_ch >> (2 * i)) & MCP47FEB02_DAC_CTRL_MASK;
		data->chdata[i].use_2x_gain = (gain_ch >> i) & MCP47FEB02_GAIN_BIT_MASK;

		/*
		 * Report a mismatch between the configuration restored from EEPROM in the
		 * volatile registers at startup and the current device tree.
		 *
		 * The device tree determines which scale options are available, but
		 * the user needs to write the selected scale if it is different from
		 * the saved configuration.
		 */
		data->chdata[i].ref_mismatch = mcp47feb02_ref_mismatch(data, i);
		if (data->chdata[i].ref_mismatch) {
			unsigned int ref_mode = data->chdata[i].ref_mode;
			const char *restored_vref;
			const char *dt_vref;

			if (ref_mode == MCP47FEB02_VREF_VDD) {
				restored_vref = MCP47FEB02_VDD_VOLTAGE_REFERENCE;
			} else if (ref_mode == MCP47FEB02_INTERNAL_BAND_GAP) {
				restored_vref = MCP47FEB02_INTERNAL_BAND_GAP_VOLTAGE_REFERENCE;
			} else if ((ref_mode == MCP47FEB02_EXTERNAL_VREF_UNBUFFERED ||
				ref_mode == MCP47FEB02_EXTERNAL_VREF_BUFFERED) &&
				data->phys_channels >= 4 && (i % 2)) {
				restored_vref = MCP47FEB02_SECOND_VOLTAGE_REFERENCE;
			} else {
				restored_vref = MCP47FEB02_PRIMARY_VOLTAGE_REFERENCE;
			}

			if (data->phys_channels >= 4 && (i % 2))
				if (data->use_vref1)
					dt_vref = MCP47FEB02_SECOND_VOLTAGE_REFERENCE;
				else
					dt_vref = MCP47FEB02_INTERNAL_BAND_GAP_VOLTAGE_REFERENCE;
			else
				if (data->use_vref)
					dt_vref = MCP47FEB02_PRIMARY_VOLTAGE_REFERENCE;
				else
					dt_vref = MCP47FEB02_INTERNAL_BAND_GAP_VOLTAGE_REFERENCE;

			dev_info(dev, "ch[%u]: restored configuration uses %s\n",
				 i, restored_vref);
			dev_info(dev,
				 "ch[%u]: currently selected vref will not be in scale_available\n",
				 i);
			dev_info(dev, "ch[%u]: DT describes %s as the available reference source\n",
				 i, dt_vref);
		} else if (data->chdata[i].ref_mode == MCP47FEB02_EXTERNAL_VREF_UNBUFFERED ||
			   data->chdata[i].ref_mode == MCP47FEB02_EXTERNAL_VREF_BUFFERED) {
			unsigned int reg_vref;
			bool is_buf;

			if (data->phys_channels >= 4 && (i % 2))
				is_buf = data->vref1_buffered;
			else
				is_buf = data->vref_buffered;

			if (is_buf)
				reg_vref = MCP47FEB02_EXTERNAL_VREF_BUFFERED;
			else
				reg_vref = MCP47FEB02_EXTERNAL_VREF_UNBUFFERED;

			if (data->chdata[i].ref_mode != reg_vref) {
				ret = regmap_update_bits(data->regmap, MCP47FEB02_VREF_REG_ADDR,
							 DAC_CTRL_MASK(i),
							 DAC_CTRL_VAL(i, reg_vref));
				if (ret)
					return ret;

				data->chdata[i].ref_mode = reg_vref;
				dev_info(dev, "Mismatch buffer property for ch[%u]\n", i);
			}
		}

		pd_tmp = (pd_ch >> (2 * i)) & MCP47FEB02_DAC_CTRL_MASK;
		data->chdata[i].powerdown_mode = pd_tmp ? (pd_tmp - 1) : pd_tmp;
		data->chdata[i].powerdown = !!pd_tmp;
	}

	return 0;
}

static int mcp47feb02_init_ch_scales(struct mcp47feb02_data *data, int vdd_uV,
				     int vref_uV, int vref1_uV)
{
	struct device *dev = regmap_get_device(data->regmap);
	unsigned int i;
	int ret;

	ret = mcp47feb02_init_scales_avail(data, vdd_uV, vref_uV, vref1_uV);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to init scales\n");

	for_each_set_bit(i, &data->active_channels_mask, data->phys_channels) {
		if (data->phys_channels >= 4 && (i % 2))
			data->chdata[i].scale_avail = data->scale_1;
		else
			data->chdata[i].scale_avail = data->scale_0;
	}

	return 0;
}

static int mcp47feb02_try_enable_reg(struct mcp47feb02_data *data)
{
	unsigned int i;
	int ret;

	for_each_set_bit(i, &data->active_channels_mask, data->phys_channels) {
		if (data->chdata[i].ref_mismatch)
			continue;

		if (data->chdata[i].ref_mode < MCP47FEB02_EXTERNAL_VREF_UNBUFFERED)
			continue;

		if (data->phys_channels >= 4 && (i % 2))
			ret = mcp47feb02_enable_reg(data, data->vref1_reg, &data->vref1_enabled);
		else
			ret = mcp47feb02_enable_reg(data, data->vref_reg, &data->vref_enabled);
		if (ret)
			return ret;
	}

	return 0;
}

int mcp47feb02_common_probe(const struct mcp47feb02_features *chip_features, struct regmap *regmap)
{
	struct device *dev = regmap_get_device(regmap);
	int vref1_uV, vref_uV, vdd_uV;
	struct mcp47feb02_data *data;
	struct regulator *vref_reg;
	struct iio_dev *indio_dev;
	int ret;

	indio_dev = devm_iio_device_alloc(dev, sizeof(*data));
	if (!indio_dev)
		return -ENOMEM;

	data = iio_priv(indio_dev);
	data->chip_features = chip_features;
	data->phys_channels = chip_features->phys_channels;
	data->regmap = regmap;

	indio_dev->info = chip_features->have_eeprom ? &mcp47feb02_info : &mcp47fvb02_info;
	indio_dev->name = chip_features->name;

	ret = devm_mutex_init(dev, &data->lock);
	if (ret)
		return ret;

	ret = mcp47feb02_parse_fw(indio_dev);
	if (ret)
		return dev_err_probe(dev, ret, "Error parsing firmware data\n");

	ret = devm_regulator_get_enable_read_voltage(dev, "vdd");
	if (ret < 0)
		return ret;

	vdd_uV = ret;

	if (device_property_present(dev, "vref-supply")) {
		vref_reg = devm_regulator_get(dev, "vref");
		if (IS_ERR(vref_reg))
			return PTR_ERR(vref_reg);

		data->vref_reg = vref_reg;

		vref_uV = regulator_get_voltage(vref_reg);
		if (vref_uV < 0)
			return vref_uV;

		if (vref_uV > vdd_uV / 2)
			dev_info(dev,
				 "Using VREF with Gain 2x will be limited to VDD, %d\n", vref_uV);

		if (vref_uV > 0)
			data->use_vref = true;
		else
			dev_dbg(dev, "Vref is 0 uV, internal band gap will be used.\n");
	} else {
		vref_uV = 0;
		dev_dbg(dev, "Using internal band gap as voltage reference.\n");
	}

	if (chip_features->have_ext_vref1 && device_property_present(dev, "vref1-supply")) {
		vref_reg = devm_regulator_get(dev, "vref1");
		if (IS_ERR(vref_reg))
			return PTR_ERR(vref_reg);

		data->vref1_reg = vref_reg;

		vref1_uV = regulator_get_voltage(vref_reg);
		if (vref1_uV < 0)
			return vref1_uV;

		if (vref1_uV > vdd_uV / 2)
			dev_info(dev,
				 "Using VREF1 with Gain 2x will be limited to VDD, %d\n", vref1_uV);

		if (vref1_uV > 0)
			data->use_vref1 = true;
		else
			dev_dbg(dev, "Vref1 is 0 uV, internal band gap will be used.\n");
	} else {
		vref1_uV = 0;
		dev_dbg(dev, "Using internal band gap as voltage reference 1.\n");
	}

	ret = mcp47feb02_init_ctrl_regs(data);
	if (ret)
		return dev_err_probe(dev, ret, "Error initialising control registers\n");

	ret = mcp47feb02_try_enable_reg(data);
	if (ret)
		return ret;

	ret = mcp47feb02_init_ch_scales(data, vdd_uV, vref_uV, vref1_uV);
	if (ret)
		return ret;

	return devm_iio_device_register(dev, indio_dev);
}
EXPORT_SYMBOL_NS_GPL(mcp47feb02_common_probe, "IIO_MCP47FEB02");

MODULE_AUTHOR("Ariana Lazar <ariana.lazar@microchip.com>");
MODULE_DESCRIPTION("IIO driver for MCP47FEB02 Multi-Channel DAC with I2C interface");
MODULE_LICENSE("GPL");

