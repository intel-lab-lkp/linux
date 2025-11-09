// SPDX-License-Identifier: GPL-2.0-only
/*
 * TI ADS1120 4-channel, 2kSPS, 16-bit delta-sigma ADC
 *
 * Datasheet: https://www.ti.com/lit/gpn/ads1120
 *
 * Copyright (C) 2025 Ajith Anandhan <ajithanandhan0406@gmail.com>
 */

#include <linux/bitfield.h>
#include <linux/cleanup.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/module.h>
#include <linux/regmap.h>
#include <linux/spi/spi.h>
#include <linux/unaligned.h>

#include <linux/iio/iio.h>


/* Commands */
#define ADS1120_CMD_RESET		0x06
#define ADS1120_CMD_START		0x08
#define ADS1120_CMD_PWRDWN		0x02
#define ADS1120_CMD_RDATA		0x10
#define ADS1120_CMD_RREG		0x20
#define ADS1120_CMD_WREG		0x40

/* Registers */
#define ADS1120_REG_CONFIG0		0x00
#define ADS1120_REG_CONFIG1		0x01
#define ADS1120_REG_CONFIG2		0x02
#define ADS1120_REG_CONFIG3		0x03

/* Config Register 0 bit definitions */
#define ADS1120_CFG0_MUX_MASK		GENMASK(7, 4)
/* Differential inputs */
#define ADS1120_CFG0_MUX_AIN0_AIN1	0
#define ADS1120_CFG0_MUX_AIN0_AIN2	1
#define ADS1120_CFG0_MUX_AIN0_AIN3	2
#define ADS1120_CFG0_MUX_AIN1_AIN2	3
#define ADS1120_CFG0_MUX_AIN1_AIN3	4
#define ADS1120_CFG0_MUX_AIN2_AIN3	5
#define ADS1120_CFG0_MUX_AIN1_AIN0	6
#define ADS1120_CFG0_MUX_AIN3_AIN2	7
/* Single-ended inputs */
#define ADS1120_CFG0_MUX_AIN0_AVSS	8
#define ADS1120_CFG0_MUX_AIN1_AVSS	9
#define ADS1120_CFG0_MUX_AIN2_AVSS	10
#define ADS1120_CFG0_MUX_AIN3_AVSS	11
/* Diagnostic inputs */
#define ADS1120_CFG0_MUX_REFP_REFN_4	12
#define ADS1120_CFG0_MUX_AVDD_AVSS_4	13
#define ADS1120_CFG0_MUX_SHORTED	14

#define ADS1120_CFG0_GAIN_MASK		GENMASK(3, 1)
#define ADS1120_CFG0_GAIN_1		0
#define ADS1120_CFG0_GAIN_2		1
#define ADS1120_CFG0_GAIN_4		2
#define ADS1120_CFG0_GAIN_8		3
#define ADS1120_CFG0_GAIN_16		4
#define ADS1120_CFG0_GAIN_32		5
#define ADS1120_CFG0_GAIN_64		6
#define ADS1120_CFG0_GAIN_128		7

#define ADS1120_CFG0_PGA_BYPASS		BIT(0)

/* Config Register 1 bit definitions */
#define ADS1120_CFG1_DR_MASK		GENMASK(7, 5)
#define ADS1120_CFG1_DR_20SPS		0
#define ADS1120_CFG1_DR_45SPS		1
#define ADS1120_CFG1_DR_90SPS		2
#define ADS1120_CFG1_DR_175SPS		3
#define ADS1120_CFG1_DR_330SPS		4
#define ADS1120_CFG1_DR_600SPS		5
#define ADS1120_CFG1_DR_1000SPS		6

#define ADS1120_CFG1_MODE_MASK		GENMASK(4, 3)
#define ADS1120_CFG1_MODE_NORMAL	0
#define ADS1120_CFG1_MODE_DUTY		1
#define ADS1120_CFG1_MODE_TURBO		2

#define ADS1120_CFG1_CM_MASK		BIT(2)
#define ADS1120_CFG1_CM_SINGLE		0
#define ADS1120_CFG1_CM_CONTINUOUS	1

