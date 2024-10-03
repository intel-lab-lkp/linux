// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2024 Axis Communications AB
 *
 * Datasheet: https://www.ti.com/lit/gpn/opt4060
 *
 * Device driver for the Texas Instruments OPT4060 RGBW Color Sensor.
 */

#include <linux/bitfield.h>
#include <linux/i2c.h>
#include <linux/iio/iio.h>
#include <linux/math64.h>
#include <linux/units.h>
#include <linux/limits.h>
#include <linux/module.h>
#include <linux/property.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/iio/events.h>
#include <linux/iio/trigger.h>
#include <linux/iio/trigger_consumer.h>
#include <linux/iio/triggered_buffer.h>

#define OPT_4060_DRV_NAME	"opt4060"

/* OPT4060 register set */
#define OPT4060_RED_MSB				0x00
#define OPT4060_RED_LSB				0x01
#define OPT4060_GREEN_MSB			0x02
#define OPT4060_GREEN_LSB			0x03
#define OPT4060_BLUE_MSB			0x04
#define OPT4060_BLUE_LSB			0x05
#define OPT4060_CLEAR_MSB			0x06
#define OPT4060_CLEAR_LSB			0x07
#define OPT4060_THRESHOLD_LOW			0x08
#define OPT4060_THRESHOLD_HIGH			0x09
#define OPT4060_CTRL				0x0a
#define OPT4060_INT_CTRL			0x0b
#define OPT4060_RES_CTRL			0x0c
#define OPT4060_DEVICE_ID			0x11

/* OPT4060 register mask */
#define OPT4060_EXPONENT_MASK			GENMASK(15, 12)
#define OPT4060_MSB_MASK			GENMASK(11, 0)
#define OPT4060_LSB_MASK			GENMASK(15, 8)
#define OPT4060_COUNTER_MASK			GENMASK(7, 4)
#define OPT4060_CRC_MASK			GENMASK(3, 0)

/* OPT4060 device id mask */
#define OPT4060_DEVICE_ID_MASK			GENMASK(11, 0)

/* OPT4060 control register masks */
#define OPT4060_CTRL_QWAKE_MASK			BIT(15)
#define OPT4060_CTRL_RANGE_MASK			GENMASK(13, 10)
#define OPT4060_CTRL_CONV_TIME_MASK		GENMASK(9, 6)
#define OPT4060_CTRL_OPER_MODE_MASK		GENMASK(5, 4)
#define OPT4060_CTRL_LATCH_MASK			BIT(3)
#define OPT4060_CTRL_INT_POL_MASK		BIT(2)
#define OPT4060_CTRL_FAULT_COUNT_MASK		GENMASK(1, 0)

/* OPT4060 interrupt control register masks */
#define OPT4060_INT_CTRL_THRESH_SEL		GENMASK(6, 5)
#define OPT4060_INT_CTRL_OUTPUT			BIT(4)
#define OPT4060_INT_CTRL_INT_CFG		GENMASK(3, 2)
#define OPT4060_INT_CTRL_THRESHOLD		0x0
#define OPT4060_INT_CTRL_NEXT_CH		0x1
#define OPT4060_INT_CTRL_ALL_CH			0x3

/* OPT4060 result control register masks */
#define OPT4060_RES_CTRL_OVERLOAD		BIT(3)
#define OPT4060_RES_CTRL_CONV_READY		BIT(2)
#define OPT4060_RES_CTRL_FLAG_H			BIT(1)
#define OPT4060_RES_CTRL_FLAG_L			BIT(0)

/* OPT4060 constants */
#define OPT4060_DEVICE_ID_VAL			0x821

/* OPT4060 operating modes */
#define OPT4060_CTRL_OPER_MODE_OFF		0x0
#define OPT4060_CTRL_OPER_MODE_FORCED		0x1
#define OPT4060_CTRL_OPER_MODE_ONE_SHOT		0x2
#define OPT4060_CTRL_OPER_MODE_CONTINUOUS	0x3

/* OPT4060 conversion control register definitions */
#define OPT4060_CTRL_CONVERSION_0_6MS		0x0
#define OPT4060_CTRL_CONVERSION_1MS		0x1
#define OPT4060_CTRL_CONVERSION_1_8MS		0x2
#define OPT4060_CTRL_CONVERSION_3_4MS		0x3
#define OPT4060_CTRL_CONVERSION_6_5MS		0x4
#define OPT4060_CTRL_CONVERSION_12_7MS		0x5
#define OPT4060_CTRL_CONVERSION_25MS		0x6
#define OPT4060_CTRL_CONVERSION_50MS		0x7
#define OPT4060_CTRL_CONVERSION_100MS		0x8
#define OPT4060_CTRL_CONVERSION_200MS		0x9
#define OPT4060_CTRL_CONVERSION_400MS		0xa
#define OPT4060_CTRL_CONVERSION_800MS		0xb

/* OPT4060 fault count control register definitions */
#define OPT4060_CTRL_FAULT_COUNT_1		0x0
#define OPT4060_CTRL_FAULT_COUNT_2		0x1
#define OPT4060_CTRL_FAULT_COUNT_4		0x2
#define OPT4060_CTRL_FAULT_COUNT_8		0x3

/* OPT4060 scale light level range definitions */
#define OPT4060_CTRL_LIGHT_SCALE_AUTO		12

/* OPT4060 default values */
#define OPT4060_DEFAULT_CONVERSION_TIME OPT4060_CTRL_CONVERSION_50MS

