// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2026 Roman Vivchar <rva333@protonmail.com>
 *
 * Based on drivers/iio/adc/mt6359-auxadc.c
 */

#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/cleanup.h>
#include <linux/delay.h>
#include <linux/iio/iio.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/regmap.h>
#include <linux/types.h>

#include <linux/mfd/mt6397/core.h>
#include <linux/mfd/mt6323/registers.h>

#include <dt-bindings/iio/adc/mediatek,mt6323-auxadc.h>

#define AUXADC_RSTB_SEL		BIT(7)
#define AUXADC_RSTB_SW		BIT(5)

#define AUXADC_CTL_CK		BIT(5)

#define AUXADC_TRIM_CH2		(3 << 10)
#define AUXADC_TRIM_CH4		(3 << 8)
#define AUXADC_TRIM_CH5		(3 << 4)
#define AUXADC_TRIM_CH6		(3 << 2)

#define AUXADC_VREF18_ENB_MD	BIT(15)
#define AUXADC_MD_STATUS	BIT(0)

#define AUXADC_GPS_STATUS	BIT(1)

#define AUXADC_VREF18_SELB	BIT(1)
#define AUXADC_DECI_GDLY_SEL	BIT(0)

#define AUXADC_VBUF_EN		BIT(4)

#define AUXADC_DECI_GDLY_MASK		GENMASK(15, 14)
#define AUXADC_ADC19_BUSY_MASK		GENMASK(15, 0)
#define AUXADC_RDY_MASK			BIT(15)
#define AUXADC_DATA_MASK		GENMASK(14, 0)
#define AUXADC_OSR_MASK			GENMASK(12, 10)
#define AUXADC_LOW_CHANNEL_MASK		GENMASK(9, 0)
#define AUXADC_AUDIO_CHANNEL_MASK	GENMASK(8, 0)

#define MTK_PMIC_IIO_CHAN(_name, _idx, _ch_type) \
	{ .type = _ch_type,                              \
	  .indexed = 1,                                  \
	  .channel = _idx,                               \
	  .address = _idx,                               \
	  .datasheet_name = __stringify(_name),          \
	  .info_mask_separate = BIT(IIO_CHAN_INFO_RAW) | \
				BIT(IIO_CHAN_INFO_SCALE) }

static const struct iio_chan_spec mt6323_auxadc_channels[] = {
	MTK_PMIC_IIO_CHAN(baton2, MT6323_AUXADC_BATON2, IIO_VOLTAGE),
	MTK_PMIC_IIO_CHAN(ch6, MT6323_AUXADC_CH6, IIO_VOLTAGE),
	MTK_PMIC_IIO_CHAN(bat_temp, MT6323_AUXADC_BAT_TEMP, IIO_TEMP),
	MTK_PMIC_IIO_CHAN(chip_temp, MT6323_AUXADC_CHIP_TEMP, IIO_TEMP),
	MTK_PMIC_IIO_CHAN(vcdt, MT6323_AUXADC_VCDT, IIO_VOLTAGE),
	MTK_PMIC_IIO_CHAN(baton1, MT6323_AUXADC_BATON1, IIO_VOLTAGE),
	MTK_PMIC_IIO_CHAN(isense, MT6323_AUXADC_ISENSE, IIO_VOLTAGE),
	MTK_PMIC_IIO_CHAN(batsns, MT6323_AUXADC_BATSNS, IIO_VOLTAGE),
	MTK_PMIC_IIO_CHAN(accdet, MT6323_AUXADC_ACCDET, IIO_VOLTAGE),
};

/**
 * struct mt6323_auxadc - Main driver structure
 * @dev:           Device pointer
 * @regmap:        Regmap from PWRAP
 * @lock:          Mutex to serialize AUXADC reading vs configuration
 */
struct mt6323_auxadc {
	struct device *dev;
	struct regmap *regmap;
	struct mutex lock;
};

static u32 mt6323_auxadc_channel_to_reg(unsigned long channel)
{
	switch (channel) {
	case 0:
		return MT6323_AUXADC_ADC6;
	case 1:
		return MT6323_AUXADC_ADC11;
	case 2:
		return MT6323_AUXADC_ADC5;
	case 3:
		return MT6323_AUXADC_ADC4;
	case 4:
		return MT6323_AUXADC_ADC2;
	case 5:
		return MT6323_AUXADC_ADC3;
	case 6:
		return MT6323_AUXADC_ADC1;
	case 7:
		return MT6323_AUXADC_ADC0;
	case 8:
		return MT6323_AUXADC_ADC7;
	default:
		return MT6323_AUXADC_ADC17;
	}
}