#define ADS1120_CFG1_TS_EN		BIT(1)
#define ADS1120_CFG1_BCS_EN		BIT(0)

/* Config Register 2 bit definitions */
#define ADS1120_CFG2_VREF_MASK		GENMASK(7, 6)
#define ADS1120_CFG2_VREF_INTERNAL	0
#define ADS1120_CFG2_VREF_EXT_REFP0	1
#define ADS1120_CFG2_VREF_EXT_AIN0	2
#define ADS1120_CFG2_VREF_AVDD		3

#define ADS1120_CFG2_REJECT_MASK	GENMASK(5, 4)
#define ADS1120_CFG2_REJECT_OFF		0
#define ADS1120_CFG2_REJECT_50_60	1
#define ADS1120_CFG2_REJECT_50		2
#define ADS1120_CFG2_REJECT_60		3

#define ADS1120_CFG2_PSW_EN		BIT(3)

#define ADS1120_CFG2_IDAC_MASK		GENMASK(2, 0)
#define ADS1120_CFG2_IDAC_OFF		0
#define ADS1120_CFG2_IDAC_10UA		1
#define ADS1120_CFG2_IDAC_50UA		2
#define ADS1120_CFG2_IDAC_100UA		3
#define ADS1120_CFG2_IDAC_250UA		4
#define ADS1120_CFG2_IDAC_500UA		5
#define ADS1120_CFG2_IDAC_1000UA	6
#define ADS1120_CFG2_IDAC_1500UA	7

/* Config Register 3 bit definitions */
#define ADS1120_CFG3_IDAC1_MASK		GENMASK(7, 5)
#define ADS1120_CFG3_IDAC1_DISABLED	0
#define ADS1120_CFG3_IDAC1_AIN0		1
#define ADS1120_CFG3_IDAC1_AIN1		2
#define ADS1120_CFG3_IDAC1_AIN2		3
#define ADS1120_CFG3_IDAC1_AIN3		4
#define ADS1120_CFG3_IDAC1_REFP0	5
#define ADS1120_CFG3_IDAC1_REFN0	6

#define ADS1120_CFG3_IDAC2_MASK		GENMASK(4, 2)
#define ADS1120_CFG3_IDAC2_DISABLED	0
#define ADS1120_CFG3_IDAC2_AIN0		1
#define ADS1120_CFG3_IDAC2_AIN1		2
#define ADS1120_CFG3_IDAC2_AIN2		3
#define ADS1120_CFG3_IDAC2_AIN3		4
#define ADS1120_CFG3_IDAC2_REFP0	5
#define ADS1120_CFG3_IDAC2_REFN0	6

#define ADS1120_CFG3_DRDYM_MASK		BIT(1)
#define ADS1120_CFG3_DRDYM_DRDY_ONLY	0
#define ADS1120_CFG3_DRDYM_BOTH		1

/* Conversion time for 20 SPS */
#define ADS1120_CONV_TIME_MS		51

/* Internal reference voltage in millivolts */
#define ADS1120_VREF_INTERNAL_MV	2048


struct ads1120_state {
	struct spi_device	*spi;
	struct regmap		*regmap;
	/*
	 * Protects chip configuration and ADC reads to ensure
	 * consistent channel/gain settings during conversions.
	 */
	struct mutex		lock;

	int vref_mv;

	/* DMA-safe buffer for SPI transfers */
	u8 data[4] __aligned(IIO_DMA_MINALIGN);
};


static const int ads1120_gain_values[] = { 1, 2, 4, 8, 16, 32, 64, 128 };
static const int ads1120_low_gain_values[] = { 1, 2, 4 };

/* Differential channel macro */
#define ADS1120_DIFF_CHANNEL(index, chan1, chan2)		\
{								\
	.type = IIO_VOLTAGE,					\
	.indexed = 1,						\
	.channel = chan1,					\
	.channel2 = chan2,					\
	.differential = 1,					\
	.address = index,					\
	.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |		\
			      BIT(IIO_CHAN_INFO_SCALE),		\
	.info_mask_separate_available = BIT(IIO_CHAN_INFO_SCALE), \
}

