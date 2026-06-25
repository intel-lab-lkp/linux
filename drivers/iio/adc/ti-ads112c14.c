// SPDX-License-Identifier: GPL-2.0-only
/*
 * IIO driver for Texas Instruments ADS112C14 and similar ADCs.
 *
 * Copyright (C) 2026 Texas Instruments Incorporated - https://www.ti.com/
 * Copyright (C) 2026 Baylibre Inc.
 *
 * Datasheet: https://www.ti.com/lit/ds/symlink/ads122c14.pdf
 */

#include <linux/bitfield.h>
#include <linux/cleanup.h>
#include <linux/delay.h>
#include <linux/dev_printk.h>
#include <linux/device/devres.h>
#include <linux/i2c.h>
#include <linux/iio/iio.h>
#include <linux/math64.h>
#include <linux/minmax.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/property.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/sysfs.h>
#include <linux/time64.h>
#include <linux/types.h>
#include <linux/unaligned.h>
#include <linux/units.h>

/* Datasheet t_d(RST) - time to wait after reset before next I2C use. */
#define ADS112C14_DELAY_RESET_US 500

#define ADS112C14_CMD_RDATA	0x00
#define ADS112C14_CMD_RREG	0x40
#define ADS112C14_CMD_WREG	0x80

#define ADS112C14_REG_DEVICE_ID				0x00
#define   ADS112C14_DEVICE_ID_BITS			GENMASK(3, 0)

#define ADS112C14_REG_REVISION_ID			0x01

#define ADS112C14_REG_STATUS_MSB			0x02
#define   ADS112C14_STATUS_MSB_RESETN			BIT(7)
#define   ADS112C14_STATUS_MSB_AVDD_UVN			BIT(6)
#define   ADS112C14_STATUS_MSB_REF_UVN			BIT(5)
#define   ADS112C14_STATUS_MSB_REG_MAP_CRC_FAULTN	BIT(3)
#define   ADS112C14_STATUS_MSB_MEM_FAULTN		BIT(2)
#define   ADS112C14_STATUS_MSB_REG_WRITE_FAULTN		BIT(1)
#define   ADS112C14_STATUS_MSB_DRDY			BIT(0)

#define ADS112C14_REG_STATUS_LSB			0x03
#define   ADS112C14_STATUS_LSB_CONV_COUNT		GENMASK(7, 4)
#define   ADS112C14_STATUS_LSB_GPIO3_DAT_IN		BIT(3)
#define   ADS112C14_STATUS_LSB_GPIO2_DAT_IN		BIT(2)
#define   ADS112C14_STATUS_LSB_GPIO1_DAT_IN		BIT(1)
#define   ADS112C14_STATUS_LSB_GPIO0_DAT_IN		BIT(0)

#define ADS112C14_REG_CONVERSION_CTRL			0x04
#define   ADS112C14_CONVERSION_CTRL_RESET		GENMASK(7, 2)
#define   ADS112C14_CONVERSION_CTRL_START		BIT(1)
#define   ADS112C14_CONVERSION_CTRL_STOP		BIT(0)

#define ADS112C14_REG_DEVICE_CFG			0x05
#define   ADS112C14_DEVICE_CFG_PWDN			BIT(7)
#define   ADS112C14_DEVICE_CFG_STBY_MODE		BIT(6)
#define   ADS112C14_DEVICE_CFG_BOCS			GENMASK(5, 4)
#define     ADS112C14_DEVICE_CFG_BOCS_DISABLED		  0
#define     ADS112C14_DEVICE_CFG_BOCS_200_nA		  1
#define     ADS112C14_DEVICE_CFG_BOCS_1_uA		  2
#define     ADS112C14_DEVICE_CFG_BOCS_10_uA		  3
#define   ADS112C14_DEVICE_CFG_CLK_SEL			BIT(3)
#define   ADS112C14_DEVICE_CFG_CONV_MODE		BIT(2)
#define   ADS112C14_DEVICE_CFG_SPEED_MODE		GENMASK(1, 0)

#define ADS112C14_REG_DATA_RATE_CFG			0x06
#define   ADS112C14_DATA_RATE_CFG_DELAY			GENMASK(7, 4)
#define   ADS112C14_DATA_RATE_CFG_GC_EN			BIT(3)
#define   ADS112C14_DATA_RATE_CFG_FLTR_OSR		GENMASK(2, 0)

#define ADS112C14_REG_MUX_CFG				0x07
#define   ADS112C14_MUX_CFG_AINP			GENMASK(7, 4)
#define   ADS112C14_MUX_CFG_AINN			GENMASK(3, 0)
#define     ADS112C14_MUX_CFG_AIN_GND			  0x8

#define ADS112C14_REG_GAIN_CFG				0x08
#define   ADS112C14_GAIN_CFG_SPARE			BIT(7)
#define   ADS112C14_GAIN_CFG_SYS_MON			GENMASK(6, 4)
#define   ADS112C14_GAIN_CFG_GAIN			GENMASK(3, 0)

#define ADS112C14_REG_REFERENCE_CFG			0x09
#define   ADS112C14_REFERENCE_CFG_REF_UV_EN		BIT(7)
#define   ADS112C14_REFERENCE_CFG_REFP_BUF_EN		BIT(5)
#define   ADS112C14_REFERENCE_CFG_REFN_BUF_EN		BIT(4)
#define   ADS112C14_REFERENCE_CFG_REF_VAL		BIT(2)
#define     ADS112C14_REFERENCE_CFG_REF_VAL_1_25V	  0
#define     ADS112C14_REFERENCE_CFG_REF_VAL_2_5V	  1
#define   ADS112C14_REFERENCE_CFG_REF_SEL		GENMASK(1, 0)
#define     ADS112C14_REFERENCE_CFG_REF_SEL_INTERNAL	  0
#define     ADS112C14_REFERENCE_CFG_REF_SEL_EXTERNAL	  1
#define     ADS112C14_REFERENCE_CFG_REF_SEL_AVDD	  2