/**
 * enum opt4060_chan_type - OPT4060 channel types
 * @OPT4060_RED:	Red channel.
 * @OPT4060_GREEN:	Green channel.
 * @OPT4060_BLUE:	Blue channel.
 * @OPT4060_CLEAR:	Clear (white) channel.
 * @OPT4060_NUM_CHANS:	Number of channel types.
 */
enum opt4060_chan_type {
	OPT4060_RED,
	OPT4060_GREEN,
	OPT4060_BLUE,
	OPT4060_CLEAR,
	OPT4060_NUM_CHANS
};

struct opt4060_chip {
	struct regmap *regmap;
	struct device *dev;
	struct iio_trigger *data_trig;
	bool data_trigger_active;
	struct iio_trigger *threshold_trig;
	bool threshold_trigger_active;
	u8 int_time;
	int irq;
	struct completion completion;
	bool thresh_event_lo_active;
	bool thresh_event_hi_active;
};

struct opt4060_channel_factor {
	u32 mul;
	u32 div;
};

struct opt4060_buffer {
	u32 chan[OPT4060_NUM_CHANS];
	s64 ts;
};

static const struct opt4060_channel_factor opt4060_channel_factors[] = {
	{
		/* RED 2.4 * 2.15e-3 */
		.mul = 516,
		.div = 100000,
	}, {
		/* GREEN 1.0 * 2.15e-3 */
		.mul = 215,
		.div = 100000,
	}, {
		/* BLUE 1.3 * 2.15e-3 */
		.mul = 258,
		.div = 100000,
	}, {
		/* CLEAR 1.0 * 2.15e-3 */
		.mul = 215,
		.div = 100000,
	},
};

static const int opt4060_int_time_available[][2] = {
	{ 0,    600 },
	{ 0,   1000 },
	{ 0,   1800 },
	{ 0,   3400 },
	{ 0,   6500 },
	{ 0,  12700 },
	{ 0,  25000 },
	{ 0,  50000 },
	{ 0, 100000 },
	{ 0, 200000 },
	{ 0, 400000 },
	{ 0, 800000 },
};

/*
 * Conversion time is integration time + time to set register
 * this is used as integration time.
 */
static const int opt4060_int_time_reg[][2] = {
	{    600,  OPT4060_CTRL_CONVERSION_0_6MS  },
	{   1000,  OPT4060_CTRL_CONVERSION_1MS    },
	{   1800,  OPT4060_CTRL_CONVERSION_1_8MS  },
	{   3400,  OPT4060_CTRL_CONVERSION_3_4MS  },
	{   6500,  OPT4060_CTRL_CONVERSION_6_5MS  },
	{  12700,  OPT4060_CTRL_CONVERSION_12_7MS },
	{  25000,  OPT4060_CTRL_CONVERSION_25MS   },
	{  50000,  OPT4060_CTRL_CONVERSION_50MS   },
	{ 100000,  OPT4060_CTRL_CONVERSION_100MS  },
	{ 200000,  OPT4060_CTRL_CONVERSION_200MS  },
	{ 400000,  OPT4060_CTRL_CONVERSION_400MS  },
	{ 800000,  OPT4060_CTRL_CONVERSION_800MS  },
};

static int opt4060_als_time_to_index(const u32 als_integration_time)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(opt4060_int_time_available); i++) {
		if (als_integration_time == opt4060_int_time_available[i][1])
			return i;
	}

	return -EINVAL;
}

static u8 opt4060_calculate_crc(u8 exp, u32 mantissa, u8 count)
{
	u8 crc;

	crc = (hweight32(mantissa) + hweight32(exp) + hweight32(count)) % 2;
	crc |= ((hweight32(mantissa & 0xAAAAA) + hweight32(exp & 0xA)
		 + hweight32(count & 0xA)) % 2) << 1;
	crc |= ((hweight32(mantissa & 0x88888) + hweight32(exp & 0x8)
		 + hweight32(count & 0x8)) % 2) << 2;
	crc |= (hweight32(mantissa & 0x80808) % 2) << 3;

	return crc;
}

static int opt4060_set_int_state(struct opt4060_chip *chip, u32 state)
{
	int ret;
	unsigned int regval;

	regval = FIELD_PREP(OPT4060_INT_CTRL_INT_CFG, state);
	ret = regmap_update_bits(chip->regmap, OPT4060_INT_CTRL,
				 OPT4060_INT_CTRL_INT_CFG, regval);
	if (ret)
		dev_err(chip->dev, "Failed to set interrupt config\n");
	return ret;
}

static int opt4060_trigger_one_shot(struct opt4060_chip *chip)
{
	int ret;
	unsigned int ctrl_reg;

	/* Enable interrupt for conversion ready of all channels */
	ret = opt4060_set_int_state(chip, OPT4060_INT_CTRL_ALL_CH);
	if (ret)
		return ret;

	ret = regmap_read(chip->regmap, OPT4060_CTRL, &ctrl_reg);
	if (ret < 0) {
		dev_err(chip->dev, "Failed to read ctrl register\n");
		return ret;
	}

	/* Trigger a one shot measurement by writing CRTL register */
	ret = regmap_write(chip->regmap, OPT4060_CTRL, ctrl_reg);
	if (ret)
		dev_err(chip->dev, "Failed to set ctrl register\n");

	return ret;
}

