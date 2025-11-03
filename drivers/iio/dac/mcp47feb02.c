// SPDX-License-Identifier: GPL-2.0+
/*
 * IIO driver for MCP47FEB02 Multi-Channel DAC with I2C interface
 *
 * Copyright (C) 2025 Microchip Technology Inc. and its subsidiaries
 *
 * Author: Ariana Lazar <ariana.lazar@microchip.com>
 *
 * Datasheet for MCP47FEBXX can be found here:
 * https://ww1.microchip.com/downloads/aemDocuments/documents/OTH/ProductDocuments/DataSheets/20005375A.pdf
 *
 * Datasheet for MCP47FVBXX can be found here:
 * https://ww1.microchip.com/downloads/aemDocuments/documents/OTH/ProductDocuments/DataSheets/20005405A.pdf
 *
 * Datasheet for MCP47FXBX4/8 can be found here:
 * https://ww1.microchip.com/downloads/aemDocuments/documents/MSLD/ProductDocuments/DataSheets/MCP47FXBX48-Data-Sheet-DS200006368A.pdf
 */
#include <linux/bits.h>
#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/i2c.h>
#include <linux/iio/iio.h>
#include <linux/iio/sysfs.h>
#include <linux/module.h>
#include <linux/mod_devicetable.h>
#include <linux/mutex.h>
#include <linux/property.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>

#define MCP47FEB02_DAC0_REG_ADDR			(0x00 << 3)
#define MCP47FEB02_VREF_REG_ADDR			(0x08 << 3)
#define MCP47FEB02_POWER_DOWN_REG_ADDR			(0x09 << 3)
#define MCP47FEB02_GAIN_BIT_STATUS_REG_ADDR		(0x0A << 3)
#define MCP47FEB02_WIPERLOCK_STATUS_REG_ADDR		(0x0B << 3)

#define MCP47FEB02_NV_DAC0_REG_ADDR			(0x10 << 3)
#define MCP47FEB02_NV_DAC1_REG_ADDR			(0x11 << 3)
#define MCP47FEB02_NV_DAC2_REG_ADDR			(0x12 << 3)
#define MCP47FEB02_NV_DAC3_REG_ADDR			(0x13 << 3)
#define MCP47FEB02_NV_DAC4_REG_ADDR			(0x14 << 3)
#define MCP47FEB02_NV_DAC5_REG_ADDR			(0x15 << 3)
#define MCP47FEB02_NV_DAC6_REG_ADDR			(0x16 << 3)
#define MCP47FEB02_NV_DAC7_REG_ADDR			(0x17 << 3)
#define MCP47FEB02_NV_VREF_REG_ADDR			(0x18 << 3)
#define MCP47FEB02_NV_POWER_DOWN_REG_ADDR		(0x19 << 3)
#define MCP47FEB02_NV_GAIN_BIT_I2C_SLAVE_REG_ADDR	(0x1A << 3)

#define MCP47FEBXX_MAX_CH				8
#define MCP47FEB02_GAIN_BIT_X1				0
#define MCP47FEB02_GAIN_BIT_X2				1
#define MCP47FEB02_MAX_VALS_SCALES_CH			6
#define MCP47FEB02_MAX_SCALES_CH			3
#define MCP47FEB02_DAC_WIPER_UNLOCKED			0
#define MCP47FEB02_INTERNAL_BAND_GAP_MV			2440
#define MCP47FEB02_DELAY_1_MS				1000

#define SET_DAC_CTRL_MASK				GENMASK(1, 0)
#define SET_GAIN_BIT					BIT(0)
#define READFLAG_MASK					GENMASK(2, 1)
#define MCP47FEB02_GAIN_BIT_STATUS_EEWA_MASK		BIT(6)
#define MCP47FEB02_VOLATILE_GAIN_BIT_MASK		GENMASK(15, 8)
#define MCP47FEB02_NV_I2C_SLAVE_ADDR_MASK		GENMASK(7, 0)

/* Voltage reference, Power-Down control register and DAC Wiperlock status register fields */
#define DAC_CTRL_MASK(ch)				(GENMASK(1, 0) << (2 * (ch)))
#define DAC_CTRL_VAL(ch, val)				((val) << (2 * (ch)))

/* Gain Control and I2C Slave Address Reguster fields */
#define DAC_GAIN_MASK(ch)				(BIT(0) << (8 + (ch)))
#define DAC_GAIN_VAL(ch, val)				((val) << (8 + (ch)))

enum vref_mode {
	MCP47FEB02_VREF_VDD = 0,
	MCP47FEB02_INTERNAL_BAND_GAP = 1,
	MCP47FEB02_EXTERNAL_VREF_UNBUFFERED = 2,
	MCP47FEB02_EXTERNAL_VREF_BUFFERED = 3,
};

enum mcp47feb02_scale {
	MCP47FEB02_SCALE_VDD = 0,
	MCP47FEB02_SCALE_GAIN_BIT_X1 = 1,
	MCP47FEB02_SCALE_GAIN_BIT_X2 = 2,
};

enum iio_powerdown_mode {
	MCP47FEB02_NORMAL_OPERATION = 0,
	MCP47FEB02_IIO_1K = 1,
	MCP47FEB02_IIO_100K = 2,
	MCP47FEB02_OPEN_CIRCUIT = 3,
};

static const char * const mcp47feb02_powerdown_modes[] = {
	"1kohm_to_gnd",
	"100kohm_to_gnd",
	"open_circuit",
};

/**
 * struct mcp47feb02_features - chip specific data
 * @name:		device name
 * @phys_channels:	number of hardware channels
 * @resolution:		DAC resolution
 * @have_ext_vref1:	does the hardware have an the second external voltage reference?
 * @have_eeprom:	does the hardware have an internal eeprom?
 */