/* Single-ended channel macro */
#define ADS1120_SINGLE_CHANNEL(index, chan)			\
{								\
	.type = IIO_VOLTAGE,					\
	.indexed = 1,						\
	.channel = chan,					\
	.address = index,					\
	.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |		\
			      BIT(IIO_CHAN_INFO_SCALE),		\
	.info_mask_separate_available = BIT(IIO_CHAN_INFO_SCALE), \
}

/* Diagnostic channel macro */
#define ADS1120_DIAG_CHANNEL(index, label)			\
{								\
	.type = IIO_VOLTAGE,					\
	.indexed = 1,						\
	.channel = index,					\
	.address = index,					\
	.extend_name = label,					\
	.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |		\
			      BIT(IIO_CHAN_INFO_SCALE),		\
	.info_mask_separate_available = BIT(IIO_CHAN_INFO_SCALE), \
}

static const struct iio_chan_spec ads1120_channels[] = {
	/* Differential inputs */
	ADS1120_DIFF_CHANNEL(ADS1120_CFG0_MUX_AIN0_AIN1, 0, 1),
	ADS1120_DIFF_CHANNEL(ADS1120_CFG0_MUX_AIN0_AIN2, 0, 2),
	ADS1120_DIFF_CHANNEL(ADS1120_CFG0_MUX_AIN0_AIN3, 0, 3),
	ADS1120_DIFF_CHANNEL(ADS1120_CFG0_MUX_AIN1_AIN2, 1, 2),
	ADS1120_DIFF_CHANNEL(ADS1120_CFG0_MUX_AIN1_AIN3, 1, 3),
	ADS1120_DIFF_CHANNEL(ADS1120_CFG0_MUX_AIN2_AIN3, 2, 3),
	ADS1120_DIFF_CHANNEL(ADS1120_CFG0_MUX_AIN1_AIN0, 1, 0),
	ADS1120_DIFF_CHANNEL(ADS1120_CFG0_MUX_AIN3_AIN2, 3, 2),
	/* Single-ended inputs */
	ADS1120_SINGLE_CHANNEL(ADS1120_CFG0_MUX_AIN0_AVSS, 0),
	ADS1120_SINGLE_CHANNEL(ADS1120_CFG0_MUX_AIN1_AVSS, 1),
	ADS1120_SINGLE_CHANNEL(ADS1120_CFG0_MUX_AIN2_AVSS, 2),
	ADS1120_SINGLE_CHANNEL(ADS1120_CFG0_MUX_AIN3_AVSS, 3),
	/* Diagnostic inputs */
	ADS1120_DIAG_CHANNEL(ADS1120_CFG0_MUX_REFP_REFN_4, "ref_div4"),
	ADS1120_DIAG_CHANNEL(ADS1120_CFG0_MUX_AVDD_AVSS_4, "avdd_div4"),
	ADS1120_DIAG_CHANNEL(ADS1120_CFG0_MUX_SHORTED, "shorted"),
};

static bool ads1120_is_differential_channel(const struct iio_chan_spec *chan)
{
	return chan->address <= ADS1120_CFG0_MUX_AIN3_AIN2;
}

static int ads1120_write_cmd(struct ads1120_state *st, u8 cmd)
{
	st->data[0] = cmd;
	return spi_write(st->spi, st->data, sizeof(st->data[0]));
}

static int ads1120_reset(struct ads1120_state *st)
{
	int ret;

	ret = ads1120_write_cmd(st, ADS1120_CMD_RESET);
	if (ret)
		return ret;

	/*
	 * Datasheet specifies reset takes 50us + 32 * t(CLK)
	 * where t(CLK) = 1/4.096MHz (~0.24us).
	 * 50us + 32 * 0.24us = ~58us. Use 200us to be safe.
	 */
	fsleep(200);

	regcache_mark_dirty(st->regmap);

	return 0;
}