static int mt6323_auxadc_check_if_stuck(struct mt6323_auxadc *auxadc)
{
	int i, ret;
	u32 val;

	for (i = 0; i < 50; i++) {
		ret = regmap_read(auxadc->regmap, MT6323_AUXADC_CON19, &val);
		if (ret)
			return ret;

		if (FIELD_GET(AUXADC_DECI_GDLY_MASK, val)) {
			ret = regmap_read(auxadc->regmap, MT6323_AUXADC_ADC19,
					  &val);
			if (ret)
				return ret;

			if (!FIELD_GET(AUXADC_ADC19_BUSY_MASK, val)) {
				ret = regmap_update_bits(
					auxadc->regmap, MT6323_AUXADC_CON19,
					FIELD_PREP(AUXADC_DECI_GDLY_MASK, 3),
					0x0);
				if (ret)
					return ret;
			}
		} else {
			return 0;
		}

		fsleep(10);
	}

	return -ETIMEDOUT;
}

static int mt6323_auxadc_request(struct mt6323_auxadc *auxadc,
				 unsigned long channel)
{
	int ret;
	u32 pmic_val, adc_val;

	if (channel < 9) {
		ret = regmap_update_bits(auxadc->regmap, MT6323_AUXADC_CON11,
					 AUXADC_VBUF_EN, AUXADC_VBUF_EN);
		if (ret)
			return ret;

		ret = regmap_read(auxadc->regmap, MT6323_AUXADC_CON22,
				  &pmic_val);
		if (ret)
			return ret;

		adc_val = FIELD_GET(AUXADC_LOW_CHANNEL_MASK, pmic_val);
		adc_val &= ~BIT(channel);

		ret = regmap_update_bits(auxadc->regmap, MT6323_AUXADC_CON22,
					 AUXADC_LOW_CHANNEL_MASK, adc_val);
		if (ret)
			return ret;

		ret = regmap_read(auxadc->regmap, MT6323_AUXADC_CON22,
				  &pmic_val);
		if (ret)
			return ret;

		adc_val = FIELD_GET(AUXADC_LOW_CHANNEL_MASK, pmic_val);
		adc_val |= BIT(channel);

		ret = regmap_update_bits(auxadc->regmap, MT6323_AUXADC_CON22,
					 AUXADC_LOW_CHANNEL_MASK, adc_val);

	} else {
		ret = regmap_read(auxadc->regmap, MT6323_AUXADC_CON23,
				  &pmic_val);
		if (ret)
			return ret;

		adc_val = FIELD_GET(AUXADC_AUDIO_CHANNEL_MASK, pmic_val);
		adc_val &= ~BIT(channel - 9);

		ret = regmap_update_bits(auxadc->regmap, MT6323_AUXADC_CON23,
					 AUXADC_AUDIO_CHANNEL_MASK, adc_val);
		if (ret)
			return ret;

		ret = regmap_read(auxadc->regmap, MT6323_AUXADC_CON23,
				  &pmic_val);
		if (ret)
			return ret;

		adc_val = FIELD_GET(AUXADC_AUDIO_CHANNEL_MASK, pmic_val);
		adc_val |= BIT(channel - 9);

		ret = regmap_update_bits(auxadc->regmap, MT6323_AUXADC_CON23,
					 AUXADC_AUDIO_CHANNEL_MASK, adc_val);
	}

	return ret;
}

static int mt6323_auxadc_read(struct mt6323_auxadc *auxadc,
			      const struct iio_chan_spec *chan, int *out)
{
	int ret;
	u32 reg = mt6323_auxadc_channel_to_reg(chan->address);
	u32 val;

	ret = regmap_read_poll_timeout(auxadc->regmap, reg, val,
				       (val & AUXADC_RDY_MASK), 1000, 100000);
	if (ret)
		return ret;

	*out = FIELD_GET(AUXADC_DATA_MASK, val);

	return 0;
}

static int mt6323_auxadc_read_raw(struct iio_dev *indio_dev,
				  const struct iio_chan_spec *chan, int *val,
				  int *val2, long mask)
{
	struct mt6323_auxadc *auxadc = iio_priv(indio_dev);
	int ret, mult = 1;

	if (mask == IIO_CHAN_INFO_RAW) {
		scoped_guard(mutex, &auxadc->lock)
		{
			ret = mt6323_auxadc_check_if_stuck(auxadc);
			if (ret)
				return ret;

			ret = mt6323_auxadc_request(auxadc, chan->address);
			if (ret)
				return ret;

			usleep_range(300, 500);

			ret = mt6323_auxadc_read(auxadc, chan, val);
			if (ret)
				return ret;
			return IIO_VAL_INT;
		}
	} else if (mask == IIO_CHAN_INFO_SCALE) {
		if (chan->channel == MT6323_AUXADC_ISENSE ||
		    chan->address == MT6323_AUXADC_BATSNS)
			mult = 4;

		*val = mult * 1800;
		*val2 = 32768;

		return IIO_VAL_FRACTIONAL;
	} else
		return -EINVAL;
}