static int opt4060_set_continous_mode(struct opt4060_chip *chip,
				      bool continuous)
{
	u16 reg;
	int ret;

	if (continuous)
		reg = FIELD_PREP(OPT4060_CTRL_OPER_MODE_MASK,
				 OPT4060_CTRL_OPER_MODE_CONTINUOUS);
	else
		reg = FIELD_PREP(OPT4060_CTRL_OPER_MODE_MASK,
				 OPT4060_CTRL_OPER_MODE_ONE_SHOT);

	ret = regmap_update_bits(chip->regmap, OPT4060_CTRL,
				 OPT4060_CTRL_OPER_MODE_MASK, reg);
	if (ret)
		dev_err(chip->dev, "Failed to set configuration\n");

	return ret;
}

static int opt4060_read_raw_value(struct opt4060_chip *chip,
				  unsigned long address, u32 *raw)
{
	int ret;
	u32 result;
	u16 msb;
	u16 lsb;
	u8 exp;
	u8 count;
	u8 crc;
	u8 calc_crc;
	u32 mantissa_raw;

	ret = regmap_bulk_read(chip->regmap, address, &result, 2);
	if (ret)
		dev_err(chip->dev, "Reading channel data failed\n");
	else {
		count = FIELD_GET(OPT4060_COUNTER_MASK, result >> 16);
		crc = FIELD_GET(OPT4060_CRC_MASK, result >> 16);
		lsb = FIELD_GET(OPT4060_LSB_MASK, result >> 16);
		exp = FIELD_GET(OPT4060_EXPONENT_MASK, result);
		msb = FIELD_GET(OPT4060_MSB_MASK, result);
		mantissa_raw = (msb << 8) + lsb;
		calc_crc = opt4060_calculate_crc(exp, mantissa_raw, count);
		if (calc_crc != crc)
			return -EIO;

		*raw = mantissa_raw << exp;
	}
	return ret;
}

static int opt4060_read_chan_value(struct iio_dev *indio_dev,
				   struct iio_chan_spec const *chan,
				   int *val, int *val2, bool processed)
{
	struct opt4060_chip *chip = iio_priv(indio_dev);
	u32 adc_raw;
	int ret;
	/* Set timeout to two times the integration time multiplied by channel count. */
	unsigned int timeout_us = 2 * OPT4060_NUM_CHANS *
				  opt4060_int_time_reg[chip->int_time][0];

	if (chip->irq) {
		reinit_completion(&chip->completion);
		ret = opt4060_trigger_one_shot(chip);
		if (ret)
			return ret;
		if (wait_for_completion_timeout(&chip->completion,
						usecs_to_jiffies(timeout_us)) == 0)
			dev_info(chip->dev, "Completion timed out.\n");
	}

	ret = opt4060_read_raw_value(chip, chan->address, &adc_raw);
	if (ret) {
		dev_err(chip->dev, "Reading raw channel data failed\n");
		return ret;
	}
	if (processed) {
		u32 lux_raw;
		u32 rem;
		u32 mul = opt4060_channel_factors[chan->scan_index].mul;
		u32 div = opt4060_channel_factors[chan->scan_index].div;

		lux_raw = adc_raw * mul;
		*val = div_u64_rem(lux_raw, div, &rem);
		*val2 = rem * div_u64(div, 10);
		return IIO_VAL_INT_PLUS_NANO;
	}
	*val = adc_raw;
	*val2 = 0;
	return IIO_VAL_INT;
}

static int opt4060_set_conf(struct opt4060_chip *chip)
{
	u16 reg;
	int ret;

	reg = FIELD_PREP(OPT4060_CTRL_RANGE_MASK,
			 OPT4060_CTRL_LIGHT_SCALE_AUTO);
	reg |= FIELD_PREP(OPT4060_CTRL_CONV_TIME_MASK, chip->int_time);
	if (chip->irq)
		reg |= FIELD_PREP(OPT4060_CTRL_OPER_MODE_MASK,
				  OPT4060_CTRL_OPER_MODE_ONE_SHOT);
	else
		reg |= FIELD_PREP(OPT4060_CTRL_OPER_MODE_MASK,
				  OPT4060_CTRL_OPER_MODE_CONTINUOUS);
	reg |= OPT4060_CTRL_QWAKE_MASK | OPT4060_CTRL_LATCH_MASK;

	ret = regmap_write(chip->regmap, OPT4060_CTRL, reg);
	if (ret)
		dev_err(chip->dev, "Failed to set configuration\n");

	return ret;
}

static int opt4060_power_down(struct opt4060_chip *chip)
{
	int ret;
	unsigned int reg;

	ret = regmap_read(chip->regmap, OPT4060_DEVICE_ID, &reg);
	if (ret) {
		dev_err(chip->dev, "Failed to read configuration\n");
		return ret;
	}

	/* MODE_OFF is 0x0 so just set bits to 0 */
	reg &= ~OPT4060_CTRL_OPER_MODE_MASK;

	ret = regmap_write(chip->regmap, OPT4060_CTRL, reg);
	if (ret)
		dev_err(chip->dev, "Failed to power down\n");

	return ret;
}

static void opt4060_chip_off_action(void *data)
{
	struct opt4060_chip *chip = data;

	opt4060_power_down(chip);
}