#define ADS112C14_REG_DIGITAL_CFG			0x0A
#define   ADS112C14_DIGITAL_CFG_REG_MAP_CRC_EN		BIT(6)
#define   ADS112C14_DIGITAL_CFG_I2C_CRC_EN		BIT(5)
#define   ADS112C14_DIGITAL_CFG_STATUS_EN		BIT(4)
#define   ADS112C14_DIGITAL_CFG_FAULT_PIN_BEHAVIOR	BIT(3)
#define   ADS112C14_DIGITAL_CFG_CODING			BIT(1)

#define ADS112C14_REG_GPIO_CFG				0x0B
#define   ADS112C14_GPIO_CFG_GPIO3_CFG			GENMASK(7, 6)
#define   ADS112C14_GPIO_CFG_GPIO2_CFG			GENMASK(5, 4)
#define   ADS112C14_GPIO_CFG_GPIO1_CFG			GENMASK(3, 2)
#define   ADS112C14_GPIO_CFG_GPIO0_CFG			GENMASK(1, 0)

#define ADS112C14_REG_GPIO_DATA_OUTPUT			0x0C
#define   ADS112C14_GPIO_DATA_OUTPUT_GPIO3_SRC		BIT(7)
#define   ADS112C14_GPIO_DATA_OUTPUT_GPIO2_SRC		BIT(6)
#define   ADS112C14_GPIO_DATA_OUTPUT_GPIO3_DAT_OUT	BIT(3)
#define   ADS112C14_GPIO_DATA_OUTPUT_GPIO2_DAT_OUT	BIT(2)
#define   ADS112C14_GPIO_DATA_OUTPUT_GPIO1_DAT_OUT	BIT(1)
#define   ADS112C14_GPIO_DATA_OUTPUT_GPIO0_DAT_OUT	BIT(0)

#define ADS112C14_REG_IDAC_MAG_CFG			0x0D
#define   ADS112C14_IDAC_MAG_CFG_I2MAG			GENMASK(7, 4)
#define   ADS112C14_IDAC_MAG_CFG_I1MAG			GENMASK(3, 0)

#define ADS112C14_REG_IDAC_MUX_CFG			0x0E
#define   ADS112C14_IDAC_MUX_CFG_IUNIT			BIT(7)
#define   ADS112C14_IDAC_MUX_CFG_I2MUX			GENMASK(6, 4)
#define   ADS112C14_IDAC_MUX_CFG_I1MUX			GENMASK(2, 0)

#define ADS112C14_REG_REG_MAP_CRC			0x0F

#define ADS112C14_INT_REF0_mV				1250
#define ADS112C14_INT_REF1_mV				2500

enum {
	ADS112C14_VREF_SOURCE_INTERNAL_2_5V,
	ADS112C14_VREF_SOURCE_INTERNAL_1_25V,
	ADS112C14_VREF_SOURCE_EXTERNAL,
	ADS112C14_VREF_SOURCE_AVDD,
};

static const char * const ads112c14_vref_source_names[] = {
	[ADS112C14_VREF_SOURCE_INTERNAL_2_5V] = "internal-2.5v",
	[ADS112C14_VREF_SOURCE_INTERNAL_1_25V] = "internal-1.25v",
	[ADS112C14_VREF_SOURCE_EXTERNAL] = "external",
	[ADS112C14_VREF_SOURCE_AVDD] = "avdd",
};

/* Available gains as tenths (x10) */
static const u32 ads112c14_pga_gains_x10[] = {
	5, /* 0.5 */
	10, /* 1 */
	20, /* 2 */
	40, /* 4 */
	50, /* 5 */
	80, /* 8 */
	100, /* 10 */
	160, /* 16 */
	200, /* 20 */
	320, /* 32 */
	500, /* 50 */
	640, /* 64 */
	1000, /* 100 */
	1280, /* 128 */
	2000, /* 200 */
	2560, /* 256 */
};

static bool ads112c14_writeable_reg(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case ADS112C14_REG_DEVICE_ID:
	case ADS112C14_REG_REVISION_ID:
	case ADS112C14_REG_STATUS_LSB:
		return false;
	default:
		return true;
	}
}

static bool ads112c14_volatile_reg(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case ADS112C14_REG_STATUS_MSB:
	case ADS112C14_REG_STATUS_LSB:
	case ADS112C14_REG_CONVERSION_CTRL:
		return true;
	default:
		return false;
	}
}

static const struct reg_default ads112c14_reg_defaults[] = {
	{ ADS112C14_REG_DEVICE_CFG, 0 },
	{ ADS112C14_REG_DATA_RATE_CFG, 0 },
	{ ADS112C14_REG_MUX_CFG, 0 },
	{ ADS112C14_REG_GAIN_CFG, FIELD_PREP_CONST(ADS112C14_GAIN_CFG_GAIN, 1) },
	{ ADS112C14_REG_REFERENCE_CFG, 0 },
	{ ADS112C14_REG_DIGITAL_CFG, 0 },
	{ ADS112C14_REG_GPIO_CFG, 0 },
	{ ADS112C14_REG_GPIO_DATA_OUTPUT, 0 },
	{ ADS112C14_REG_IDAC_MAG_CFG, 0 },
	{ ADS112C14_REG_IDAC_MUX_CFG, FIELD_PREP_CONST(ADS112C14_IDAC_MUX_CFG_I2MUX, 1) },
};

static const struct regmap_config ads112c14_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.read_flag_mask = ADS112C14_CMD_RREG,
	.write_flag_mask = ADS112C14_CMD_WREG,
	.max_register = ADS112C14_REG_REG_MAP_CRC,
	.writeable_reg = ads112c14_writeable_reg,
	.volatile_reg = ads112c14_volatile_reg,
	.reg_defaults = ads112c14_reg_defaults,
	.num_reg_defaults = ARRAY_SIZE(ads112c14_reg_defaults),
	.cache_type = REGCACHE_MAPLE,
};

struct ads112c14_chip_info {
	const char *name;
	u32 resolution_bits;
};