static int mt6323_auxadc_init(struct mt6323_auxadc *auxadc)
{
	int ret;

	ret = regmap_update_bits(auxadc->regmap, MT6323_STRUP_CON10,
				 AUXADC_RSTB_SW | AUXADC_RSTB_SEL,
				 AUXADC_RSTB_SW | AUXADC_RSTB_SEL);
	if (ret)
		return ret;

	ret = regmap_update_bits(auxadc->regmap, MT6323_TOP_CKPDN2,
				 AUXADC_CTL_CK, AUXADC_CTL_CK);
	if (ret)
		return ret;

	ret = regmap_update_bits(auxadc->regmap, MT6323_AUXADC_CON10,
				 AUXADC_TRIM_CH2 | AUXADC_TRIM_CH4 |
					 AUXADC_TRIM_CH5 | AUXADC_TRIM_CH6,
				 AUXADC_TRIM_CH2 | AUXADC_TRIM_CH4 |
					 AUXADC_TRIM_CH5 | AUXADC_TRIM_CH6);
	if (ret)
		return ret;

	ret = regmap_update_bits(auxadc->regmap, MT6323_AUXADC_CON27,
				 AUXADC_VREF18_ENB_MD | AUXADC_MD_STATUS,
				 AUXADC_VREF18_ENB_MD | AUXADC_MD_STATUS);
	if (ret)
		return ret;

	ret = regmap_update_bits(auxadc->regmap, MT6323_AUXADC_CON19,
				 AUXADC_GPS_STATUS, AUXADC_GPS_STATUS);
	if (ret)
		return ret;

	ret = regmap_update_bits(auxadc->regmap, MT6323_AUXADC_CON26,
				 AUXADC_VREF18_SELB | AUXADC_DECI_GDLY_SEL,
				 AUXADC_VREF18_SELB | AUXADC_DECI_GDLY_SEL);
	if (ret)
		return ret;

	ret = regmap_update_bits(auxadc->regmap, MT6323_AUXADC_CON9,
				 AUXADC_OSR_MASK,
				 FIELD_PREP(AUXADC_OSR_MASK, 3));
	return ret;
}

static const struct iio_info mt6323_auxadc_iio_info = {
	.read_raw = mt6323_auxadc_read_raw,
};

static int mt6323_auxadc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mt6323_auxadc *auxadc;
	struct iio_dev *iio;
	struct regmap *regmap;
	int ret;

	/* mfd->pwrap regmap */
	regmap = dev_get_regmap(dev->parent->parent, NULL);
	if (!regmap)
		return dev_err_probe(dev, -ENODEV, "failed to get regmap\n");

	iio = devm_iio_device_alloc(dev, sizeof(*auxadc));
	if (!iio)
		return -ENOMEM;

	auxadc = iio_priv(iio);
	auxadc->regmap = regmap;
	auxadc->dev = dev;
	mutex_init(&auxadc->lock);

	ret = mt6323_auxadc_init(auxadc);
	if (ret)
		return dev_err_probe(dev, ret, "failed to initialize auxadc\n");

	iio->name = "mt6323-auxadc";
	iio->info = &mt6323_auxadc_iio_info;
	iio->modes = INDIO_DIRECT_MODE;
	iio->channels = mt6323_auxadc_channels;
	iio->num_channels = ARRAY_SIZE(mt6323_auxadc_channels);

	ret = devm_iio_device_register(dev, iio);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to register iio device\n");

	return 0;
}

static const struct of_device_id mt6323_auxadc_of_match[] = {
	{ .compatible = "mediatek,mt6323-auxadc" },
	{}
};
MODULE_DEVICE_TABLE(of, mt6323_auxadc_of_match);

static struct platform_driver mt6323_auxadc_driver = {
	.driver = {
		.name = "mt6323-auxadc",
		.of_match_table = mt6323_auxadc_of_match,
	},
	.probe	= mt6323_auxadc_probe,
};
module_platform_driver(mt6323_auxadc_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("MediaTek MT6323 PMIC AUXADC Driver");
