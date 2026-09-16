// SPDX-License-Identifier: GPL-2.0-only
/*
 * MediaTek MT6397 PMIC AUXADC IIO driver
 *
 * Copyright (c) 2026 Ryan Brue <ryanbrue.dev@gmail.com>
 *
 * Based on drivers/iio/adc/mt6323-auxadc.c
 */

#include <linux/array_size.h>
#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/cleanup.h>
#include <linux/delay.h>
#include <linux/iio/iio.h>
#include <linux/math.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/stringify.h>
#include <linux/time.h>
#include <linux/types.h>

#include <linux/mfd/mt6397/core.h>

#include <dt-bindings/iio/adc/mediatek,mt6397-auxadc.h>

/* The ready bit is in the raw result register, the value in the trimmed one. */
#define MT6397_AUXADC_ADC(n)		(0x0514 + 2 * (n))
#define MT6397_AUXADC_ADC_RDY		BIT(15)
#define MT6397_AUXADC_ADC_VAL		GENMASK(9, 0)
#define MT6397_AUXADC_ADC_TRIM(n)	(0x052a + 2 * (n))	/* ADC11 + n */

/* The thermistor needs the battery-detect bias and the input buffer on. */
#define MT6397_AUXADC_CON0		0x0542
#define MT6397_AUXADC_CON0_BUF_PWD_B	BIT(1)
#define MT6397_AUXADC_CON0_BUF_PWD_ON	BIT(3)
#define MT6397_CHR_CON7			0x000e
#define MT6397_CHR_CON7_BATON_TDET_EN	BIT(2)
#define MT6397_AUXADC_BATTEMP_SETTLE_US	(20 * USEC_PER_MSEC)

#define MT6397_AUXADC_CON1		0x0544
#define MT6397_AUXADC_CON1_CHSEL	GENMASK(10, 7)
#define MT6397_AUXADC_CON1_START	BIT(0)

/*
 * CHR_CON16 and the two SOURCE_CH0 selects route either BATSNS or ISENSE onto
 * the battery channel. Only ISENSE is used: with a switching charger in the
 * power path BATSNS sits on the system rail rather than on the pack.
 */
#define MT6397_CHR_CON16		0x0020
#define MT6397_CHR_CON16_ADCIN_VSEN_EN	BIT(11)
#define MT6397_CHR_CON16_ADCIN_VBAT_EN	BIT(10)
#define MT6397_CHR_CON16_ADCIN_SEL	(MT6397_CHR_CON16_ADCIN_VSEN_EN | \
					 MT6397_CHR_CON16_ADCIN_VBAT_EN)
#define MT6397_AUXADC_CON14		0x055e
#define MT6397_AUXADC_CON14_CH0_NORM_SEL BIT(2)
#define MT6397_AUXADC_CON14_CH0_LBAT_SEL BIT(0)
#define MT6397_AUXADC_ISENSE_SETTLE_US	USEC_PER_MSEC

/* Samples accumulated per conversion; the chip's accumulator is left off. */
#define MT6397_AUXADC_CON0_SPL_NUM	GENMASK(11, 7)

/* Conversions averaged in software per read. */
#define MT6397_AUXADC_SAMPLES		16

/* Hardware limitation: the result is not ready for this long after START. */
#define MT6397_AUXADC_START_US		30

/* The battery channel is divided down by 4; the thermistor is not divided. */
#define MT6397_AUXADC_ISENSE_DIVIDER	4

/* The PMIC's own channel numbers, as the CON1 select field takes them. */
#define MT6397_AUXADC_HWCHAN_BATSNS	0
#define MT6397_AUXADC_HWCHAN_BAT_TEMP	3

struct mt6397_auxadc {
	struct regmap *regmap;
	/* AUXADC doesn't support reading multiple channels simultaneously. */
	struct mutex lock;
};

#define MTK_PMIC_IIO_CHAN(_name, _chan, _hwchan)                \
{                                                               \
	.type = IIO_VOLTAGE,                                    \
	.indexed = 1,                                           \
	.channel = _chan,                                       \
	.address = _hwchan,                                     \
	.datasheet_name = __stringify(_name),                   \
	.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |          \
			      BIT(IIO_CHAN_INFO_SCALE),         \
}