static int ads1120_set_mux(struct ads1120_state *st, u8 mux_val)
{
	if (mux_val > ADS1120_CFG0_MUX_SHORTED)
		return -EINVAL;

	return regmap_update_bits(st->regmap, ADS1120_REG_CONFIG0,
				  ADS1120_CFG0_MUX_MASK,
				  FIELD_PREP(ADS1120_CFG0_MUX_MASK, mux_val));
}

static int ads1120_set_gain(struct ads1120_state *st, int gain_val)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(ads1120_gain_values); i++) {
		if (ads1120_gain_values[i] == gain_val)
			break;
	}

	if (i == ARRAY_SIZE(ads1120_gain_values))
		return -EINVAL;

	return regmap_update_bits(st->regmap, ADS1120_REG_CONFIG0,
				  ADS1120_CFG0_GAIN_MASK,
				  FIELD_PREP(ADS1120_CFG0_GAIN_MASK, i));
}

static int ads1120_get_gain(struct ads1120_state *st)
{
	unsigned int val;
	int ret;

	ret = regmap_read(st->regmap, ADS1120_REG_CONFIG0, &val);
	if (ret)
		return ret;

	return ads1120_gain_values[FIELD_GET(ADS1120_CFG0_GAIN_MASK, val)];
}

static int ads1120_read_raw_adc(struct ads1120_state *st, int *val)
{
	int ret;
	struct spi_transfer xfer[2] = {
		{
			.tx_buf = st->data,
			.len = 1,
		}, {
			.rx_buf = st->data,
			.len = 2,
		}
	};

	st->data[0] = ADS1120_CMD_RDATA;

	ret = spi_sync_transfer(st->spi, xfer, ARRAY_SIZE(xfer));
	if (ret)
		return ret;

	*val = sign_extend32(get_unaligned_be16(st->data), 15);

	return 0;
}

static int ads1120_read_measurement(struct ads1120_state *st,
				    const struct iio_chan_spec *chan, int *val)
{
	int ret;

	ret = ads1120_set_mux(st, chan->address);
	if (ret)
		return ret;

	ret = ads1120_write_cmd(st, ADS1120_CMD_START);
	if (ret)
		return ret;

	msleep(ADS1120_CONV_TIME_MS);

	return ads1120_read_raw_adc(st, val);
}

static int ads1120_read_raw(struct iio_dev *indio_dev,
			    struct iio_chan_spec const *chan,
			    int *val, int *val2, long mask)
{
	struct ads1120_state *st = iio_priv(indio_dev);
	int ret, gain;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		guard(mutex)(&st->lock);
		ret = ads1120_read_measurement(st, chan, val);
		if (ret)
			return ret;
		return IIO_VAL_INT;

	case IIO_CHAN_INFO_SCALE:
		/*
		 * Scale = Vref / (gain * 2^15)
		 * Return in format: val / 2^val2
		 */
		gain = ads1120_get_gain(st);
		if (gain < 0)
			return gain;

		*val = st->vref_mv;
		*val2 = gain * 15;
		return IIO_VAL_FRACTIONAL_LOG2;

	default:
		return -EINVAL;
	}
}

static int ads1120_write_raw(struct iio_dev *indio_dev,
			     struct iio_chan_spec const *chan,
			     int val, int val2, long mask)
{
	struct ads1120_state *st = iio_priv(indio_dev);

	switch (mask) {
	case IIO_CHAN_INFO_SCALE:
		/*
		 * Validate gain for single-ended and diagnostic channels.
		 * These channels require PGA bypass and support only
		 * gains 1, 2, and 4.
		 */
		if (!ads1120_is_differential_channel(chan) && val > 4)
			return -EINVAL;

		guard(mutex)(&st->lock);
		return ads1120_set_gain(st, val);

	default:
		return -EINVAL;
	}
}