struct mcp47feb02_features {
	const char	*name;
	unsigned int	phys_channels;
	unsigned int	resolution;
	bool have_ext_vref1;
	bool have_eeprom;
};

static const struct mcp47feb02_features mcp47feb01_chip_info = {
	.name = "mcp47feb01",
	.phys_channels = 1,
	.resolution = 8,
	.have_ext_vref1 = false,
	.have_eeprom = true,
};

static const struct mcp47feb02_features mcp47feb11_chip_info = {
	.name = "mcp47feb11",
	.phys_channels = 1,
	.resolution = 10,
	.have_ext_vref1 = false,
	.have_eeprom = true,
};

static const struct mcp47feb02_features mcp47feb21_chip_info = {
	.name = "mcp47feb21",
	.phys_channels = 1,
	.resolution = 12,
	.have_ext_vref1 = false,
	.have_eeprom = true,
};

static const struct mcp47feb02_features mcp47feb02_chip_info = {
	.name = "mcp47feb02",
	.phys_channels = 2,
	.resolution = 8,
	.have_ext_vref1 = false,
	.have_eeprom = true,
};

static const struct mcp47feb02_features mcp47feb12_chip_info = {
	.name = "mcp47feb12",
	.phys_channels = 2,
	.resolution = 10,
	.have_ext_vref1 = false,
	.have_eeprom = true,
};

static const struct mcp47feb02_features mcp47feb22_chip_info = {
	.name = "mcp47feb22",
	.phys_channels = 2,
	.resolution = 12,
	.have_ext_vref1 = false,
	.have_eeprom = true,
};

static const struct mcp47feb02_features mcp47feb04_chip_info = {
	.name = "mcp47feb04",
	.phys_channels = 4,
	.resolution = 8,
	.have_ext_vref1 = true,
	.have_eeprom = true,
};

static const struct mcp47feb02_features mcp47feb14_chip_info = {
	.name = "mcp47feb14",
	.phys_channels = 4,
	.resolution = 10,
	.have_ext_vref1 = true,
	.have_eeprom = true,
};

static const struct mcp47feb02_features mcp47feb24_chip_info = {
	.name = "mcp47feb24",
	.phys_channels = 4,
	.resolution = 12,
	.have_ext_vref1 = true,
	.have_eeprom = true,
};

static const struct mcp47feb02_features mcp47feb08_chip_info = {
	.name = "mcp47feb08",
	.phys_channels = 8,
	.resolution = 8,
	.have_ext_vref1 = true,
	.have_eeprom = true,
};

static const struct mcp47feb02_features mcp47feb18_chip_info = {
	.name = "mcp47feb18",
	.phys_channels = 8,
	.resolution = 10,
	.have_ext_vref1 = true,
	.have_eeprom = true,
};

static const struct mcp47feb02_features mcp47feb28_chip_info = {
	.name = "mcp47feb28",
	.phys_channels = 8,
	.resolution = 12,
	.have_ext_vref1 = true,
	.have_eeprom = true,
};

static const struct mcp47feb02_features mcp47fvb01_chip_info = {
	.name = "mcp47fvb01",
	.phys_channels = 1,
	.resolution = 8,
	.have_ext_vref1 = false,
	.have_eeprom = false,
};

static const struct mcp47feb02_features mcp47fvb11_chip_info = {
	.name = "mcp47fvb11",
	.phys_channels = 1,
	.resolution = 10,
	.have_ext_vref1 = false,
	.have_eeprom = false,
};

static const struct mcp47feb02_features mcp47fvb21_chip_info = {
	.name = "mcp47fvb21",
	.phys_channels = 1,
	.resolution = 12,
	.have_ext_vref1 = false,
	.have_eeprom = false,
};

static const struct mcp47feb02_features mcp47fvb02_chip_info = {
	.name = "mcp47fvb02",
	.phys_channels = 2,
	.resolution = 8,
	.have_ext_vref1 = false,
	.have_eeprom = false,
};

static const struct mcp47feb02_features mcp47fvb12_chip_info = {
	.name = "mcp47fvb12",
	.phys_channels = 2,
	.resolution = 8,
	.have_ext_vref1 = false,
	.have_eeprom = false,
};

static const struct mcp47feb02_features mcp47fvb22_chip_info = {
	.name = "mcp47fvb22",
	.phys_channels = 2,
	.resolution = 12,
	.have_ext_vref1 = false,
	.have_eeprom = false,
};

static const struct mcp47feb02_features mcp47fvb04_chip_info = {
	.name = "mcp47fvb04",
	.phys_channels = 4,
	.resolution = 8,
	.have_ext_vref1 = true,
	.have_eeprom = false,
};

static const struct mcp47feb02_features mcp47fvb14_chip_info = {
	.name = "mcp47fvb14",
	.phys_channels = 4,
	.resolution = 10,
	.have_ext_vref1 = true,
	.have_eeprom = false,
};

static const struct mcp47feb02_features mcp47fvb24_chip_info = {
	.name = "mcp47fvb24",
	.phys_channels = 4,
	.resolution = 12,
	.have_ext_vref1 = true,
	.have_eeprom = false,
};

static const struct mcp47feb02_features mcp47fvb08_chip_info = {
	.name = "mcp47fvb08",
	.phys_channels = 8,
	.resolution = 8,
	.have_ext_vref1 = true,
	.have_eeprom = false,
};

static const struct mcp47feb02_features mcp47fvb18_chip_info = {
	.name = "mcp47fvb18",
	.phys_channels = 8,
	.resolution = 10,
	.have_ext_vref1 = true,
	.have_eeprom = false,
};