/*
 * A device tree channel ID indexes this array; .address holds the PMIC channel
 * number. The thermistor reports as a voltage because that is all the PMIC
 * measures: the divider across an NTC whose curve belongs to the board.
 */
static const struct iio_chan_spec mt6397_auxadc_channels[] = {
	MTK_PMIC_IIO_CHAN(isense,   MT6397_AUXADC_ISENSE,
			  MT6397_AUXADC_HWCHAN_BATSNS),
	MTK_PMIC_IIO_CHAN(bat_temp, MT6397_AUXADC_BAT_TEMP,
			  MT6397_AUXADC_HWCHAN_BAT_TEMP),
};

static int mt6397_auxadc_read_once(struct mt6397_auxadc *adc,
				   const struct iio_chan_spec *chan, int *val)
{
	struct regmap *map = adc->regmap;
	unsigned int reg;
	int ret;

	ret = regmap_update_bits(map, MT6397_AUXADC_CON1,
				 MT6397_AUXADC_CON1_CHSEL,
				 FIELD_PREP(MT6397_AUXADC_CON1_CHSEL,
					    chan->address));
	if (ret)
		return ret;

	/* START is edge triggered: it has to be lowered before being raised. */
	ret = regmap_clear_bits(map, MT6397_AUXADC_CON1,
				MT6397_AUXADC_CON1_START);
	if (ret)
		return ret;

	ret = regmap_set_bits(map, MT6397_AUXADC_CON1,
			      MT6397_AUXADC_CON1_START);
	if (ret)
		return ret;

	fsleep(MT6397_AUXADC_START_US);

	ret = regmap_read_poll_timeout(map, MT6397_AUXADC_ADC(chan->address),
				       reg, reg & MT6397_AUXADC_ADC_RDY,
				       100, 100 * USEC_PER_MSEC);
	if (ret)
		return ret;

	ret = regmap_read(map, MT6397_AUXADC_ADC_TRIM(chan->address), &reg);
	if (ret)
		return ret;

	*val = FIELD_GET(MT6397_AUXADC_ADC_VAL, reg);

	return 0;
}

static int mt6397_auxadc_battemp_bias(struct mt6397_auxadc *adc, bool on)
{
	struct regmap *map = adc->regmap;
	int ret;

	if (on) {
		ret = regmap_set_bits(map, MT6397_AUXADC_CON0,
				      MT6397_AUXADC_CON0_BUF_PWD_ON);
		if (ret)
			return ret;
		ret = regmap_set_bits(map, MT6397_AUXADC_CON0,
				      MT6397_AUXADC_CON0_BUF_PWD_B);
		if (ret)
			return ret;
		return regmap_set_bits(map, MT6397_CHR_CON7,
				       MT6397_CHR_CON7_BATON_TDET_EN);
	}

	ret = regmap_clear_bits(map, MT6397_CHR_CON7,
				MT6397_CHR_CON7_BATON_TDET_EN);
	ret = ret ?: regmap_clear_bits(map, MT6397_AUXADC_CON0,
				       MT6397_AUXADC_CON0_BUF_PWD_B);
	return ret ?: regmap_clear_bits(map, MT6397_AUXADC_CON0,
					MT6397_AUXADC_CON0_BUF_PWD_ON);
}

static int mt6397_auxadc_isense_enable(struct mt6397_auxadc *adc)
{
	struct regmap *map = adc->regmap;
	int ret;

	ret = regmap_update_bits(map, MT6397_CHR_CON16,
				 MT6397_CHR_CON16_ADCIN_SEL,
				 MT6397_CHR_CON16_ADCIN_VSEN_EN);
	if (ret)
		return ret;

	return regmap_set_bits(map, MT6397_AUXADC_CON14,
			       MT6397_AUXADC_CON14_CH0_NORM_SEL |
			       MT6397_AUXADC_CON14_CH0_LBAT_SEL);
}

static int mt6397_auxadc_isense_disable(struct mt6397_auxadc *adc)
{
	struct regmap *map = adc->regmap;
	int ret;

	ret = regmap_clear_bits(map, MT6397_AUXADC_CON14,
				MT6397_AUXADC_CON14_CH0_NORM_SEL |
				MT6397_AUXADC_CON14_CH0_LBAT_SEL);

	return ret ?: regmap_clear_bits(map, MT6397_CHR_CON16,
					MT6397_CHR_CON16_ADCIN_SEL);
}

