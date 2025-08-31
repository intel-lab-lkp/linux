// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2025, Duje Mihanović <duje@dujemihanovic.xyz>
 */

#include <linux/bits.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/i2c.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <linux/types.h>
#include <linux/units.h>

#include <linux/iio/iio.h>
#include <linux/iio/types.h>

#include <linux/mfd/88pm886.h>

struct pm886_gpadc {
	struct regmap *map;
};

static const int pm886_gpadc_regs[] = {
	PM886_REG_GPADC_VSC,
	PM886_REG_GPADC_VCHG_PWR,
	PM886_REG_GPADC_VCF_OUT,
	PM886_REG_GPADC_VBAT,
	PM886_REG_GPADC_VBAT_SLP,
	PM886_REG_GPADC_VBUS,

	PM886_REG_GPADC_GPADC0,
	PM886_REG_GPADC_GPADC1,
	PM886_REG_GPADC_GPADC2,
	PM886_REG_GPADC_GPADC3,

	PM886_REG_GPADC_GND_DET1,
	PM886_REG_GPADC_GND_DET2,
	PM886_REG_GPADC_MIC_DET,

	PM886_REG_GPADC_TINT,
};

/* Must be kept in sync with the table above */
enum pm886_gpadc_channel {
	VSC_CHAN,
	VCHG_PWR_CHAN,
	VCF_OUT_CHAN,
	VBAT_CHAN,
	VBAT_SLP_CHAN,
	VBUS_CHAN,

	GPADC0_CHAN,
	GPADC1_CHAN,
	GPADC2_CHAN,
	GPADC3_CHAN,

	GND_DET1_CHAN,
	GND_DET2_CHAN,
	MIC_DET_CHAN,

	TINT_CHAN,
};

#define ADC_CHANNEL_VOLTAGE(index, lsb, name) {	\
	.type = IIO_VOLTAGE, \
	.indexed = 1, \
	.channel = index, \
	.address = lsb, \
	.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) | \
			      BIT(IIO_CHAN_INFO_SCALE), \
	.datasheet_name = name, \
}

#define ADC_CHANNEL_RESISTANCE(index, lsb, name) { \
	.type = IIO_RESISTANCE, \
	.indexed = 1, \
	.channel = index, \
	.address = lsb, \
	.info_mask_separate = BIT(IIO_CHAN_INFO_PROCESSED), \
	.datasheet_name = name, \
}

#define ADC_CHANNEL_TEMPERATURE(index, lsb, name) { \
	.type = IIO_TEMP, \
	.indexed = 1, \
	.channel = index, \
	.address = lsb, \
	.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) | \
			      BIT(IIO_CHAN_INFO_SCALE) | \
			      BIT(IIO_CHAN_INFO_OFFSET), \
	.datasheet_name = name, \
}

static const struct iio_chan_spec pm886_gpadc_channels[] = {
	ADC_CHANNEL_VOLTAGE(VSC_CHAN, 1367, "vsc"),
	ADC_CHANNEL_VOLTAGE(VCHG_PWR_CHAN, 1709, "vchg_pwr"),
	ADC_CHANNEL_VOLTAGE(VCF_OUT_CHAN, 1367, "vcf_out"),
	ADC_CHANNEL_VOLTAGE(VBAT_CHAN, 1367, "vbat"),
	ADC_CHANNEL_VOLTAGE(VBAT_SLP_CHAN, 1367, "vbat_slp"),
	ADC_CHANNEL_VOLTAGE(VBUS_CHAN, 1709, "vbus"),

	ADC_CHANNEL_RESISTANCE(GPADC0_CHAN, 342, "gpadc0"),
	ADC_CHANNEL_RESISTANCE(GPADC1_CHAN, 342, "gpadc1"),
	ADC_CHANNEL_RESISTANCE(GPADC2_CHAN, 342, "gpadc2"),
	ADC_CHANNEL_RESISTANCE(GPADC3_CHAN, 342, "gpadc3"),

	ADC_CHANNEL_VOLTAGE(GND_DET1_CHAN, 342, "gnddet1"),
	ADC_CHANNEL_VOLTAGE(GND_DET2_CHAN, 342, "gnddet2"),
	ADC_CHANNEL_VOLTAGE(MIC_DET_CHAN, 1367, "mic_det"),

	ADC_CHANNEL_TEMPERATURE(TINT_CHAN, 104, "tint"),
};

static const struct regmap_config pm886_gpadc_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = PM886_REG_GPADC_VBAT_SLP + 1,
};