#define OPT4060_CHAN(color)							\
{										\
	.type = IIO_LIGHT,							\
	.modified = 1,								\
	.channel2 = IIO_MOD_LIGHT_##color,					\
	.info_mask_separate = BIT(IIO_CHAN_INFO_PROCESSED) |			\
			      BIT(IIO_CHAN_INFO_RAW),				\
	.info_mask_shared_by_all = BIT(IIO_CHAN_INFO_INT_TIME),			\
	.info_mask_shared_by_all_available = BIT(IIO_CHAN_INFO_INT_TIME),	\
	.address = OPT4060_##color##_MSB,					\
	.scan_index = OPT4060_##color,						\
	.scan_type = {								\
		.sign = 'u',							\
		.realbits = 32,							\
		.storagebits = 32,						\
		.endianness = IIO_LE,						\
	},									\
	.event_spec = opt4060_event_spec,					\
	.num_event_specs = ARRAY_SIZE(opt4060_event_spec),			\
}

static const struct iio_event_spec opt4060_event_spec[] = {
	{
		.type = IIO_EV_TYPE_THRESH,
		.dir = IIO_EV_DIR_RISING,
		.mask_separate = BIT(IIO_EV_INFO_VALUE) |
				 BIT(IIO_EV_INFO_ENABLE),
	}, {
		.type = IIO_EV_TYPE_THRESH,
		.dir = IIO_EV_DIR_FALLING,
		.mask_separate = BIT(IIO_EV_INFO_VALUE) |
				 BIT(IIO_EV_INFO_ENABLE),
	}, {
		.type = IIO_EV_TYPE_THRESH,
		.dir = IIO_EV_DIR_EITHER,
		.mask_separate = BIT(IIO_EV_INFO_PERIOD),
	},
};

static const struct iio_chan_spec opt4060_channels[] = {
	OPT4060_CHAN(RED),
	OPT4060_CHAN(GREEN),
	OPT4060_CHAN(BLUE),
	OPT4060_CHAN(CLEAR),
	IIO_CHAN_SOFT_TIMESTAMP(OPT4060_NUM_CHANS),
};

static int opt4060_read_raw(struct iio_dev *indio_dev,
			    struct iio_chan_spec const *chan,
			    int *val, int *val2, long mask)
{
	struct opt4060_chip *chip = iio_priv(indio_dev);

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		return opt4060_read_chan_value(indio_dev, chan, val, val2,
					       false);
	case IIO_CHAN_INFO_PROCESSED:
		return opt4060_read_chan_value(indio_dev, chan, val, val2,
					       true);
	case IIO_CHAN_INFO_INT_TIME:
		*val = 0;
		*val2 = opt4060_int_time_reg[chip->int_time][0];
		return IIO_VAL_INT_PLUS_MICRO;
	default:
		return -EINVAL;
	}
}

static int opt4060_write_raw(struct iio_dev *indio_dev,
			     struct iio_chan_spec const *chan,
			     int val, int val2, long mask)
{
	struct opt4060_chip *chip = iio_priv(indio_dev);
	int int_time;

	switch (mask) {
	case IIO_CHAN_INFO_INT_TIME:
		int_time = opt4060_als_time_to_index(val2);
		if (int_time < 0)
			return int_time;
		chip->int_time = int_time;
		return opt4060_set_conf(chip);
	default:
		return -EINVAL;
	}
}

static int opt4060_write_raw_get_fmt(struct iio_dev *indio_dev,
				     struct iio_chan_spec const *chan,
				     long mask)
{
	switch (mask) {
	case IIO_CHAN_INFO_INT_TIME:
		return IIO_VAL_INT_PLUS_MICRO;
	default:
		return -EINVAL;
	}
}

static u32 opt4060_calc_th_reg(u32 adc_val)
{
	u32 th_val, th_exp;
	u32 bits;
	/* The threshold registers take 4 bits of exponent and 12 bits of data*/
	/* ADC = TH_VAL << (8 + TH_EXP) */

	bits = fls(adc_val);

	if (bits > 31)
		th_exp = 11; /* Maximum exponent */
	else if (bits > 20)
		th_exp = bits - 20;
	else
		th_exp = 0;
	th_val = (adc_val >> (8 + th_exp)) & 0xfff;

	return (th_exp << 12) + th_val;
}

static u32 opt4060_calc_val_from_th_reg(u32 th_reg)
{
	/* The threshold registers take 4 bits of exponent and 12 bits of data*/
	/* ADC = TH_VAL << (8 + TH_EXP) */
	u32 th_val, th_exp;

	th_exp = (th_reg >> 12) & 0xf;
	th_val = th_reg & 0xfff;

	return th_val << (8 + th_exp);
}

static bool opt4060_event_active(struct opt4060_chip *chip)
{
	return chip->thresh_event_lo_active || chip->thresh_event_hi_active;
}

static int opt4060_read_available(struct iio_dev *indio_dev,
				  struct iio_chan_spec const *chan,
				  const int **vals, int *type, int *length,
				  long mask)
{
	switch (mask) {
	case IIO_CHAN_INFO_INT_TIME:
		*length = ARRAY_SIZE(opt4060_int_time_available) * 2;
		*vals = (const int *)opt4060_int_time_available;
		*type = IIO_VAL_INT_PLUS_MICRO;
		return IIO_AVAIL_LIST;

	default:
		return -EINVAL;
	}
}


static int opt4060_event_set_state(struct opt4060_chip *chip, bool state)
{
	int ret = 0;

	if (state)
		ret = opt4060_set_continous_mode(chip, true);
	else if (!chip->data_trigger_active && chip->irq)
		ret = opt4060_set_continous_mode(chip, false);

	return ret;
}