static int mt6397_auxadc_read_channel(struct mt6397_auxadc *adc,
				      const struct iio_chan_spec *chan,
				      int *val)
{
	unsigned int i, sum = 0;
	int ret, sample;

	/* Held across the whole burst: the channel select is shared state. */
	guard(mutex)(&adc->lock);

	if (chan->channel == MT6397_AUXADC_ISENSE) {
		ret = mt6397_auxadc_isense_enable(adc);
		if (ret)
			return ret;
		fsleep(MT6397_AUXADC_ISENSE_SETTLE_US);
	} else {
		ret = mt6397_auxadc_battemp_bias(adc, true);
		if (ret)
			return ret;
		fsleep(MT6397_AUXADC_BATTEMP_SETTLE_US);
	}

	for (i = 0; i < MT6397_AUXADC_SAMPLES; i++) {
		ret = mt6397_auxadc_read_once(adc, chan, &sample);
		if (ret)
			break;

		sum += sample;
	}

	/* Lower START so the converter is not left armed between reads. */
	regmap_clear_bits(adc->regmap, MT6397_AUXADC_CON1,
			  MT6397_AUXADC_CON1_START);

	if (chan->channel == MT6397_AUXADC_ISENSE)
		mt6397_auxadc_isense_disable(adc);
	else
		mt6397_auxadc_battemp_bias(adc, false);

	if (ret)
		return ret;

	*val = DIV_ROUND_CLOSEST(sum, MT6397_AUXADC_SAMPLES);

	return 0;
}

static int mt6397_auxadc_read_raw(struct iio_dev *indio_dev,
				  const struct iio_chan_spec *chan,
				  int *val, int *val2, long mask)
{
	struct mt6397_auxadc *adc = iio_priv(indio_dev);
	int ret;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		ret = mt6397_auxadc_read_channel(adc, chan, val);
		if (ret)
			return ret;

		return IIO_VAL_INT;

	case IIO_CHAN_INFO_SCALE:
		/* 1200 mV full range with 10-bit resolution. */
		*val = 1200;
		if (chan->channel == MT6397_AUXADC_ISENSE)
			*val *= MT6397_AUXADC_ISENSE_DIVIDER;
		*val2 = 10;

		return IIO_VAL_FRACTIONAL_LOG2;

	default:
		return -EINVAL;
	}
}

static int mt6397_auxadc_init(struct mt6397_auxadc *adc)
{
	return regmap_update_bits(adc->regmap, MT6397_AUXADC_CON0,
				  MT6397_AUXADC_CON0_SPL_NUM,
				  FIELD_PREP(MT6397_AUXADC_CON0_SPL_NUM, 1));
}

static const struct iio_info mt6397_auxadc_info = {
	.read_raw = mt6397_auxadc_read_raw,
};

static int mt6397_auxadc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mt6397_chip *chip = dev_get_drvdata(dev->parent);
	struct mt6397_auxadc *adc;
	struct iio_dev *indio_dev;
	int ret;

	indio_dev = devm_iio_device_alloc(dev, sizeof(*adc));
	if (!indio_dev)
		return -ENOMEM;

	adc = iio_priv(indio_dev);
	adc->regmap = chip->regmap;

	ret = devm_mutex_init(dev, &adc->lock);
	if (ret)
		return ret;

	ret = mt6397_auxadc_init(adc);
	if (ret)
		return dev_err_probe(dev, ret, "failed to initialize auxadc\n");

	indio_dev->name = "mt6397-auxadc";
	indio_dev->info = &mt6397_auxadc_info;
	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->channels = mt6397_auxadc_channels;
	indio_dev->num_channels = ARRAY_SIZE(mt6397_auxadc_channels);

	return devm_iio_device_register(dev, indio_dev);
}

static const struct of_device_id mt6397_auxadc_of_match[] = {
	{ .compatible = "mediatek,mt6397-auxadc" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, mt6397_auxadc_of_match);

static struct platform_driver mt6397_auxadc_driver = {
	.driver = {
		.name = "mt6397-auxadc",
		.of_match_table = mt6397_auxadc_of_match,
	},
	.probe = mt6397_auxadc_probe,
};
module_platform_driver(mt6397_auxadc_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Ryan Brue <ryanbrue.dev@gmail.com>");
MODULE_DESCRIPTION("MediaTek MT6397 PMIC AUXADC Driver");