static int gpadc_get_raw(struct iio_dev *iio, enum pm886_gpadc_channel chan)
{
	struct pm886_gpadc *gpadc = iio_priv(iio);
	__be16 buf;
	int ret;

	ret = regmap_bulk_read(gpadc->map, pm886_gpadc_regs[chan], &buf, sizeof(buf));
	return !ret ? be16_to_cpu(buf) >> 4 : ret;
}

static int
gpadc_set_bias(struct pm886_gpadc *gpadc, enum pm886_gpadc_channel chan, bool on)
{
	unsigned int gpadc_num = chan - GPADC0_CHAN;
	unsigned int bits = BIT(gpadc_num + 4) | BIT(gpadc_num);

	return regmap_assign_bits(gpadc->map, PM886_REG_GPADC_CONFIG(0x14), bits, on);
}

static int
gpadc_find_bias_current(struct iio_dev *iio, struct iio_chan_spec const *chan,
			unsigned int *raw_uv, unsigned int *raw_ua)
{
	struct pm886_gpadc *gpadc = iio_priv(iio);
	unsigned int gpadc_num = chan->channel - GPADC0_CHAN;
	unsigned int reg = PM886_REG_GPADC_CONFIG(0xb + gpadc_num);
	unsigned long lsb = chan->address;
	int ret;

	for (unsigned int i = 0; i < PM886_GPADC_BIAS_LEVELS; i++) {
		ret = regmap_update_bits(gpadc->map, reg, GENMASK(3, 0), i);
		if (ret)
			return ret;

		fsleep(5000);

		*raw_ua = PM886_GPADC_INDEX_TO_BIAS_UA(i);
		*raw_uv = gpadc_get_raw(iio, chan->channel) * lsb;

		/*
		 * Vendor kernel errors out above 1.25V, but testing shows that
		 * the resistance of the battery detection channel (GPADC2 on
		 * coreprimevelte) reaches about 1.4Mohm when the battery is
		 * removed, which can't be measured with such a low upper
		 * limit. Therefore, to be able to detect the battery without
		 * ugly externs as used in the vendor fuelgauge driver,
		 * increase this limit a bit.
		 */
		if (WARN_ON(*raw_uv > 1500 * (MICRO / MILLI)))
			return -EIO;

		/*
		 * Vendor kernel errors out under 300mV, but for the same
		 * reason as above (except the channel hovers around 3.5kohm
		 * with battery present) reduce this limit.
		 */
		if (*raw_uv < 200 * (MICRO / MILLI)) {
			dev_dbg(&iio->dev, "bad bias for chan %d: %duA @ %duV\n", chan->channel,
				*raw_ua, *raw_uv);
			continue;
		}

		dev_dbg(&iio->dev, "good bias for chan %d: %duA @ %duV\n", chan->channel,
			*raw_ua, *raw_uv);
		return 0;
	}

	dev_err(&iio->dev, "failed to find good bias for chan %d\n", chan->channel);
	return -EINVAL;
}

static int
gpadc_get_resistance_ohm(struct iio_dev *iio, struct iio_chan_spec const *chan)
{
	struct pm886_gpadc *gpadc = iio_priv(iio);
	unsigned int raw_uv, raw_ua;
	int ret;

	ret = gpadc_set_bias(gpadc, chan->channel, true);
	if (ret)
		goto err;

	ret = gpadc_find_bias_current(iio, chan, &raw_uv, &raw_ua);
	if (ret)
		goto err;

	ret = gpadc_set_bias(gpadc, chan->channel, false);
	if (ret)
		return ret;

	return DIV_ROUND_CLOSEST(raw_uv, raw_ua);
err:
	gpadc_set_bias(gpadc, chan->channel, false);
	return ret;
}

static int
__pm886_gpadc_read_raw(struct iio_dev *iio, struct iio_chan_spec const *chan,
		       int *val, int *val2, long mask)
{
	unsigned long lsb = chan->address;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		*val = gpadc_get_raw(iio, chan->channel);
		if (*val < 0)
			return *val;

		dev_dbg(&iio->dev, "chan: %d, raw: %d\n", chan->channel, *val);
		return IIO_VAL_INT;
	case IIO_CHAN_INFO_SCALE:
		*val = lsb;
		*val2 = (MICRO / MILLI);
		return chan->type == IIO_VOLTAGE
		       ? IIO_VAL_FRACTIONAL
		       : IIO_VAL_INT;
	case IIO_CHAN_INFO_OFFSET:
		/* Raw value is 104 millikelvin/LSB, convert it to 104 millicelsius/LSB */
		*val = ABSOLUTE_ZERO_MILLICELSIUS;
		*val2 = lsb;
		return IIO_VAL_FRACTIONAL;
	case IIO_CHAN_INFO_PROCESSED:
		*val = gpadc_get_resistance_ohm(iio, chan);
		if (*val < 0)
			return *val;

		dev_dbg(&iio->dev, "chan: %d, %d Ohm\n", chan->channel, *val);
		return IIO_VAL_INT;
	default:
		return -EINVAL;
	}
}