struct ads112c14_measurement {
	const char *label;
	u32 vref_source;
	u8 iunit;
	u8 idac1_mag;
	u8 idac2_mag;
	u8 idac1_mux;
	u8 idac2_mux;
	u8 iadc_count;
	u8 gain_val;
	u8 burnout;
	bool global_chop;
	bool bipolar;
	int scale_available[ARRAY_SIZE(ads112c14_pga_gains_x10)][2];
};

struct ads112c14_data {
	const struct ads112c14_chip_info *chip_info;
	struct regmap *regmap;
	u32 avdd_uV;
	u32 ext_ref_uV;
	bool refp_is_avdd;
	bool refn_is_gnd;
	u32 ext_ref_ohms;
	struct ads112c14_measurement *measurements;
	u32 num_measurements;
	u8 sys_mon_chan_short_gain_val;
	int sys_mon_chan_short_scale_available[ARRAY_SIZE(ads112c14_pga_gains_x10)][2];
};

/* Fixed channels for system monitor measurements. */
#define ADS112C14_SYS_MON_CHANNEL_TEMP		100
#define ADS112C14_SYS_MON_CHANNEL_EXT_REF	101
#define ADS112C14_SYS_MON_CHANNEL_AVDD		102
#define ADS112C14_SYS_MON_CHANNEL_DVDD		103
#define ADS112C14_SYS_MON_CHANNEL_SHORT		104

static const struct iio_chan_spec ads112c14_sys_mon_channels[] = {
	{
		.type = IIO_TEMP,
		.indexed = 1,
		.channel = ADS112C14_SYS_MON_CHANNEL_TEMP,
		.address = 2,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW)
				    | BIT(IIO_CHAN_INFO_SCALE)
				    | BIT(IIO_CHAN_INFO_OFFSET),
	},
	{
		.type = IIO_VOLTAGE,
		.indexed = 1,
		.channel = ADS112C14_SYS_MON_CHANNEL_EXT_REF,
		.address = 3,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW)
				    | BIT(IIO_CHAN_INFO_SCALE),
	},
	{
		.type = IIO_VOLTAGE,
		.indexed = 1,
		.channel = ADS112C14_SYS_MON_CHANNEL_AVDD,
		.address = 4,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW)
				    | BIT(IIO_CHAN_INFO_SCALE),
	},
	{
		.type = IIO_VOLTAGE,
		.indexed = 1,
		.channel = ADS112C14_SYS_MON_CHANNEL_DVDD,
		.address = 5,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW)
				    | BIT(IIO_CHAN_INFO_SCALE),
	},
	{
		.type = IIO_VOLTAGE,
		.indexed = 1,
		.channel = ADS112C14_SYS_MON_CHANNEL_SHORT,
		.channel2 = ADS112C14_SYS_MON_CHANNEL_SHORT,
		.differential = 1,
		.address = 1,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW)
				    | BIT(IIO_CHAN_INFO_SCALE),
		.info_mask_separate_available = BIT(IIO_CHAN_INFO_SCALE),
	},
};

static int ads112c14_prepare_measurement_channel(struct ads112c14_data *data,
						 const struct iio_chan_spec *chan)
{
	struct ads112c14_measurement *measurement = &data->measurements[chan->scan_index];
	u32 refp_buf_en, refn_buf_en, ref_val, ref_sel;
	int ret;

	ret = regmap_write(data->regmap, ADS112C14_REG_MUX_CFG,
			   FIELD_PREP(ADS112C14_MUX_CFG_AINP, chan->channel) |
			   FIELD_PREP(ADS112C14_MUX_CFG_AINN, chan->channel2));
	if (ret)
		return ret;

	ret = regmap_update_bits(data->regmap, ADS112C14_REG_DIGITAL_CFG,
				 ADS112C14_DIGITAL_CFG_CODING,
				 FIELD_PREP(ADS112C14_DIGITAL_CFG_CODING,
					    measurement->bipolar ? 0 : 1));
	if (ret)
		return ret;

	ret = regmap_update_bits(data->regmap, ADS112C14_REG_GAIN_CFG,
				 ADS112C14_GAIN_CFG_SYS_MON | ADS112C14_GAIN_CFG_GAIN,
				 FIELD_PREP(ADS112C14_GAIN_CFG_SYS_MON, 0) |
				 FIELD_PREP(ADS112C14_GAIN_CFG_GAIN,
					    measurement->gain_val));
	if (ret)
		return ret;

	ret = regmap_write(data->regmap, ADS112C14_REG_IDAC_MAG_CFG,
			   FIELD_PREP(ADS112C14_IDAC_MAG_CFG_I2MAG,
				      measurement->idac2_mag) |
			   FIELD_PREP(ADS112C14_IDAC_MAG_CFG_I1MAG,
				      measurement->idac1_mag));
	if (ret)
		return ret;

	ret = regmap_write(data->regmap, ADS112C14_REG_IDAC_MUX_CFG,
			   FIELD_PREP(ADS112C14_IDAC_MUX_CFG_IUNIT,
				      measurement->iunit) |
			   FIELD_PREP(ADS112C14_IDAC_MUX_CFG_I2MUX,
				      measurement->idac2_mux) |
			   FIELD_PREP(ADS112C14_IDAC_MUX_CFG_I1MUX,
				      measurement->idac1_mux));
	if (ret)
		return ret;

	ret = regmap_update_bits(data->regmap, ADS112C14_REG_DATA_RATE_CFG,
				 ADS112C14_DATA_RATE_CFG_GC_EN,
				 FIELD_PREP(ADS112C14_DATA_RATE_CFG_GC_EN,
					    measurement->global_chop));
	if (ret)
		return ret;

	refp_buf_en = !data->refp_is_avdd &&
		      measurement->vref_source == ADS112C14_VREF_SOURCE_EXTERNAL;
	refn_buf_en = !data->refn_is_gnd &&
		      measurement->vref_source == ADS112C14_VREF_SOURCE_EXTERNAL;

	ref_val = measurement->vref_source == ADS112C14_VREF_SOURCE_INTERNAL_2_5V ?
		ADS112C14_REFERENCE_CFG_REF_VAL_2_5V :
		ADS112C14_REFERENCE_CFG_REF_VAL_1_25V;

	switch (measurement->vref_source) {
	case ADS112C14_VREF_SOURCE_AVDD:
		ref_sel = ADS112C14_REFERENCE_CFG_REF_SEL_AVDD;
		break;
	case ADS112C14_VREF_SOURCE_EXTERNAL:
		ref_sel = ADS112C14_REFERENCE_CFG_REF_SEL_EXTERNAL;
		break;
	default:
		ref_sel = ADS112C14_REFERENCE_CFG_REF_SEL_INTERNAL;
		break;
	}

	return regmap_update_bits(data->regmap, ADS112C14_REG_REFERENCE_CFG,
				  ADS112C14_REFERENCE_CFG_REFP_BUF_EN |
				  ADS112C14_REFERENCE_CFG_REFN_BUF_EN |
				  ADS112C14_REFERENCE_CFG_REF_VAL |
				  ADS112C14_REFERENCE_CFG_REF_SEL,
				  FIELD_PREP(ADS112C14_REFERENCE_CFG_REFP_BUF_EN,
					     refp_buf_en) |
				  FIELD_PREP(ADS112C14_REFERENCE_CFG_REFN_BUF_EN,
					     refn_buf_en) |
				  FIELD_PREP(ADS112C14_REFERENCE_CFG_REF_VAL,
					     ref_val) |
				  FIELD_PREP(ADS112C14_REFERENCE_CFG_REF_SEL,
					     ref_sel));
}