static const struct mcp47feb02_features mcp47fvb28_chip_info = {
	.name = "mcp47fvb28",
	.phys_channels = 4,
	.resolution = 8,
	.have_ext_vref1 = true,
	.have_eeprom = false,
};

/**
 * struct mcp47feb02_channel_data - channel configuration
 * @ref_mode: chosen voltage for reference
 * @powerdown_mode: selected power-down mode
 * @use_2x_gain: output driver gain control
 * @powerdown: is false if the channel is in normal operation mode
 * @dac_data: read dac value
 */
struct mcp47feb02_channel_data {
	enum vref_mode ref_mode;
	u8 powerdown_mode;
	bool use_2x_gain;
	bool powerdown;
	u16 dac_data;
};

/**
 * struct mcp47feb02_data - chip configuration
 * @chdata: options configured for each channel on the device
 * @scale: scales set on channels that are based on Vref/Vref0
 * @scale_1:  scales set on channels that are based on Vref1
 * @info: pointer to features struct
 * @labels: table with channels labels
 * @active_channels_mask: enabled channels
 * @client: the i2c-client attached to the device
 * @regmap: regmap for directly accessing device register
 * @vref1_buffered: Vref1 buffer is enabled
 * @vref_buffered: Vref/Vref0 buffer is enabled
 * @phys_channels: physical channels on the device
 * @lock: prevents concurrent reads/writes
 * @use_vref1: vref1-supply is defined
 * @use_vref: vref-supply is defined
 */
struct mcp47feb02_data {
	struct mcp47feb02_channel_data chdata[MCP47FEBXX_MAX_CH];
	int scale_1[MCP47FEB02_MAX_VALS_SCALES_CH];
	int scale[MCP47FEB02_MAX_VALS_SCALES_CH];
	const struct mcp47feb02_features *info;
	const char *labels[MCP47FEBXX_MAX_CH];
	unsigned long active_channels_mask;
	struct i2c_client *client;
	struct regmap *regmap;
	bool vref1_buffered;
	bool vref_buffered;
	u16 phys_channels;
	struct mutex lock; /* synchronize access to driver's state members */
	bool use_vref1;
	bool use_vref;
};

static const struct regmap_range mcp47feb02_readable_ranges[] = {
	regmap_reg_range(MCP47FEB02_DAC0_REG_ADDR, MCP47FEB02_WIPERLOCK_STATUS_REG_ADDR),
	regmap_reg_range(MCP47FEB02_NV_DAC0_REG_ADDR, MCP47FEB02_NV_GAIN_BIT_I2C_SLAVE_REG_ADDR),
};

static const struct regmap_range mcp47feb02_writable_ranges[] = {
	regmap_reg_range(MCP47FEB02_DAC0_REG_ADDR, MCP47FEB02_WIPERLOCK_STATUS_REG_ADDR),
	regmap_reg_range(MCP47FEB02_NV_DAC0_REG_ADDR, MCP47FEB02_NV_GAIN_BIT_I2C_SLAVE_REG_ADDR),
};