static int ads1120_read_avail(struct iio_dev *indio_dev,
			      struct iio_chan_spec const *chan,
			      const int **vals, int *type, int *length,
			      long mask)
{
	switch (mask) {
	case IIO_CHAN_INFO_SCALE:
		if (ads1120_is_differential_channel(chan)) {
			*vals = ads1120_gain_values;
			*length = ARRAY_SIZE(ads1120_gain_values);
		} else {
			*vals = ads1120_low_gain_values;
			*length = ARRAY_SIZE(ads1120_low_gain_values);
		}
		*type = IIO_VAL_INT;
		return IIO_AVAIL_LIST;

	default:
		return -EINVAL;
	}
}

static const struct iio_info ads1120_info = {
	.read_raw = ads1120_read_raw,
	.write_raw = ads1120_write_raw,
	.read_avail = ads1120_read_avail,
};

/* Regmap read function for ADS1120 */
static int ads1120_regmap_read(void *context, const void *reg_buf,
			       size_t reg_size, void *val_buf, size_t val_size)
{
	struct ads1120_state *st = context;
	u8 reg = *(u8 *)reg_buf;
	u8 *val = val_buf;
	int ret;
	struct spi_transfer xfer[2] = {
		{
			.tx_buf = st->data,
			.len = 1,
		}, {
			.rx_buf = val,
			.len = val_size,
		}
	};

	if (reg > ADS1120_REG_CONFIG3)
		return -EINVAL;

	/* RREG command: 0010rr00 where rr is register address */
	st->data[0] = ADS1120_CMD_RREG | (reg << 2);

	ret = spi_sync_transfer(st->spi, xfer, ARRAY_SIZE(xfer));
	if (ret)
		return ret;

	return 0;
}

/* Regmap write function for ADS1120 */
static int ads1120_regmap_write(void *context, const void *data, size_t count)
{
	struct ads1120_state *st = context;
	const u8 *buf = data;

	if (count != 2)
		return -EINVAL;

	/* WREG command: 0100rr00 where rr is register address */
	st->data[0] = ADS1120_CMD_WREG | (buf[0] << 2);
	st->data[1] = buf[1];

	return spi_write(st->spi, st->data, 2);
}

static const struct regmap_bus ads1120_regmap_bus = {
	.read = ads1120_regmap_read,
	.write = ads1120_regmap_write,
	.reg_format_endian_default = REGMAP_ENDIAN_BIG,
	.val_format_endian_default = REGMAP_ENDIAN_BIG,
};

static const struct regmap_config ads1120_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = ADS1120_REG_CONFIG3,
	.cache_type = REGCACHE_FLAT,
};