static int ads112c14_prepare_sys_mon_channel(struct ads112c14_data *data,
					     const struct iio_chan_spec *chan)
{
	u32 gain_val;
	int ret;

	/*
	 * NB: IDAC registers are left as-is in case they are generating current
	 * needed for the external reference measurement.
	 */

	/*
	 * All SYS_MON channels use GAIN of 1 to keep it simple. Other than
	 * the internal short channel, where it is useful in practice.
	 */
	gain_val = chan->channel == ADS112C14_SYS_MON_CHANNEL_SHORT ?
		   data->sys_mon_chan_short_gain_val : 1;

	ret = regmap_update_bits(data->regmap, ADS112C14_REG_GAIN_CFG,
				 ADS112C14_GAIN_CFG_SYS_MON |
				 ADS112C14_GAIN_CFG_GAIN,
				 FIELD_PREP(ADS112C14_GAIN_CFG_SYS_MON, chan->address) |
				 FIELD_PREP(ADS112C14_GAIN_CFG_GAIN, gain_val));
	if (ret)
		return ret;

	/* All SYS_MON channels use signed data to keep it simple. */
	ret = regmap_clear_bits(data->regmap, ADS112C14_REG_DIGITAL_CFG,
				ADS112C14_DIGITAL_CFG_CODING);
	if (ret)
		return ret;

	/*
	 * REVISIT: if we implement regulator support for the REFOUT pin, we
	 * might need to make this voltage match what is required by that. In
	 * that case, we could also adjust GAIN so that we still get the same
	 * range.
	 */
	/*
	 * NB: SYS_MON channels ignore REF_SEL except for the shorted input
	 * channel, so we set it here to internal reference to be consistent.
	 * If we ever need to make a measurement of shorted input with other
	 * reference source, we could add additional channels for that.
	 */
	ret = regmap_update_bits(data->regmap, ADS112C14_REG_REFERENCE_CFG,
				 ADS112C14_REFERENCE_CFG_REF_VAL |
				 ADS112C14_REFERENCE_CFG_REF_SEL,
				 FIELD_PREP(ADS112C14_REFERENCE_CFG_REF_VAL,
					    ADS112C14_REFERENCE_CFG_REF_VAL_2_5V) |
				 FIELD_PREP(ADS112C14_REFERENCE_CFG_REF_SEL,
					    ADS112C14_REFERENCE_CFG_REF_SEL_INTERNAL));
	if (ret)
		return ret;

	return 0;
}

static int ads112c14_single_conversion(struct ads112c14_data *data,
				       const struct iio_chan_spec *chan,
				       u8 *buf)
{
	struct i2c_client *client = to_i2c_client(regmap_get_device(data->regmap));
	u32 reg_val;
	int ret;

	if (chan->channel < 100) {
		ret = ads112c14_prepare_measurement_channel(data, chan);
		if (ret)
			return ret;
	} else {
		ret = ads112c14_prepare_sys_mon_channel(data, chan);
		if (ret)
			return ret;
	}

	ret = regmap_write(data->regmap, ADS112C14_REG_CONVERSION_CTRL,
			   ADS112C14_CONVERSION_CTRL_START);
	if (ret)
		return ret;

	ret = regmap_read_poll_timeout(data->regmap,
				       ADS112C14_REG_STATUS_MSB, reg_val,
				       FIELD_GET(ADS112C14_STATUS_MSB_DRDY, reg_val),
				       1 * USEC_PER_MSEC, 100 * USEC_PER_MSEC);
	if (ret)
		return ret;

	return i2c_smbus_read_i2c_block_data(client, ADS112C14_CMD_RDATA,
					     BITS_TO_BYTES(data->chip_info->resolution_bits),
					     buf);
}