static int opt4060_trigger_set_state(struct iio_trigger *trig, bool state)
{
	struct iio_dev *indio_dev = iio_trigger_get_drvdata(trig);
	struct opt4060_chip *chip = iio_priv(indio_dev);
	int ret = 0;

	if (indio_dev->trig) {
		iio_trigger_put(indio_dev->trig);
		indio_dev->trig = NULL;
	}

	if (state) {
		indio_dev->trig = iio_trigger_get(trig);
		ret = opt4060_set_continous_mode(chip, true);
	} else if (!opt4060_event_active(chip) && chip->irq)
		ret = opt4060_set_continous_mode(chip, false);

	if (trig == chip->data_trig) {
		chip->data_trigger_active = state;
		chip->threshold_trigger_active = !state;
	} else if (trig == chip->threshold_trig) {
		chip->data_trigger_active = !state;
		chip->threshold_trigger_active = state;
	}

	return ret;
}

static ssize_t opt4060_read_ev_period(struct opt4060_chip *chip, int *val,
				     int *val2)
{
	int ret, pers, fault_count, int_time;
	u64 uval;

	int_time = opt4060_int_time_reg[chip->int_time][0];

	ret = regmap_read(chip->regmap, OPT4060_CTRL, &fault_count);
	if (ret < 0)
		return ret;

	fault_count = fault_count & OPT4060_CTRL_FAULT_COUNT_MASK;
	switch (fault_count) {
	case OPT4060_CTRL_FAULT_COUNT_2:
		pers = 2;
		break;
	case OPT4060_CTRL_FAULT_COUNT_4:
		pers = 4;
		break;
	case OPT4060_CTRL_FAULT_COUNT_8:
		pers = 8;
		break;

	default:
		pers = 1;
	}

	uval = mul_u32_u32(int_time, pers);
	*val = div_u64_rem(uval, MICRO, val2);

	return IIO_VAL_INT_PLUS_MICRO;
}

static ssize_t opt4060_write_ev_period(struct opt4060_chip *chip, int val,
				      int val2)
{
	u64 uval, int_time;
	unsigned int regval, fault_count_val;

	uval = mul_u32_u32(val, MICRO) + val2;
	int_time = opt4060_int_time_reg[chip->int_time][0];

	/* Check if the period is closest to 1, 2, 4 or 8 times integration time.*/
	if (uval <= int_time)
		fault_count_val = OPT4060_CTRL_FAULT_COUNT_1;
	else if (uval <= int_time * 2)
		fault_count_val = OPT4060_CTRL_FAULT_COUNT_2;
	else if (uval <= int_time * 4)
		fault_count_val = OPT4060_CTRL_FAULT_COUNT_4;
	else
		fault_count_val = OPT4060_CTRL_FAULT_COUNT_8;

	regval = FIELD_PREP(OPT4060_CTRL_FAULT_COUNT_MASK, fault_count_val);
	return regmap_update_bits(chip->regmap, OPT4060_CTRL,
				 OPT4060_CTRL_FAULT_COUNT_MASK, regval);
}

static int opt4060_get_channel_sel(struct opt4060_chip *chip, int *ch_sel)
{
	int ret;
	u32 regval;

	ret = regmap_read(chip->regmap, OPT4060_INT_CTRL, &regval);
	if (ret)
		dev_err(chip->dev, "Failed to get channel selection.\n");
	else
		*ch_sel = FIELD_GET(OPT4060_INT_CTRL_THRESH_SEL, regval);
	return ret;
}

static int opt4060_set_channel_sel(struct opt4060_chip *chip, int ch_sel)
{
	int ret;
	u32 regval;

	regval = FIELD_PREP(OPT4060_INT_CTRL_THRESH_SEL, ch_sel);
	ret = regmap_update_bits(chip->regmap, OPT4060_INT_CTRL,
				 OPT4060_INT_CTRL_THRESH_SEL, regval);
	if (ret)
		dev_err(chip->dev, "Failed to set channel selection.\n");
	return ret;
}

static int opt4060_get_thresholds(struct opt4060_chip *chip, u32 *th_lo, u32 *th_hi)
{
	int ret;
	u32 regval;

	ret = regmap_read(chip->regmap, OPT4060_THRESHOLD_LOW, &regval);
	if (ret) {
		dev_err(chip->dev, "Failed to read THRESHOLD_LOW.\n");
		return ret;
	}
	*th_lo = opt4060_calc_val_from_th_reg(regval);

	ret = regmap_read(chip->regmap, OPT4060_THRESHOLD_HIGH, &regval);
	if (ret) {
		dev_err(chip->dev, "Failed to read THRESHOLD_LOW.\n");
		return ret;
	}
	*th_hi = opt4060_calc_val_from_th_reg(regval);

	return ret;
}

static int opt4060_set_thresholds(struct opt4060_chip *chip, u32 th_lo, u32 th_hi)
{
	int ret;

	ret = regmap_write(chip->regmap, OPT4060_THRESHOLD_LOW, opt4060_calc_th_reg(th_lo));
	if (ret) {
		dev_err(chip->dev, "Failed to write THRESHOLD_LOW.\n");
		return ret;
	}

	ret = regmap_write(chip->regmap, OPT4060_THRESHOLD_HIGH, opt4060_calc_th_reg(th_hi));
	if (ret)
		dev_err(chip->dev, "Failed to write THRESHOLD_HIGH.\n");

	return ret;
}