static int pm886_gpadc_read_raw(struct iio_dev *iio, struct iio_chan_spec const *chan,
				int *val, int *val2, long mask)
{
	struct device *dev = iio->dev.parent;
	int ret;

	ret = pm_runtime_resume_and_get(dev);
	if (ret)
		return ret;

	ret = __pm886_gpadc_read_raw(iio, chan, val, val2, mask);

	pm_runtime_put_autosuspend(dev);
	return ret;
}

static int pm886_gpadc_setup(struct regmap *map, bool enable)
{
	const u8 config[] = {
		PM886_GPADC_CONFIG1_EN_ALL,
		PM886_GPADC_CONFIG2_EN_ALL,
		PM886_GPADC_GND_DET2_EN
	};
	int ret;

	/* Enable/disable the ADC block */
	ret = regmap_assign_bits(map, PM886_REG_GPADC_CONFIG(0x6), BIT(0), enable);
	if (ret)
		return ret;

	if (!enable)
		return 0;

	/* If enabling, enable all channels */
	return regmap_bulk_write(map, PM886_REG_GPADC_CONFIG(0x1), config, ARRAY_SIZE(config));
}

static const struct iio_info pm886_gpadc_iio_info = {
	.read_raw = pm886_gpadc_read_raw,
};

static int pm886_gpadc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct pm886_chip *chip = dev_get_drvdata(dev->parent);
	struct i2c_client *client = chip->client;
	struct pm886_gpadc *gpadc;
	struct i2c_client *page;
	struct iio_dev *iio;
	int ret;

	iio = devm_iio_device_alloc(dev, sizeof(*gpadc));
	if (!iio)
		return -ENOMEM;

	gpadc = iio_priv(iio);
	dev_set_drvdata(dev, iio);

	page = devm_i2c_new_dummy_device(dev, client->adapter,
					 client->addr + PM886_PAGE_OFFSET_GPADC);
	if (IS_ERR(page))
		return dev_err_probe(dev, PTR_ERR(page), "Failed to initialize GPADC page\n");

	gpadc->map = devm_regmap_init_i2c(page, &pm886_gpadc_regmap_config);
	if (IS_ERR(gpadc->map))
		return dev_err_probe(dev, PTR_ERR(gpadc->map),
				     "Failed to initialize GPADC regmap\n");

	iio->name = "88pm886-gpadc";
	iio->dev.of_node = dev->parent->of_node;
	iio->modes = INDIO_DIRECT_MODE;
	iio->info = &pm886_gpadc_iio_info;
	iio->channels = pm886_gpadc_channels;
	iio->num_channels = ARRAY_SIZE(pm886_gpadc_channels);

	pm_runtime_set_autosuspend_delay(dev, 50);
	pm_runtime_use_autosuspend(dev);
	ret = devm_pm_runtime_enable(dev);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to enable runtime PM\n");

	ret = devm_iio_device_register(dev, iio);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to register ADC\n");

	return 0;
}

static int pm886_gpadc_runtime_resume(struct device *dev)
{
	struct iio_dev *iio = dev_get_drvdata(dev);
	struct pm886_gpadc *gpadc = iio_priv(iio);

	return pm886_gpadc_setup(gpadc->map, true);
}

static int pm886_gpadc_runtime_suspend(struct device *dev)
{
	struct iio_dev *iio = dev_get_drvdata(dev);
	struct pm886_gpadc *gpadc = iio_priv(iio);

	return pm886_gpadc_setup(gpadc->map, false);
}

static DEFINE_RUNTIME_DEV_PM_OPS(pm886_gpadc_pm_ops,
				 pm886_gpadc_runtime_suspend,
				 pm886_gpadc_runtime_resume, NULL);

static const struct platform_device_id pm886_gpadc_id[] = {
	{ "88pm886-gpadc" },
	{ }
};
MODULE_DEVICE_TABLE(platform, pm886_gpadc_id);

static struct platform_driver pm886_gpadc_driver = {
	.driver = {
		.name = "88pm886-gpadc",
		.pm = pm_ptr(&pm886_gpadc_pm_ops),
	},
	.probe = pm886_gpadc_probe,
	.id_table = pm886_gpadc_id,
};
module_platform_driver(pm886_gpadc_driver);

MODULE_AUTHOR("Duje Mihanović <duje@dujemihanovic.xyz>");
MODULE_DESCRIPTION("Marvell 88PM886 GPADC driver");
MODULE_LICENSE("GPL");