static int ads112c14_read_raw(struct iio_dev *indio_dev,
			      struct iio_chan_spec const *chan, int *val,
			      int *val2, long mask)
{
	struct ads112c14_data *data = iio_priv(indio_dev);
	struct ads112c14_measurement *measurement = NULL;
	u32 vref_uV, fsr_bits;
	int *scale_avail;

	/* Selecting V_REF source is not implemented yet. */
	vref_uV = ADS112C14_INT_REF1_mV * (MICRO / MILLI);

	if (chan->channel < 100) {
		measurement = &data->measurements[chan->scan_index];
		fsr_bits = data->chip_info->resolution_bits - measurement->bipolar;
	} else {
		/* All SYS_MON channels are using signed coding. */
		fsr_bits = data->chip_info->resolution_bits - 1;
	}

	switch (mask) {
	case IIO_CHAN_INFO_RAW: {
		u8 buf[3];
		int ret;

		if (!iio_device_claim_direct(indio_dev))
			return -EBUSY;

		ret = ads112c14_single_conversion(data, chan, buf);
		iio_device_release_direct(indio_dev);
		if (ret < 0)
			return ret;

		switch (data->chip_info->resolution_bits) {
		case 16:
			*val = get_unaligned_be16(buf);
			break;
		case 24:
			*val = get_unaligned_be24(buf);
			break;
		default:
			return -EINVAL;
		}

		if (!measurement || measurement->bipolar)
			*val = sign_extend32(*val, fsr_bits);

		return IIO_VAL_INT;
	}
	case IIO_CHAN_INFO_SCALE:
		if (chan->type == IIO_TEMP) {
			/* TS_TC (typical) = 405 uV/°C */
			*val = MILLI * vref_uV / 405;
			*val2 = fsr_bits;
			return IIO_VAL_FRACTIONAL_LOG2;
		}

		if (chan->channel < 100) {
			scale_avail = measurement->scale_available[measurement->gain_val];
			*val = scale_avail[0];
			*val2 = scale_avail[1];

			return IIO_VAL_INT_PLUS_NANO;
		}

		if (chan->channel == ADS112C14_SYS_MON_CHANNEL_SHORT) {
			u8 idx = data->sys_mon_chan_short_gain_val;

			scale_avail = data->sys_mon_chan_short_scale_available[idx];

			*val = scale_avail[0];
			*val2 = scale_avail[1];

			return IIO_VAL_INT_PLUS_NANO;
		}

		*val = vref_uV / (MICRO / MILLI);
		/*
		 * Last 3 SYS_MON channels (ext ref, AVDD, DVDD) need to be
		 * multiplied by 8 to account for internal attenuation of / 8.
		 */
		*val2 = fsr_bits - (chan->address >= 3 ? 3 : 0);
		return IIO_VAL_FRACTIONAL_LOG2;
	case IIO_CHAN_INFO_OFFSET:
		/* Only the temperature channel has an offset. */
		if (chan->type != IIO_TEMP)
			return -EINVAL;
		/*
		 * Die temperature [°C] = 25°C + (Measured voltage – TS_Offset) / TS_TC
		 * TS_TC (typical) = 405 uV/°C
		 * TS_Offset (typical) = 119.5 mV
		 */
		*val = div_s64((s64)(25 * 405 - 119500) * BIT(fsr_bits), vref_uV);
		return IIO_VAL_INT;
	default:
		return -EINVAL;
	}
}

static int ads112c14_read_avail(struct iio_dev *indio_dev,
				const struct iio_chan_spec *chan, const int **vals,
				int *type, int *length, long mask)
{
	struct ads112c14_data *data = iio_priv(indio_dev);

	if (chan->channel < 100) {
		struct ads112c14_measurement *measurement;

		measurement = &data->measurements[chan->scan_index];
		*vals = (const int *)measurement->scale_available;
		*length = 2 * ARRAY_SIZE(measurement->scale_available);
		*type = IIO_VAL_INT_PLUS_NANO;
		return IIO_AVAIL_LIST;
	}

	if (chan->channel == ADS112C14_SYS_MON_CHANNEL_SHORT) {
		*vals = (const int *)data->sys_mon_chan_short_scale_available;
		*length = 2 * ARRAY_SIZE(data->sys_mon_chan_short_scale_available);
		*type = IIO_VAL_INT_PLUS_NANO;
		return IIO_AVAIL_LIST;
	}

	return -EINVAL;
}

static int ads112c14_write_raw(struct iio_dev *indio_dev,
			       struct iio_chan_spec const *chan, int val,
			       int val2, long mask)
{
	struct ads112c14_data *data = iio_priv(indio_dev);
	const int (*scale_avail)[2];
	u8 *gain_val;

	switch (mask) {
	case IIO_CHAN_INFO_SCALE: {
		if (chan->channel < 100) {
			struct ads112c14_measurement *measurement;

			measurement = &data->measurements[chan->scan_index];
			scale_avail = measurement->scale_available;
			gain_val = &measurement->gain_val;
		} else if (chan->channel == ADS112C14_SYS_MON_CHANNEL_SHORT) {
			scale_avail = data->sys_mon_chan_short_scale_available;
			gain_val = &data->sys_mon_chan_short_gain_val;
		} else {
			return -EINVAL;
		}

		IIO_DEV_ACQUIRE_DIRECT_MODE(indio_dev, claim);
		if (IIO_DEV_ACQUIRE_FAILED(claim))
			return -EBUSY;

		for (u32 i = 0; i < ARRAY_SIZE(ads112c14_pga_gains_x10); i++) {
			if (val == scale_avail[i][0] && val2 == scale_avail[i][1]) {
				*gain_val = i;
				return 0;
			}
		}

		return -EINVAL;
	}
	default:
		return -EINVAL;
	}
}

static int ads112c14_write_raw_get_fmt(struct iio_dev *indio_dev,
				       struct iio_chan_spec const *chan,
				       long mask)
{
	switch (mask) {
	case IIO_CHAN_INFO_SCALE:
		return IIO_VAL_INT_PLUS_NANO;
	default:
		return IIO_VAL_INT_PLUS_MICRO;
	}
}

static int ads112c14_read_label(struct iio_dev *indio_dev,
				struct iio_chan_spec const *chan, char *label)
{
	struct ads112c14_data *data = iio_priv(indio_dev);
	const char *label_source;

	/* measurement channels */
	if (chan->channel < 100) {
		struct ads112c14_measurement *measurement;

		measurement = &data->measurements[chan->scan_index];

		if (!measurement->label)
			return -EINVAL;

		return sysfs_emit(label, "%s\n", measurement->label);
	}