static int opt4060_read_event(struct iio_dev *indio_dev,
			      const struct iio_chan_spec *chan,
			      enum iio_event_type type,
			      enum iio_event_direction dir,
			      enum iio_event_info info,
			      int *val, int *val2)
{
	int ret = -EINVAL;
	struct opt4060_chip *chip = iio_priv(indio_dev);

	switch (info) {
	case IIO_EV_INFO_VALUE:
		if (chan->type == IIO_LIGHT && type == IIO_EV_TYPE_THRESH) {
			u32 th_lo, th_hi;

			if (opt4060_get_thresholds(chip, &th_lo, &th_hi))
				return -EFAULT;
			if (dir == IIO_EV_DIR_FALLING) {
				*val = th_lo;
				ret = IIO_VAL_INT;
			} else if (dir == IIO_EV_DIR_RISING) {
				*val = th_hi;
				ret = IIO_VAL_INT;
			}
		}
		break;
	case IIO_EV_INFO_PERIOD:
		return opt4060_read_ev_period(chip, val, val2);
	default:
		return -EINVAL;
	}
	return ret;
}

static int opt4060_write_event(struct iio_dev *indio_dev,
			       const struct iio_chan_spec *chan,
			       enum iio_event_type type,
			       enum iio_event_direction dir,
			       enum iio_event_info info,
			       int val, int val2)
{
	int ret = -EINVAL;
	struct opt4060_chip *chip = iio_priv(indio_dev);

	switch (info) {
	case IIO_EV_INFO_VALUE:
		if (chan->type == IIO_LIGHT && type == IIO_EV_TYPE_THRESH) {
			u32 th_lo, th_hi;

			if (opt4060_get_thresholds(chip, &th_lo, &th_hi))
				return -EFAULT;
			if (dir == IIO_EV_DIR_FALLING)
				th_lo = val;
			else if (dir == IIO_EV_DIR_RISING)
				th_hi = val;
			if (opt4060_set_thresholds(chip, th_lo, th_hi))
				return -EFAULT;
			ret = 0;
		}
		break;
	case IIO_EV_INFO_PERIOD:
		return opt4060_write_ev_period(chip, val, val2);
	default:
		return -EINVAL;
	}
	return ret;
}

static int opt4060_read_event_config(struct iio_dev *indio_dev,
				     const struct iio_chan_spec *chan,
				     enum iio_event_type type,
				     enum iio_event_direction dir)
{
	int ret = -EINVAL;
	struct opt4060_chip *chip = iio_priv(indio_dev);

	if (chan->type == IIO_LIGHT && type == IIO_EV_TYPE_THRESH) {
		int ch_idx = chan->scan_index;
		int ch_sel;

		if (opt4060_get_channel_sel(chip, &ch_sel))
			return -EFAULT;

		if (((dir == IIO_EV_DIR_FALLING) && chip->thresh_event_lo_active) ||
		    ((dir == IIO_EV_DIR_RISING) && chip->thresh_event_hi_active))
			ret = (ch_sel == ch_idx);
		else
			ret = FALSE;
	}
	return ret;
}

static int opt4060_write_event_config(struct iio_dev *indio_dev,
				      const struct iio_chan_spec *chan,
				      enum iio_event_type type,
				      enum iio_event_direction dir, int state)
{
	int ret = -EINVAL;
	struct opt4060_chip *chip = iio_priv(indio_dev);

	if (chan->type == IIO_LIGHT && type == IIO_EV_TYPE_THRESH) {
		int ch_idx = chan->scan_index;
		int ch_sel;

		if (opt4060_get_channel_sel(chip, &ch_sel))
			return -EFAULT;

		if (state) {
			/* Only one channel can be active at the same time */
			if ((chip->thresh_event_lo_active ||
			     chip->thresh_event_hi_active) && (ch_idx != ch_sel))
				return -EBUSY;
			if (dir == IIO_EV_DIR_FALLING)
				chip->thresh_event_lo_active = TRUE;
			else if (dir == IIO_EV_DIR_RISING)
				chip->thresh_event_hi_active = TRUE;
			if (opt4060_set_channel_sel(chip, ch_idx))
				return -EFAULT;
			ret = 0;
		} else {
			if (ch_idx == ch_sel) {
				if (dir == IIO_EV_DIR_FALLING)
					chip->thresh_event_lo_active = FALSE;
				else if (dir == IIO_EV_DIR_RISING)
					chip->thresh_event_hi_active = FALSE;
			}
			ret = 0;
		}

		if (opt4060_event_set_state(chip, chip->thresh_event_hi_active |
					    chip->thresh_event_lo_active))
			return -EFAULT;
	}
	return ret;
}

static const struct iio_info opt4060_info = {
	.read_raw = opt4060_read_raw,
	.write_raw = opt4060_write_raw,
	.write_raw_get_fmt = opt4060_write_raw_get_fmt,
	.read_avail = opt4060_read_available,
	.read_event_value = opt4060_read_event,
	.write_event_value = opt4060_write_event,
	.read_event_config = opt4060_read_event_config,
	.write_event_config = opt4060_write_event_config,
};

static int opt4060_load_defaults(struct opt4060_chip *chip)
{
	int ret;

	chip->int_time = OPT4060_DEFAULT_CONVERSION_TIME;

	/* Set initial MIN/MAX thresholds */
	ret = opt4060_set_thresholds(chip, 0, UINT_MAX);
	if (ret)
		return ret;

	return opt4060_set_conf(chip);
}

static bool opt4060_volatile_reg(struct device *dev, unsigned int reg)
{
	if ((reg >= OPT4060_RED_MSB && reg <= OPT4060_CLEAR_LSB) ||
	    reg == OPT4060_RES_CTRL)
		return true;
	return false;
}