static const struct regmap_range mcp47feb02_volatile_ranges[] = {
	regmap_reg_range(MCP47FEB02_DAC0_REG_ADDR, MCP47FEB02_WIPERLOCK_STATUS_REG_ADDR),
	regmap_reg_range(MCP47FEB02_NV_DAC0_REG_ADDR, MCP47FEB02_NV_GAIN_BIT_I2C_SLAVE_REG_ADDR),
	regmap_reg_range(MCP47FEB02_DAC0_REG_ADDR, MCP47FEB02_WIPERLOCK_STATUS_REG_ADDR),
	regmap_reg_range(MCP47FEB02_NV_DAC0_REG_ADDR, MCP47FEB02_NV_GAIN_BIT_I2C_SLAVE_REG_ADDR),
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

static const struct regmap_config mcp47feb02_regmap_config = {
	.name = "mcp47feb02_regmap",
	.reg_bits = 8,
	.val_bits = 16,
	.rd_table = &mcp47feb02_readable_table,
	.wr_table = &mcp47feb02_writable_table,
	.volatile_table = &mcp47feb02_volatile_table,
	.max_register =  MCP47FEB02_NV_GAIN_BIT_I2C_SLAVE_REG_ADDR,
	.read_flag_mask	= READFLAG_MASK,
	.cache_type = REGCACHE_MAPLE,
	.val_format_endian = REGMAP_ENDIAN_BIG,
};

/* For devices that doesn't have nonvolatile memory */
static const struct regmap_range mcp47fvb02_readable_ranges[] = {
	regmap_reg_range(MCP47FEB02_DAC0_REG_ADDR, MCP47FEB02_WIPERLOCK_STATUS_REG_ADDR),
};

static const struct regmap_range mcp47fvb02_writable_ranges[] = {
	regmap_reg_range(MCP47FEB02_DAC0_REG_ADDR, MCP47FEB02_WIPERLOCK_STATUS_REG_ADDR),
};

static const struct regmap_range mcp47fvb02_volatile_ranges[] = {
	regmap_reg_range(MCP47FEB02_DAC0_REG_ADDR, MCP47FEB02_WIPERLOCK_STATUS_REG_ADDR),
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

static const struct regmap_config mcp47fvb02_regmap_config = {
	.name = "mcp47fvb02_regmap",
	.reg_bits = 8,
	.val_bits = 16,
	.rd_table = &mcp47fvb02_readable_table,
	.wr_table = &mcp47fvb02_writable_table,
	.volatile_table = &mcp47fvb02_volatile_table,
	.max_register = MCP47FEB02_WIPERLOCK_STATUS_REG_ADDR,
	.read_flag_mask	= READFLAG_MASK,
	.cache_type = REGCACHE_MAPLE,
	.val_format_endian = REGMAP_ENDIAN_BIG,
};

static int mcp47feb02_write_to_eeprom(struct mcp47feb02_data *data, unsigned int reg,
				      unsigned int val)
{
	int eewa_val, ret;

	/*
	 * Wait till the currently occurring EEPROM Write Cycle is completed.
	 * Only serial commands to the volatile memory are allowed.
	 */
	guard(mutex)(&data->lock);

	ret = regmap_read_poll_timeout(data->regmap, MCP47FEB02_GAIN_BIT_STATUS_REG_ADDR,
				       eewa_val,
				       !(eewa_val & MCP47FEB02_GAIN_BIT_STATUS_EEWA_MASK),
				       MCP47FEB02_DELAY_1_MS, MCP47FEB02_DELAY_1_MS * 5);
	if (ret)
		return ret;

	return regmap_write(data->regmap, reg, val);
}

static ssize_t mcp47feb02_store_eeprom(struct device *dev, struct device_attribute *attr,
				       const char *buf, size_t len)
{
	struct mcp47feb02_data *data = iio_priv(dev_to_iio_dev(dev));
	int ret, i, val, val1, eewa_val;
	bool state;

	ret = kstrtobool(buf, &state);
	if (ret < 0)
		return ret;

	if (!state)
		return 0;

	/*
	 * Verify DAC Wiper and DAC Configuratioin are unlocked. If both are disabled,
	 * writing to EEPROM is available.
	 */
	ret = regmap_read(data->regmap, MCP47FEB02_WIPERLOCK_STATUS_REG_ADDR, &val);
	if (ret)
		return ret;

	if (val)  {
		dev_err(dev, "DAC Wiper and DAC Configuration not are unlocked.\n");
		return -EINVAL;
	}

	for_each_set_bit(i, &data->active_channels_mask, data->phys_channels) {
		ret = mcp47feb02_write_to_eeprom(data, i << 3, data->chdata[i].dac_data);
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

	ret = regmap_read_poll_timeout(data->regmap, MCP47FEB02_GAIN_BIT_STATUS_REG_ADDR, eewa_val,
				       !(eewa_val & MCP47FEB02_GAIN_BIT_STATUS_EEWA_MASK),
				       MCP47FEB02_DELAY_1_MS, MCP47FEB02_DELAY_1_MS * 5);
	if (ret)
		return ret;

	ret = regmap_read(data->regmap, MCP47FEB02_NV_GAIN_BIT_I2C_SLAVE_REG_ADDR, &val);
	if (ret)
		return ret;

	ret = regmap_read(data->regmap, MCP47FEB02_GAIN_BIT_STATUS_REG_ADDR, &val1);
	if (ret)
		return ret;

	ret = mcp47feb02_write_to_eeprom(data, MCP47FEB02_NV_GAIN_BIT_I2C_SLAVE_REG_ADDR,
					 (val1 & MCP47FEB02_VOLATILE_GAIN_BIT_MASK) |
					 (val & MCP47FEB02_NV_I2C_SLAVE_ADDR_MASK));
	if (ret)
		return ret;

	return len;
}

static IIO_DEVICE_ATTR(store_eeprom, 0200, NULL, mcp47feb02_store_eeprom, 0);
static struct attribute *mcp47feb02_attributes[] = {
	&iio_dev_attr_store_eeprom.dev_attr.attr,
	NULL
};

static const struct attribute_group mcp47feb02_attribute_group = {
	.attrs = mcp47feb02_attributes,
};

static int mcp47feb02_suspend(struct device *dev)
{
	struct iio_dev *indio_dev = dev_get_drvdata(dev);
	struct mcp47feb02_data *data = iio_priv(indio_dev);
	int ret, ch;
	u8 pd_mode;

	guard(mutex)(&data->lock);

	for_each_set_bit(ch, &data->active_channels_mask, data->phys_channels) {
		data->chdata[ch].powerdown = true;
		pd_mode = data->chdata[ch].powerdown_mode + 1;
		regmap_update_bits(data->regmap, MCP47FEB02_POWER_DOWN_REG_ADDR,
				   DAC_CTRL_MASK(ch), DAC_CTRL_VAL(ch, pd_mode));
		if (ret)
			return ret;

		ret = regmap_write(data->regmap, ch << 3, data->chdata[ch].dac_data);
		if (ret)
			return ret;
	}

	return 0;
}

static int mcp47feb02_resume(struct device *dev)
{
	struct iio_dev *indio_dev = dev_get_drvdata(dev);
	struct mcp47feb02_data *data = iio_priv(indio_dev);
	int ch, ret;
	u8 pd_mode;

	guard(mutex)(&data->lock);

	for_each_set_bit(ch, &data->active_channels_mask, data->phys_channels) {
		data->chdata[ch].powerdown = false;
		pd_mode = data->chdata[ch].powerdown_mode + 1;

		ret = regmap_write(data->regmap, ch << 3, data->chdata[ch].dac_data);
		if (ret)
			return ret;

		ret = regmap_update_bits(data->regmap, MCP47FEB02_VREF_REG_ADDR,
					 DAC_CTRL_MASK(ch), DAC_CTRL_VAL(ch, pd_mode));
		if (ret)
			return ret;

		ret = regmap_update_bits(data->regmap, MCP47FEB02_GAIN_BIT_STATUS_REG_ADDR,
					 DAC_GAIN_MASK(ch),
					 DAC_GAIN_VAL(ch, data->chdata[ch].use_2x_gain));
		if (ret)
			return ret;

		ret = regmap_update_bits(data->regmap, MCP47FEB02_POWER_DOWN_REG_ADDR,
					 DAC_CTRL_MASK(ch),
					 DAC_CTRL_VAL(ch, MCP47FEB02_NORMAL_OPERATION));
		if (ret)
			return ret;
	}

	return 0;
}

static int mcp47feb02_get_powerdown_mode(struct iio_dev *indio_dev,
					 const struct iio_chan_spec *chan)
{
	struct mcp47feb02_data *data = iio_priv(indio_dev);

	return data->chdata[chan->address].powerdown_mode;
}

static int mcp47feb02_set_powerdown_mode(struct iio_dev *indio_dev, const struct iio_chan_spec *ch,
					 unsigned int mode)
{
	struct mcp47feb02_data *data = iio_priv(indio_dev);

	data->chdata[ch->address].powerdown_mode = mode;

	return 0;
}

static ssize_t mcp47feb02_read_powerdown(struct iio_dev *indio_dev, uintptr_t private,
					 const struct iio_chan_spec *ch, char *buf)
{
	struct mcp47feb02_data *data = iio_priv(indio_dev);

	/* Check if channel is in a power-down mode or not */
	return sysfs_emit(buf, "%d\n", data->chdata[ch->address].powerdown);
}

static ssize_t mcp47feb02_write_powerdown(struct iio_dev *indio_dev, uintptr_t private,
					  const struct iio_chan_spec *ch, const char *buf,
					  size_t len)
{
	struct mcp47feb02_data *data = iio_priv(indio_dev);
	unsigned long reg;
	u8 tmp_pd_mode;
	bool state;
	int ret;

	guard(mutex)(&data->lock);

	ret = kstrtobool(buf, &state);
	if (ret)
		return ret;

	reg = ch->address;

	/*
	 * Set channel to the power-down mode selected. Normal operation mode (0000h)
	 * must be written to register in order to exit  power-down mode.
	 */
	tmp_pd_mode = state ? (data->chdata[reg].powerdown_mode + 1) : MCP47FEB02_NORMAL_OPERATION;
	ret = regmap_update_bits(data->regmap, MCP47FEB02_POWER_DOWN_REG_ADDR,
				 DAC_CTRL_MASK(reg), DAC_CTRL_VAL(reg, tmp_pd_mode));
	if (ret)
		return ret;

	data->chdata[reg].powerdown = state;

	return len;
}

static DEFINE_SIMPLE_DEV_PM_OPS(mcp47feb02_pm_ops, mcp47feb02_suspend, mcp47feb02_resume);

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
				  int vref_mv, int scale_avail[])
{
	int value_micro, value_int;
	s64 tmp;

	tmp = (s64)vref_mv * 1000000LL >> data->info->resolution;
	value_int = div_s64_rem(tmp, 1000000LL, &value_micro);
	scale_avail[scale * 2] = value_int;
	scale_avail[scale * 2 + 1] = value_micro;
}

static int mcp47feb02_init_scales_avail(struct mcp47feb02_data *data, int vdd_mv,
					int vref_mv, int vref1_mv)
{
	struct device *dev = &data->client->dev;
	int tmp_vref;

	mcp47feb02_init_scale(data, MCP47FEB02_SCALE_VDD, vdd_mv, data->scale);

	if (data->use_vref)
		tmp_vref = vref_mv;
	else
		tmp_vref = MCP47FEB02_INTERNAL_BAND_GAP_MV;

	mcp47feb02_init_scale(data, MCP47FEB02_SCALE_GAIN_BIT_X1, tmp_vref, data->scale);
	mcp47feb02_init_scale(data, MCP47FEB02_SCALE_GAIN_BIT_X2, tmp_vref * 2, data->scale);

	if (data->phys_channels >= 4) {
		mcp47feb02_init_scale(data, MCP47FEB02_SCALE_VDD, vdd_mv, data->scale_1);

		if (data->use_vref1 && vref1_mv <= 0)
			return dev_err_probe(dev, -EINVAL, "Invalid voltage for Vref1\n");

		if (data->use_vref1)
			tmp_vref = vref1_mv;
		else
			tmp_vref = MCP47FEB02_INTERNAL_BAND_GAP_MV;

		mcp47feb02_init_scale(data, MCP47FEB02_SCALE_GAIN_BIT_X1,
				      tmp_vref, data->scale_1);
		mcp47feb02_init_scale(data, MCP47FEB02_SCALE_GAIN_BIT_X2,
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
			if (data->phys_channels >= 4 && (ch->address % 2))
				*vals = data->scale_1;
			else
				*vals = data->scale;

			*length = MCP47FEB02_MAX_VALS_SCALES_CH;
			*type = IIO_VAL_INT_PLUS_MICRO;
			return IIO_AVAIL_LIST;
		default:
			return -EINVAL;
		}
	default:
		return -EINVAL;
	}
}

static void mcp47feb02_get_scale_avail(struct mcp47feb02_data *data, int *val, int *val2,
				       enum mcp47feb02_scale scale, int ch)
{
	if (data->phys_channels >= 4 && (ch % 2)) {
		*val = data->scale_1[scale * 2];
		*val2 = data->scale_1[scale * 2 + 1];
	} else {
		*val = data->scale[scale * 2];
		*val2 = data->scale[scale * 2 + 1];
	}
}

static void mcp47feb02_get_scale(int ch, struct mcp47feb02_data *data, int *val, int *val2)
{
	enum mcp47feb02_scale tmp_scale;

	if (data->chdata[ch].ref_mode == MCP47FEB02_VREF_VDD)
		tmp_scale = MCP47FEB02_SCALE_VDD;
	else if (data->chdata[ch].use_2x_gain)
		tmp_scale = MCP47FEB02_SCALE_GAIN_BIT_X2;
	else
		tmp_scale = MCP47FEB02_SCALE_GAIN_BIT_X1;

	mcp47feb02_get_scale_avail(data, val, val2, tmp_scale, ch);
}

static int mcp47feb02_check_scale(struct mcp47feb02_data *data, int val, int val2, int scale[])
{
	for (int i = 0; i < MCP47FEB02_MAX_SCALES_CH; i++) {
		if (scale[i * 2] == val && scale[i * 2 + 1] == val2)
			return i;
	}

	return -EINVAL;
}

static int mcp47feb02_ch_scale(struct mcp47feb02_data *data, int ch, int scale)
{
	int tmp_val, ret;

	if (scale == MCP47FEB02_SCALE_VDD) {
		tmp_val = MCP47FEB02_VREF_VDD;
	} else if (data->phys_channels >= 4 && (ch % 2)) {
		if (data->use_vref1) {
			if (data->vref1_buffered)
				tmp_val = MCP47FEB02_EXTERNAL_VREF_BUFFERED;
			else
				tmp_val = MCP47FEB02_EXTERNAL_VREF_UNBUFFERED;
		} else {
			tmp_val = MCP47FEB02_INTERNAL_BAND_GAP;
		}
	} else if (data->use_vref) {
		if (data->vref_buffered)
			tmp_val = MCP47FEB02_EXTERNAL_VREF_BUFFERED;
		else
			tmp_val = MCP47FEB02_EXTERNAL_VREF_UNBUFFERED;
	} else {
		tmp_val = MCP47FEB02_INTERNAL_BAND_GAP;
	}

	ret = regmap_update_bits(data->regmap, MCP47FEB02_VREF_REG_ADDR,
				 DAC_CTRL_MASK(ch), DAC_CTRL_VAL(ch, tmp_val));
	if (ret)
		return ret;

	data->chdata[ch].ref_mode = tmp_val;

	return 0;
}

/*
 * Setting the scale in order to choose between VDD and (Vref or BandGap) from the user
 * space. You can't have an external voltage reference connected to the pin and select the
 * internal BandGap. The VREF pin is either an input or an output. When the DAC’s voltage
 * reference is configured as the VREF pin, the pin is an input. When the DAC’s voltage
 * reference is configured as the internal BandGap, the pin is an output.
 *
 * If Vref voltage is not available then the internal BandGap will be used to calculate one
 * of the possible scale.
 * If Vref1 voltage is not available then the internal BandGap will be used to calculate
 * one of the possible scale.
 */
static int mcp47feb02_set_scale(struct mcp47feb02_data *data, int ch, int scale)
{
	int tmp_val, ret;

	ret = mcp47feb02_ch_scale(data, ch, scale);
	if (ret)
		return ret;

	if (scale == MCP47FEB02_SCALE_GAIN_BIT_X2)
		tmp_val = MCP47FEB02_GAIN_BIT_X2;
	else
		tmp_val = MCP47FEB02_GAIN_BIT_X1;

	ret = regmap_update_bits(data->regmap, MCP47FEB02_GAIN_BIT_STATUS_REG_ADDR,
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

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		ret = regmap_read(data->regmap, ch->address << 3, val);
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
	int *tmp_scale;
	int ret;

	guard(mutex)(&data->lock);

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		ret = regmap_write(data->regmap, ch->address << 3, val);
		if (ret)
			return ret;

		data->chdata[ch->address].dac_data = val;
		return 0;
	case IIO_CHAN_INFO_SCALE:
		if (data->phys_channels >= 4 && (ch->address % 2))
			tmp_scale = data->scale_1;
		else
			tmp_scale = data->scale;

		ret = mcp47feb02_check_scale(data, val, val2, tmp_scale);
		if (ret < 0)
			return ret;

		return mcp47feb02_set_scale(data, ch->address, ret);
	default:
		return -EINVAL;
	}
}

static int mcp47feb02_read_label(struct iio_dev *indio_dev,
				 struct iio_chan_spec const *ch, char *label)
{
	struct mcp47feb02_data *data = iio_priv(indio_dev);

	return sysfs_emit(label, "%s\n", data->labels[ch->address]);

	return 0;
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
	.attrs = &mcp47feb02_attribute_group,
};

static int mcp47feb02_parse_fw(struct iio_dev *indio_dev, const struct mcp47feb02_features *info)
{
	struct iio_chan_spec chanspec = mcp47febxx_ch_template;
	struct mcp47feb02_data *data = iio_priv(indio_dev);
	struct device *dev = &data->client->dev;
	struct iio_chan_spec *channels;
	u32 num_channels;
	int chan_idx = 0;
	u32 reg = 0;
	int ret;

	num_channels = device_get_child_node_count(dev);
	if (num_channels > info->phys_channels)
		return dev_err_probe(dev, -EINVAL, "More channels than the chip supports\n");

	if (!num_channels)
		return dev_err_probe(dev, -EINVAL, "No channel specified in the devicetree.\n");

	channels = devm_kcalloc(dev, num_channels, sizeof(*channels), GFP_KERNEL);
	if (!channels)
		return -ENOMEM;

	device_for_each_child_node_scoped(dev, child) {
		ret = fwnode_property_read_u32(child, "reg", &reg);
		if (ret)
			return dev_err_probe(dev, ret, "Invalid channel number\n");

		if (reg >= info->phys_channels)
			return dev_err_probe(dev, -EINVAL,
					     "The index of the channels does not match the chip\n");

		set_bit(reg, &data->active_channels_mask);

		if (fwnode_property_present(child, "label"))
			fwnode_property_read_string(child, "label", &data->labels[reg]);

		chanspec.address = reg;
		chanspec.channel = reg;
		channels[chan_idx] = chanspec;
		chan_idx++;
	}

	indio_dev->num_channels = num_channels;
	indio_dev->channels = channels;
	indio_dev->modes = INDIO_DIRECT_MODE;
	data->phys_channels = info->phys_channels;

	/*
	 * Check if microchip,vref-buffered and microchip,vref1-buffered are defined
	 * in the devicetree
	 */
	data->vref_buffered = device_property_read_bool(dev, "microchip,vref-buffered");

	if (info->have_ext_vref1)
		data->vref1_buffered = device_property_read_bool(dev, "microchip,vref1-buffered");

	return 0;
}

static int mcp47feb02_init_ctrl_regs(struct mcp47feb02_data *data)
{
	int ret, i, vref_ch, gain_ch, pd_ch, pd_tmp;
	struct device *dev = &data->client->dev;

	ret = regmap_read(data->regmap, MCP47FEB02_VREF_REG_ADDR, &vref_ch);
	if (ret)
		return ret;

	ret = regmap_read(data->regmap, MCP47FEB02_GAIN_BIT_STATUS_REG_ADDR, &gain_ch);
	if (ret)
		return ret;

	ret = regmap_read(data->regmap, MCP47FEB02_POWER_DOWN_REG_ADDR, &pd_ch);
	if (ret)
		return ret;

	gain_ch = gain_ch >> 8;
	for_each_set_bit(i, &data->active_channels_mask, data->phys_channels) {
		data->chdata[i].ref_mode = (vref_ch >> (2 * i)) & SET_DAC_CTRL_MASK;
		data->chdata[i].use_2x_gain = (gain_ch >> i)  & SET_GAIN_BIT;

		/*
		 * Inform the user that the current voltage reference read from volatile
		 * register of the chip is different from the one from device tree.
		 * You can't have an external voltage reference connected to the pin and
		 * select the internal BandGap, because the VREF pin is either an input or
		 * an output. When the DAC’s voltage reference is configured as the VREF pin,
		 * the pin is an input. When the DAC’s voltage reference is configured as the
		 * internal band gap, the pin is an output.
		 */
		if (data->chdata[i].ref_mode == MCP47FEB02_INTERNAL_BAND_GAP) {
			if (data->phys_channels >= 4 && (i % 2)) {
				if (data->use_vref1)
					dev_info(dev, "cannot use Vref1 and internal BandGap");
			} else {
				if (data->use_vref)
					dev_info(dev, "cannot use Vref and internal BandGap");
			}
		}

		pd_tmp = (pd_ch >> (2 * i)) & SET_DAC_CTRL_MASK;
		data->chdata[i].powerdown_mode = pd_tmp ? (pd_tmp - 1) : pd_tmp;
		data->chdata[i].powerdown = !!(data->chdata[i].powerdown_mode);
	}

	return 0;
}

static int mcp47feb02_init_ch_scales(struct mcp47feb02_data *data, int vdd_mv,
				     int vref_mv, int vref1_mv)
{
	struct device *dev = &data->client->dev;
	int i, ret;

	for_each_set_bit(i, &data->active_channels_mask, data->phys_channels) {
		ret = mcp47feb02_init_scales_avail(data, vdd_mv, vref_mv, vref1_mv);
		if (ret)
			return dev_err_probe(dev, ret, "failed to init scales for ch i %d\n", i);
	}

	return 0;
}

static int mcp47feb02_probe(struct i2c_client *client)
{
	const struct i2c_device_id *id = i2c_client_get_device_id(client);
	const struct mcp47feb02_features *info;
	struct device *dev = &client->dev;
	struct mcp47feb02_data *data;
	struct iio_dev *indio_dev;
	int vref1_mv = 0;
	int vref_mv = 0;
	int vdd_mv = 0;
	int ret;

	indio_dev = devm_iio_device_alloc(dev, sizeof(*data));
	if (!indio_dev)
		return -ENOMEM;

	data = iio_priv(indio_dev);
	data->client = client;
	info = i2c_get_match_data(client);
	if (!info)
		return -EINVAL;

	data->info = info;

	if (info->have_eeprom) {
		data->regmap = devm_regmap_init_i2c(client, &mcp47feb02_regmap_config);
		indio_dev->info = &mcp47feb02_info;
	} else {
		data->regmap = devm_regmap_init_i2c(client, &mcp47fvb02_regmap_config);
		indio_dev->info = &mcp47fvb02_info;
	}

	if (IS_ERR(data->regmap))
		return dev_err_probe(dev, PTR_ERR(data->regmap), "Error initializing i2c regmap\n");

	indio_dev->name = id->name;

	ret = mcp47feb02_parse_fw(indio_dev, info);
	if (ret)
		return dev_err_probe(dev, ret, "Error parsing devicetree data\n");

	ret = devm_mutex_init(dev, &data->lock);
	if (ret < 0)
		return ret;

	ret = devm_regulator_get_enable_read_voltage(dev, "vdd");
	if (ret < 0)
		return ret;

	vdd_mv = ret / 1000;

	ret = devm_regulator_get_enable_read_voltage(dev, "vref");
	if (ret > 0) {
		vref_mv = ret / 1000;
		data->use_vref = true;
	} else {
		dev_info(dev, "Vref is unavailable, internal band gap can be used instead\n");
	}

	if (info->have_ext_vref1) {
		ret = devm_regulator_get_enable_read_voltage(dev, "vref1");
		if (ret > 0) {
			vref1_mv = ret / 1000;
			data->use_vref1 = true;
		} else {
			dev_info(dev,
				 "Vref1 is unavailable, internal band gap can be used instead\n");
		}
	}

	ret = mcp47feb02_init_ctrl_regs(data);
	if (ret)
		return dev_err_probe(dev, ret, "Error initialising vref register\n");

	ret = mcp47feb02_init_ch_scales(data, vdd_mv, vref_mv, vref1_mv);
	if (ret)
		return ret;

	return devm_iio_device_register(dev, indio_dev);
}

static const struct i2c_device_id mcp47feb02_id[] = {
	{ "mcp47feb01", (kernel_ulong_t)&mcp47feb01_chip_info },
	{ "mcp47feb11", (kernel_ulong_t)&mcp47feb11_chip_info },
	{ "mcp47feb21", (kernel_ulong_t)&mcp47feb21_chip_info },
	{ "mcp47feb02", (kernel_ulong_t)&mcp47feb02_chip_info },
	{ "mcp47feb12", (kernel_ulong_t)&mcp47feb12_chip_info },
	{ "mcp47feb22", (kernel_ulong_t)&mcp47feb22_chip_info },
	{ "mcp47feb04", (kernel_ulong_t)&mcp47feb04_chip_info },
	{ "mcp47feb14", (kernel_ulong_t)&mcp47feb14_chip_info },
	{ "mcp47feb24", (kernel_ulong_t)&mcp47feb24_chip_info },
	{ "mcp47feb08", (kernel_ulong_t)&mcp47feb08_chip_info },
	{ "mcp47feb18", (kernel_ulong_t)&mcp47feb18_chip_info },
	{ "mcp47feb28", (kernel_ulong_t)&mcp47feb28_chip_info },
	{ "mcp47fvb01", (kernel_ulong_t)&mcp47fvb01_chip_info },
	{ "mcp47fvb11", (kernel_ulong_t)&mcp47fvb11_chip_info },
	{ "mcp47fvb21", (kernel_ulong_t)&mcp47fvb21_chip_info },
	{ "mcp47fvb02", (kernel_ulong_t)&mcp47fvb02_chip_info },
	{ "mcp47fvb12", (kernel_ulong_t)&mcp47fvb12_chip_info },
	{ "mcp47fvb22", (kernel_ulong_t)&mcp47fvb22_chip_info },
	{ "mcp47fvb04", (kernel_ulong_t)&mcp47fvb04_chip_info },
	{ "mcp47fvb14", (kernel_ulong_t)&mcp47fvb14_chip_info },
	{ "mcp47fvb24", (kernel_ulong_t)&mcp47fvb24_chip_info },
	{ "mcp47fvb08", (kernel_ulong_t)&mcp47fvb08_chip_info },
	{ "mcp47fvb18", (kernel_ulong_t)&mcp47fvb18_chip_info },
	{ "mcp47fvb28", (kernel_ulong_t)&mcp47fvb28_chip_info },
	{ }
};
MODULE_DEVICE_TABLE(i2c, mcp47feb02_id);

static const struct of_device_id mcp47feb02_of_match[] = {
	{ .compatible = "microchip,mcp47feb01", .data = &mcp47feb01_chip_info },
	{ .compatible = "microchip,mcp47feb11", .data = &mcp47feb11_chip_info },
	{ .compatible = "microchip,mcp47feb21", .data = &mcp47feb21_chip_info },
	{ .compatible = "microchip,mcp47feb02", .data = &mcp47feb02_chip_info },
	{ .compatible = "microchip,mcp47feb12", .data = &mcp47feb12_chip_info },
	{ .compatible = "microchip,mcp47feb22", .data = &mcp47feb22_chip_info },
	{ .compatible = "microchip,mcp47feb04", .data = &mcp47feb04_chip_info },
	{ .compatible = "microchip,mcp47feb14", .data = &mcp47feb14_chip_info },
	{ .compatible = "microchip,mcp47feb24", .data = &mcp47feb24_chip_info },
	{ .compatible = "microchip,mcp47feb08", .data = &mcp47feb08_chip_info },
	{ .compatible = "microchip,mcp47feb18", .data = &mcp47feb18_chip_info },
	{ .compatible = "microchip,mcp47feb28", .data = &mcp47feb28_chip_info },
	{ .compatible = "microchip,mcp47fvb01", .data = &mcp47fvb01_chip_info },
	{ .compatible = "microchip,mcp47fvb11", .data = &mcp47fvb11_chip_info },
	{ .compatible = "microchip,mcp47fvb21", .data = &mcp47fvb21_chip_info },
	{ .compatible = "microchip,mcp47fvb02", .data = &mcp47fvb02_chip_info },
	{ .compatible = "microchip,mcp47fvb12", .data = &mcp47fvb12_chip_info },
	{ .compatible = "microchip,mcp47fvb22", .data = &mcp47fvb22_chip_info },
	{ .compatible = "microchip,mcp47fvb04", .data = &mcp47fvb04_chip_info },
	{ .compatible = "microchip,mcp47fvb14",	.data = &mcp47fvb14_chip_info },
	{ .compatible = "microchip,mcp47fvb24", .data = &mcp47fvb24_chip_info },
	{ .compatible = "microchip,mcp47fvb08", .data = &mcp47fvb08_chip_info },
	{ .compatible = "microchip,mcp47fvb18", .data = &mcp47fvb18_chip_info },
	{ .compatible = "microchip,mcp47fvb28", .data = &mcp47fvb28_chip_info },
	{ }
};
MODULE_DEVICE_TABLE(of, mcp47feb02_of_match);

static struct i2c_driver mcp47feb02_driver = {
	.driver = {
		.name	= "mcp47feb02",
		.of_match_table = mcp47feb02_of_match,
		.pm	= pm_sleep_ptr(&mcp47feb02_pm_ops),
	},
	.probe		= mcp47feb02_probe,
	.id_table	= mcp47feb02_id,
};
module_i2c_driver(mcp47feb02_driver);

MODULE_AUTHOR("Ariana Lazar <ariana.lazar@microchip.com>");
MODULE_DESCRIPTION("IIO driver for MCP47FEB02 Multi-Channel DAC with I2C interface");
MODULE_LICENSE("GPL");