	/* System monitor channels. */
	switch (chan->channel) {
	case ADS112C14_SYS_MON_CHANNEL_TEMP:
		label_source = "Internal temperature sensor";
		break;
	case ADS112C14_SYS_MON_CHANNEL_EXT_REF:
		label_source = "External reference";
		break;
	case ADS112C14_SYS_MON_CHANNEL_AVDD:
		label_source = "AVDD";
		break;
	case ADS112C14_SYS_MON_CHANNEL_DVDD:
		label_source = "DVDD";
		break;
	case ADS112C14_SYS_MON_CHANNEL_SHORT:
		label_source = "Internal short (internal reference source)";
		break;
	default:
		return -EINVAL;
	}

	return sysfs_emit(label, "%s\n", label_source);
}

static const struct iio_info ads112c14_info = {
	.read_raw = ads112c14_read_raw,
	.read_avail = ads112c14_read_avail,
	.write_raw = ads112c14_write_raw,
	.write_raw_get_fmt = ads112c14_write_raw_get_fmt,
	.read_label = ads112c14_read_label,
};

static int ads112c14_populate_idac_mag(u32 current_nA, u8 *idac_mag)
{
	u32 current_uA = current_nA / (NANO / MICRO);

	/* Convert microamps to IMAG bits */
	if (current_uA == 1)
		*idac_mag = 1;
	else if (in_range(current_uA, 10, 100) && current_uA % 10 == 0)
		*idac_mag = current_uA / 10 + 1;
	else
		return dev_err_probe(NULL, -EINVAL,
				     "invalid excitation-current-nanoamp value\n");

	return 0;
}

static int ads112c14_parse_channels(struct iio_dev *indio_dev,
				    bool *need_avdd_ref, bool *need_ext_ref)
{
	struct ads112c14_data *data = iio_priv(indio_dev);
	struct device *dev = indio_dev->dev.parent;
	struct iio_chan_spec *channels;
	u32 num_child_nodes, i, pair[2];
	int ret;

	*need_avdd_ref = false;
	*need_ext_ref = false;

	num_child_nodes = device_get_named_child_node_count(dev, "channel");

	data->measurements = devm_kcalloc(dev, num_child_nodes,
					  sizeof(*data->measurements), GFP_KERNEL);
	if (!data->measurements)
		return -ENOMEM;

	channels = devm_kcalloc(dev, num_child_nodes +
				ARRAY_SIZE(ads112c14_sys_mon_channels),
				sizeof(*channels), GFP_KERNEL);
	if (!channels)
		return -ENOMEM;

	i = 0;
	device_for_each_named_child_node_scoped(dev, child, "channel") {
		struct ads112c14_measurement *measurement = &data->measurements[i];
		struct iio_chan_spec *spec = &channels[i];

		if (!fwnode_device_is_available(child))
			continue;

		spec->indexed = 1;
		spec->scan_index = i;
		measurement->gain_val = 1;

		fwnode_property_read_string(child, "label", &measurement->label);

		if (fwnode_property_present(child, "single-channel")) {
			ret = fwnode_property_read_u32(child, "single-channel",
						       &pair[0]);
			if (ret)
				return dev_err_probe(dev, ret,
						     "failed to read single-channel property\n");

			if (pair[0] >= 8)
				return dev_err_probe(dev, -EINVAL,
						     "single-channel value must be between 0 and 7\n");

			spec->channel = pair[0];
			/* NB: channel2 is unused by iio core code in this case. */
			spec->channel2 = ADS112C14_MUX_CFG_AIN_GND;
		} else if (fwnode_property_present(child, "diff-channels")) {
			ret = fwnode_property_read_u32_array(child, "diff-channels",
							     pair, ARRAY_SIZE(pair));
			if (ret)
				return dev_err_probe(dev, ret,
						     "failed to read diff-channels property\n");

			if (pair[0] >= 8 || pair[1] >= 8)
				return dev_err_probe(dev, -EINVAL,
						     "diff-channels values must be between 0 and 7\n");

			spec->differential = 1;
			spec->channel = pair[0];
			spec->channel2 = pair[1];
		} else {
			return dev_err_probe(dev, -EINVAL,
					     "channel node missing channel type property\n");
		}

		if (fwnode_property_present(child, "excitation-channels")) {
			ret = fwnode_property_count_u32(child, "excitation-channels");
			if (ret < 0)
				return dev_err_probe(dev, ret,
						     "failed to read excitation-channels property\n");

			if (ret < 1 || ret > 2)
				return dev_err_probe(dev, -EINVAL,
						     "excitation-channels property must have 1 or 2 values\n");

			measurement->iadc_count = ret;
			pair[1] = 0;

			ret = fwnode_property_read_u32_array(child, "excitation-channels",
							     pair, measurement->iadc_count);
			if (ret)
				return dev_err_probe(dev, ret,
						     "failed to read excitation-channels property\n");

			if (pair[0] >= 8 || pair[1] >= 8)
				return dev_err_probe(dev, -EINVAL,
						     "excitation-channels values must be between 0 and 7\n");

			measurement->idac1_mux = pair[0];
			measurement->idac2_mux = measurement->iadc_count > 1 ? pair[1] : 0;

			ret = fwnode_property_read_u32_array(child, "excitation-current-nanoamp",
							     pair, measurement->iadc_count);
			if (ret)
				return dev_err_probe(dev, ret,
						     "failed to read excitation-current-nanoamp property\n");

			if (pair[0] <= 100000 && (measurement->iadc_count == 1 || pair[1] <= 100000)) {
				/*
				 * If both values are 100uA or less, then we can
				 * use IUNIT = 1uA for better precision.
				 */
				ret = ads112c14_populate_idac_mag(pair[0],
								  &measurement->idac1_mag);
				if (ret)
					return ret;

				if (measurement->iadc_count > 1) {
					ret = ads112c14_populate_idac_mag(pair[1],
									  &measurement->idac2_mag);
					if (ret)
						return ret;
				}
			} else {
				/*
				 * Otherwise, IUINT is 10uA (flag set) and so
				 * IxMAG is 1/10 of the actual current.
				 */
				measurement->iunit = 1;

				ret = ads112c14_populate_idac_mag(pair[0] / 10,
								  &measurement->idac1_mag);
				if (ret)
					return ret;

				if (measurement->iadc_count > 1) {
					ret = ads112c14_populate_idac_mag(pair[1] / 10,
									  &measurement->idac2_mag);
					if (ret)
						return ret;
				}
			}
		}

		measurement->global_chop = fwnode_property_read_bool(child,
								     "input-channel-rotation");

		if (fwnode_property_present(child, "burn-out-current-nanoamp")) {
			u32 burnout_nA;

			ret = fwnode_property_read_u32(child, "burn-out-current-nanoamp",
						       &burnout_nA);
			if (ret)
				return dev_err_probe(dev, ret,
						     "failed to read burn-out-current-nanoamp property\n");

			switch (burnout_nA) {
			case 200:
				measurement->burnout = ADS112C14_DEVICE_CFG_BOCS_200_nA;
				break;
			case 1000:
				measurement->burnout = ADS112C14_DEVICE_CFG_BOCS_1_uA;
				break;
			case 10000:
				measurement->burnout = ADS112C14_DEVICE_CFG_BOCS_10_uA;
				break;
			default:
				return dev_err_probe(dev, -EINVAL,
						     "invalid burn-out-current-nanoamp value\n");
			}
		}

		measurement->bipolar = fwnode_property_read_bool(child, "bipolar");

		if (fwnode_property_present(child, "reference-sources")) {
			ret = fwnode_property_match_property_string(child,
				"reference-sources", ads112c14_vref_source_names,
				ARRAY_SIZE(ads112c14_vref_source_names));
			if (ret < 0)
				return dev_err_probe(dev, ret,
						     "invalid reference-sources value\n");

			measurement->vref_source = ret;
		}

		if (measurement->vref_source == ADS112C14_VREF_SOURCE_AVDD)
			*need_avdd_ref = true;
		if (measurement->vref_source == ADS112C14_VREF_SOURCE_EXTERNAL)
			*need_ext_ref = true;

		spec->info_mask_separate = BIT(IIO_CHAN_INFO_RAW) | BIT(IIO_CHAN_INFO_SCALE);
		spec->info_mask_separate_available = BIT(IIO_CHAN_INFO_SCALE);

		/*
		 * If reference source is resistor rather than voltage supply,
		 * then the measurement is effectively a resistance measurement.
		 */
		spec->type = (measurement->vref_source == ADS112C14_VREF_SOURCE_EXTERNAL &&
			      data->ext_ref_ohms) ? IIO_RESISTANCE : IIO_VOLTAGE;

		if (spec->type == IIO_RESISTANCE)
			spec->differential = 0;

		i++;
	}

	data->num_measurements = i;
	memcpy(channels + i, ads112c14_sys_mon_channels, sizeof(ads112c14_sys_mon_channels));

	indio_dev->channels = channels;
	indio_dev->num_channels = i + ARRAY_SIZE(ads112c14_sys_mon_channels);

	return 0;
}