static bool opt4060_writable_reg(struct device *dev, unsigned int reg)
{
	if (reg >= OPT4060_THRESHOLD_LOW || reg >= OPT4060_INT_CTRL)
		return true;
	return false;
}

static bool opt4060_readonly_reg(struct device *dev, unsigned int reg)
{
	if (reg == OPT4060_DEVICE_ID)
		return true;
	return false;
}

static bool opt4060_readable_reg(struct device *dev, unsigned int reg)
{
	/* Volatile, writable and read-only registers are readable. */
	if (opt4060_volatile_reg(dev, reg) || opt4060_writable_reg(dev, reg) ||
	    opt4060_readonly_reg(dev, reg))
		return TRUE;
	return false;
}

static const struct regmap_config opt4060_regmap_config = {
	.name = OPT_4060_DRV_NAME,
	.reg_bits = 8,
	.val_bits = 16,
	.cache_type = REGCACHE_RBTREE,
	.max_register = OPT4060_DEVICE_ID,
	.readable_reg = opt4060_readable_reg,
	.writeable_reg = opt4060_writable_reg,
	.volatile_reg = opt4060_volatile_reg,
	.val_format_endian = REGMAP_ENDIAN_BIG,
};

static const struct iio_trigger_ops opt4060_trigger_ops = {
	.validate_device = iio_trigger_validate_own_device,
	.set_trigger_state = opt4060_trigger_set_state,
};

static irqreturn_t opt4060_trigger_handler(int irq, void *p)
{
	struct iio_poll_func *pf = p;
	struct iio_dev *idev = pf->indio_dev;
	struct opt4060_chip *chip = iio_priv(idev);
	struct opt4060_buffer raw;
	int ret, chan;

	memset(&raw, 0, sizeof(raw));

	for (chan = 0; chan < OPT4060_NUM_CHANS; chan++) {
		ret = opt4060_read_raw_value(chip,
					     opt4060_channels[chan].address,
					     &raw.chan[chan]);
		if (ret) {
			dev_err(chip->dev, "Reading raw channel data failed\n");
			goto err_read;
		}
	}

	iio_push_to_buffers_with_timestamp(idev, &raw, pf->timestamp);
err_read:
	iio_trigger_notify_done(idev->trig);
	return IRQ_HANDLED;
}

static int opt4060_buffer_preenable(struct iio_dev *idev)
{
	struct opt4060_chip *chip = iio_priv(idev);

	return opt4060_trigger_one_shot(chip);
}

static const struct iio_buffer_setup_ops opt4060_buffer_ops = {
	.preenable = opt4060_buffer_preenable,
};

static irqreturn_t opt4060_irq_thread(int irq, void *private)
{
	struct iio_dev *idev = private;
	struct opt4060_chip *chip = iio_priv(idev);
	int ret, chan, dummy;
	unsigned int int_res;

	ret = regmap_read(chip->regmap, OPT4060_RES_CTRL, &int_res);
	if (ret < 0) {
		dev_err(chip->dev, "Failed to read interrupt reasons.\n");
		return IRQ_NONE;
	}

	if (!chip->data_trigger_active) {
		/* Only trigger interrupts on thresholds */
		ret = opt4060_set_int_state(chip, OPT4060_INT_CTRL_THRESHOLD);
		if (ret) {
			dev_err(chip->dev, "Failed to set interrupt state\n");
			return IRQ_NONE;
		}
	}

	/* Read OPT4060_CTRL to clear interrupt */
	ret = regmap_read(chip->regmap, OPT4060_CTRL, &dummy);
	if (ret < 0) {
		dev_err(chip->dev, "Failed to clear interrupt\n");
		return IRQ_NONE;
	}

	/* Handle events */
	if (int_res & (OPT4060_RES_CTRL_FLAG_H | OPT4060_RES_CTRL_FLAG_L)) {
		u64 code;

		/* Check if the interrupt is from the lower threshold */
		if (int_res & OPT4060_RES_CTRL_FLAG_L) {
			code = IIO_UNMOD_EVENT_CODE(IIO_LIGHT,
							chan,
							IIO_EV_TYPE_THRESH,
							IIO_EV_DIR_FALLING);
			iio_push_event(idev, code,
					iio_get_time_ns(idev));
		}
		/* Check if the interrupt is from the upper threshold */
		if (int_res & OPT4060_RES_CTRL_FLAG_H) {
			code = IIO_UNMOD_EVENT_CODE(IIO_LIGHT,
							chan,
							IIO_EV_TYPE_THRESH,
							IIO_EV_DIR_RISING);
			iio_push_event(idev, code,
					iio_get_time_ns(idev));
		}
		/* Handle threshold triggers */
		if (chip->threshold_trigger_active && iio_buffer_enabled(idev))
			iio_trigger_poll_nested(chip->threshold_trig);
	}

	/* Handle conversion ready */
	if (int_res & OPT4060_RES_CTRL_CONV_READY) {
		/* Signal completion for potentially waiting reads */
		complete(&chip->completion);

		/* Handle data ready triggers */
		if (chip->data_trigger_active && iio_buffer_enabled(idev))
			iio_trigger_poll_nested(chip->data_trig);
	}
	return IRQ_HANDLED;
}