static int ads1120_init(struct ads1120_state *st)
{
	int ret;

	ret = ads1120_reset(st);
	if (ret)
		return dev_err_probe(&st->spi->dev, ret,
					"Failed to reset device\n");

	/*
	 * Configure Register 0:
	 * - Input MUX: AIN0/AVSS
	 * - Gain: 1
	 * - PGA bypass enabled. When gain is set > 4, this bit is
	 *   automatically ignored by the hardware and PGA is enabled,
	 *   so it's safe to leave it set.
	 */
	ret = regmap_write(st->regmap, ADS1120_REG_CONFIG0,
			   FIELD_PREP(ADS1120_CFG0_MUX_MASK,
				      ADS1120_CFG0_MUX_AIN0_AVSS) |
			   FIELD_PREP(ADS1120_CFG0_GAIN_MASK,
				      ADS1120_CFG0_GAIN_1) |
			   ADS1120_CFG0_PGA_BYPASS);
	if (ret)
		return ret;

	/*
	 * Configure Register 1:
	 * - Data rate: 20 SPS (for single-shot mode)
	 * - Operating mode: Normal
	 * - Conversion mode: Single-shot
	 * - Temperature sensor: Disabled
	 * - Burnout current: Disabled
	 */
	ret = regmap_write(st->regmap, ADS1120_REG_CONFIG1,
			   FIELD_PREP(ADS1120_CFG1_DR_MASK,
				      ADS1120_CFG1_DR_20SPS) |
			   FIELD_PREP(ADS1120_CFG1_MODE_MASK,
				      ADS1120_CFG1_MODE_NORMAL) |
			   FIELD_PREP(ADS1120_CFG1_CM_MASK,
				      ADS1120_CFG1_CM_SINGLE) |
			   FIELD_PREP(ADS1120_CFG1_TS_EN, 0) |
			   FIELD_PREP(ADS1120_CFG1_BCS_EN, 0));
	if (ret)
		return ret;

	/*
	 * Configure Register 2:
	 * - Voltage reference: Internal 2.048V
	 * - 50/60Hz rejection: Off
	 * - Power switch: Disabled
	 * - IDAC current: Off
	 */
	ret = regmap_write(st->regmap, ADS1120_REG_CONFIG2,
			   FIELD_PREP(ADS1120_CFG2_VREF_MASK,
				      ADS1120_CFG2_VREF_INTERNAL) |
			   FIELD_PREP(ADS1120_CFG2_REJECT_MASK,
				      ADS1120_CFG2_REJECT_OFF) |
			   FIELD_PREP(ADS1120_CFG2_PSW_EN, 0) |
			   FIELD_PREP(ADS1120_CFG2_IDAC_MASK,
				      ADS1120_CFG2_IDAC_OFF));
	if (ret)
		return ret;

	/*
	 * Configure Register 3:
	 * - IDAC1: Disabled
	 * - IDAC2: Disabled
	 * - DRDY mode: Only reflects data ready status
	 */
	ret = regmap_write(st->regmap, ADS1120_REG_CONFIG3,
			   FIELD_PREP(ADS1120_CFG3_IDAC1_MASK,
				      ADS1120_CFG3_IDAC1_DISABLED) |
			   FIELD_PREP(ADS1120_CFG3_IDAC2_MASK,
				      ADS1120_CFG3_IDAC2_DISABLED) |
			   FIELD_PREP(ADS1120_CFG3_DRDYM_MASK,
				      ADS1120_CFG3_DRDYM_DRDY_ONLY));
	if (ret)
		return ret;

	st->vref_mv = ADS1120_VREF_INTERNAL_MV;

	return 0;
}

static int ads1120_probe(struct spi_device *spi)
{
	struct device *dev = &spi->dev;
	struct iio_dev *indio_dev;
	struct ads1120_state *st;
	int ret;

	indio_dev = devm_iio_device_alloc(dev, sizeof(*st));
	if (!indio_dev)
		return -ENOMEM;

	st = iio_priv(indio_dev);
	st->spi = spi;

	ret = devm_mutex_init(dev, &st->lock);
	if (ret)
		return ret;

	st->regmap = devm_regmap_init(dev, &ads1120_regmap_bus, st,
				      &ads1120_regmap_config);
	if (IS_ERR(st->regmap))
		return dev_err_probe(dev, PTR_ERR(st->regmap),
					"Failed to initialize regmap\n");

	indio_dev->name = "ads1120";
	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->channels = ads1120_channels;
	indio_dev->num_channels = ARRAY_SIZE(ads1120_channels);
	indio_dev->info = &ads1120_info;

	ret = ads1120_init(st);
	if (ret)
		return dev_err_probe(dev, ret,
					"Failed to initialize device\n");

	return devm_iio_device_register(dev, indio_dev);
}

static const struct of_device_id ads1120_of_match[] = {
	{ .compatible = "ti,ads1120" },
	{ }
};
MODULE_DEVICE_TABLE(of, ads1120_of_match);

static const struct spi_device_id ads1120_id[] = {
	{ "ads1120" },
	{ }
};
MODULE_DEVICE_TABLE(spi, ads1120_id);

static struct spi_driver ads1120_driver = {
	.driver = {
		.name = "ads1120",
		.of_match_table = ads1120_of_match,
	},
	.probe = ads1120_probe,
	.id_table = ads1120_id,
};
module_spi_driver(ads1120_driver);

MODULE_AUTHOR("Ajith Anandhan <ajithanandhan0406@gmail.com>");
MODULE_DESCRIPTION("Texas Instruments ADS1120 ADC driver");
MODULE_LICENSE("GPL");