static void ads112c14_populate_scale_available(int scale_avail[][2],
					       u32 full_scale, u32 fsr_bits)
{
	for (u32 i = 0; i < ARRAY_SIZE(ads112c14_pga_gains_x10); i++) {
		int *entry = scale_avail[i];
		u64 gain_x10, nano_scale;

		gain_x10 = ads112c14_pga_gains_x10[i];
		nano_scale = div64_u64((u64)NANO * 10U * full_scale,
				       gain_x10 * BIT(fsr_bits));
		entry[0] = div_u64_rem(nano_scale, NANO, &entry[1]);
	}
}

static void ads112c14_populate_tables(struct ads112c14_data *data)
{
	u32 full_scale, fsr_bits;

	for (u32 i = 0; i < data->num_measurements; i++) {
		struct ads112c14_measurement *measurement = &data->measurements[i];

		switch (measurement->vref_source) {
		case ADS112C14_VREF_SOURCE_EXTERNAL:
			if (data->ext_ref_ohms)
				full_scale = data->ext_ref_ohms;
			else
				full_scale = data->ext_ref_uV / (MICRO / MILLI);
			break;
		case ADS112C14_VREF_SOURCE_AVDD:
			full_scale = data->avdd_uV / (MICRO / MILLI);
			break;
		case ADS112C14_VREF_SOURCE_INTERNAL_1_25V:
			full_scale = ADS112C14_INT_REF0_mV;
			break;
		default:
			full_scale = ADS112C14_INT_REF1_mV;
			break;
		}

		fsr_bits = data->chip_info->resolution_bits - measurement->bipolar;

		ads112c14_populate_scale_available(measurement->scale_available,
						   full_scale, fsr_bits);
	}

	/* For now, assuming all sys_mon channels are using 2.5V reference. */
	full_scale = ADS112C14_INT_REF1_mV;
	fsr_bits = data->chip_info->resolution_bits - 1;

	ads112c14_populate_scale_available(data->sys_mon_chan_short_scale_available,
					   full_scale, fsr_bits);
}