static int opt4060_setup_triggers(struct opt4060_chip *chip, struct iio_dev *idev)
{
	struct iio_trigger *data_trigger;
	struct iio_trigger *threshold_trigger;
	char *name;
	int ret;

	ret = devm_iio_triggered_buffer_setup(chip->dev, idev,
					      &iio_pollfunc_store_time,
					      opt4060_trigger_handler,
					      &opt4060_buffer_ops);
	if (ret)
		return dev_err_probe(chip->dev, ret,
			     "iio_triggered_buffer_setup_ext FAIL\n");

	data_trigger = devm_iio_trigger_alloc(chip->dev, "%s-data-ready-dev%d",
				       idev->name, iio_device_id(idev));
	if (!data_trigger)
		return -ENOMEM;
	chip->data_trig = data_trigger;
	data_trigger->ops = &opt4060_trigger_ops;
	iio_trigger_set_drvdata(data_trigger, idev);
	ret = devm_iio_trigger_register(chip->dev, data_trigger);
	if (ret)
		return dev_err_probe(chip->dev, ret,
				     "Data ready trigger registration failed\n");

	threshold_trigger = devm_iio_trigger_alloc(chip->dev, "%s-threshold-dev%d",
				       idev->name, iio_device_id(idev));
	if (!threshold_trigger)
		return -ENOMEM;
	chip->threshold_trig = threshold_trigger;
	threshold_trigger->ops = &opt4060_trigger_ops;
	iio_trigger_set_drvdata(threshold_trigger, idev);
	ret = devm_iio_trigger_register(chip->dev, threshold_trigger);
	if (ret)
		return dev_err_probe(chip->dev, ret,
				     "Threshold trigger registration failed\n");


	name = devm_kasprintf(chip->dev, GFP_KERNEL, "%s-opt4060",
			      dev_name(chip->dev));

	ret = devm_request_threaded_irq(chip->dev, chip->irq, NULL, opt4060_irq_thread,
					IRQF_TRIGGER_FALLING | IRQF_TRIGGER_RISING |
					IRQF_ONESHOT, name, idev);
	if (ret)
		return dev_err_probe(chip->dev, ret, "Could not request IRQ\n");

	ret = regmap_write_bits(chip->regmap, OPT4060_INT_CTRL,
				OPT4060_INT_CTRL_OUTPUT,
				OPT4060_INT_CTRL_OUTPUT);
	if (ret)
		return dev_err_probe(chip->dev, ret,
				     "Failed to set interrupt as output\n");

	return 0;
}

static int opt4060_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct opt4060_chip *chip;
	struct iio_dev *indio_dev;
	int ret;
	uint dev_id;

	indio_dev = devm_iio_device_alloc(dev, sizeof(*chip));
	if (!indio_dev)
		return -ENOMEM;

	chip = iio_priv(indio_dev);

	/* Request regulator */
	ret = devm_regulator_get_enable(dev, "vdd");
	if (ret)
		return dev_err_probe(dev, ret, "Failed to enable vdd supply\n");

	chip->regmap = devm_regmap_init_i2c(client, &opt4060_regmap_config);
	if (IS_ERR(chip->regmap))
		return dev_err_probe(dev, PTR_ERR(chip->regmap),
				     "regmap initialization failed\n");

	chip->dev = dev;
	chip->irq = client->irq;
	init_completion(&chip->completion);

	indio_dev->info = &opt4060_info;

	ret = regmap_reinit_cache(chip->regmap, &opt4060_regmap_config);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to reinit regmap cache\n");

	ret = regmap_read(chip->regmap, OPT4060_DEVICE_ID, &dev_id);
	if (ret < 0)
		return dev_err_probe(dev, ret,
			"Failed to read the device ID register\n");

	dev_id = FIELD_GET(OPT4060_DEVICE_ID_MASK, dev_id);
	if (dev_id != OPT4060_DEVICE_ID_VAL)
		dev_warn(dev, "Device ID: %#04x unknown\n", dev_id);

	indio_dev->channels = opt4060_channels;
	indio_dev->num_channels = ARRAY_SIZE(opt4060_channels);
	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->name = OPT_4060_DRV_NAME;

	/* Initialize device with default settings */
	ret = opt4060_load_defaults(chip);
	if (ret < 0)
		return dev_err_probe(dev, ret,
				     "Failed to set sensor defaults\n");

	/* If irq is enabled, initialize triggers */
	if (chip->irq) {
		ret = opt4060_setup_triggers(chip, indio_dev);
		if (ret)
			return ret;
	} else {
		dev_info(chip->dev, "No IRQ, events and triggers disabled\n");
	}

	ret = devm_add_action_or_reset(dev, opt4060_chip_off_action, chip);
	if (ret < 0)
		return dev_err_probe(dev, ret,
				     "Failed to setup power off action\n");

	return devm_iio_device_register(dev, indio_dev);
}

/*
 * The compatible string determines which constants to use depending on
 * opt4060 packaging
 */
static const struct i2c_device_id opt4060_id[] = {
	{ OPT_4060_DRV_NAME, 0 },
	{}
};
MODULE_DEVICE_TABLE(i2c, opt4060_id);

static const struct of_device_id opt4060_of_match[] = {
	{ .compatible = "ti,opt4060" },
	{}
};
MODULE_DEVICE_TABLE(of, opt4060_of_match);

static struct i2c_driver opt4060_driver = {
	.driver = {
		.name = OPT_4060_DRV_NAME,
		.of_match_table = opt4060_of_match,
	},
	.probe = opt4060_probe,
	.id_table = opt4060_id,
};
module_i2c_driver(opt4060_driver);

MODULE_DESCRIPTION("Texas Instruments opt4060 RGBW color sensor driver");
MODULE_LICENSE("GPL");