static int ads112c14_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	const struct ads112c14_chip_info *info;
	struct iio_dev *indio_dev;
	struct ads112c14_data *data;
	bool need_avdd_ref, need_ext_ref;
	u32 refp_uV = 0;
	u32 refn_uV = 0;
	u32 reg_val;
	int ret;

	info = i2c_get_match_data(client);
	if (!info)
		return dev_err_probe(dev, -EINVAL, "missing match data\n");

	indio_dev = devm_iio_device_alloc(dev, sizeof(*data));
	if (!indio_dev)
		return -ENOMEM;

	data = iio_priv(indio_dev);
	data->chip_info = info;

	if (device_property_present(dev, "ti,refp-refn-resistor-ohms")) {
		ret = device_property_read_u32(dev, "ti,refp-refn-resistor-ohms",
					       &data->ext_ref_ohms);
		if (ret)
			return dev_err_probe(dev, ret,
					     "failed to read ti,refp-refn-resistor-ohms property\n");
	}

	ret = ads112c14_parse_channels(indio_dev, &need_avdd_ref, &need_ext_ref);
	if (ret)
		return ret;

	ret = devm_regulator_get_enable(dev, "dvdd");
	if (ret)
		return dev_err_probe(dev, ret, "failed to get dvdd regulator\n");

	if (need_avdd_ref) {
		ret = devm_regulator_get_enable_read_voltage(dev, "avdd");
		if (ret < 0)
			return dev_err_probe(dev, ret, "failed to get avdd voltage\n");

		data->avdd_uV = ret;
	} else {
		ret = devm_regulator_get_enable(dev, "avdd");
		if (ret)
			return dev_err_probe(dev, ret, "failed to get avdd regulator\n");
	}

	if (device_property_present(dev, "refp-supply")) {
		ret = devm_regulator_get_enable_read_voltage(dev, "refp");
		if (ret < 0)
			return dev_err_probe(dev, ret, "failed to get refp voltage\n");

		refp_uV = ret;

		struct fwnode_handle *refp_fwnode __free(fwnode_handle) =
			fwnode_find_reference(dev->fwnode, "refp-supply", 0);
		if (IS_ERR(refp_fwnode))
			return dev_err_probe(dev, PTR_ERR(refp_fwnode),
					     "failed to get refp fwnode\n");

		struct fwnode_handle *avdd_fwnode __free(fwnode_handle) =
			fwnode_find_reference(dev->fwnode, "avdd-supply", 0);
		if (IS_ERR(avdd_fwnode))
			return dev_err_probe(dev, PTR_ERR(avdd_fwnode),
					     "failed to get avdd fwnode\n");

		/* REFP buffer should not be enabled when connected to AVDD */
		data->refp_is_avdd = refp_fwnode == avdd_fwnode;
	}

	if (device_property_present(dev, "refn-supply")) {
		ret = devm_regulator_get_enable_read_voltage(dev, "refn");
		if (ret < 0)
			return dev_err_probe(dev, ret, "failed to get refn voltage\n");

		refn_uV = ret;
	} else {
		data->refn_is_gnd = true;
	}

	data->ext_ref_uV = refp_uV - refn_uV;

	if (data->ext_ref_uV && data->ext_ref_ohms)
		return dev_err_probe(dev, -EINVAL,
				     "ti,refp-refn-resistor-ohms property should not be present when refp-supply or refn-supply is present\n");

	if (need_ext_ref && !data->ext_ref_uV && !data->ext_ref_ohms)
		return dev_err_probe(dev, -EINVAL,
				     "external reference measurements require either refp-supply or ti,refp-refn-resistor-ohms property\n");

	data->regmap = devm_regmap_init_i2c(client, &ads112c14_regmap_config);
	if (IS_ERR(data->regmap))
		return dev_err_probe(dev, PTR_ERR(data->regmap),
				     "failed to init regmap\n");

	/* Write magic reset value (0x16) to ensure known state.*/
	ret = regmap_write(data->regmap, ADS112C14_REG_CONVERSION_CTRL,
			   FIELD_PREP(ADS112C14_CONVERSION_CTRL_RESET, 0x16));
	/*
	 * The reset may cause an -EREMOTEIO error because of failing to get the
	 * I2C ACK at the end of the message. The device still gets reset so it
	 * is safe to ignore this error.
	 */
	if (ret == -EREMOTEIO)
		ret = 0;
	if (ret)
		return ret;

	fsleep(ADS112C14_DELAY_RESET_US);

	ret = regmap_read(data->regmap, ADS112C14_REG_STATUS_MSB, &reg_val);
	if (ret)
		return ret;

	if (FIELD_GET(ADS112C14_STATUS_MSB_RESETN, reg_val))
		return dev_err_probe(dev, -EIO, "reset failed\n");

	/* Default gain after reset is 1. */
	data->sys_mon_chan_short_gain_val = 1;

	/*
	 * Clear reset bit to prepare for next probe. And clear AVDD fault since
	 * that happens on every reset.
	 */
	ret = regmap_write(data->regmap, ADS112C14_REG_STATUS_MSB,
			   ADS112C14_STATUS_MSB_RESETN |
			   ADS112C14_STATUS_MSB_AVDD_UVN);
	if (ret)
		return ret;

	/* Place in single-shot conversion mode to make ready for raw read. */
	ret = regmap_set_bits(data->regmap, ADS112C14_REG_DEVICE_CFG,
			      ADS112C14_DEVICE_CFG_CONV_MODE);
	if (ret)
		return ret;

	ads112c14_populate_tables(data);

	indio_dev->name = info->name;
	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->info = &ads112c14_info;

	return devm_iio_device_register(dev, indio_dev);
}

static const struct ads112c14_chip_info ads112c14_chip_info = {
	.name = "ads112c14",
	.resolution_bits = 16,
};

static const struct ads112c14_chip_info ads122c14_chip_info = {
	.name = "ads122c14",
	.resolution_bits = 24,
};

static const struct of_device_id ads112c14_of_match[] = {
	{ .compatible = "ti,ads112c14", .data = &ads112c14_chip_info },
	{ .compatible = "ti,ads122c14", .data = &ads122c14_chip_info },
	{ }
};
MODULE_DEVICE_TABLE(of, ads112c14_of_match);

static const struct i2c_device_id ads112c14_id[] = {
	{ .name = "ads112c14", .driver_data = (kernel_ulong_t)&ads112c14_chip_info },
	{ .name = "ads122c14", .driver_data = (kernel_ulong_t)&ads122c14_chip_info },
	{ }
};
MODULE_DEVICE_TABLE(i2c, ads112c14_id);

static struct i2c_driver ads112c14_driver = {
	.driver = {
		.name = "ads112c14",
		.of_match_table = ads112c14_of_match,
	},
	.probe = ads112c14_probe,
	.id_table = ads112c14_id,
};
module_i2c_driver(ads112c14_driver);

MODULE_AUTHOR("David Lechner (TI) <dlechner@baylibre.com>");
MODULE_DESCRIPTION("TI ADS112C14 I2C ADC driver");
MODULE_LICENSE("GPL");
