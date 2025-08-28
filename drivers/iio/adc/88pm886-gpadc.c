// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2025, Duje Mihanović <duje@dujemihanovic.xyz>
 */

#include <linux/device.h>
#include <linux/i2c.h>
#include <linux/iio/driver.h>
#include <linux/iio/iio.h>
#include <linux/iio/types.h>
#include <linux/kernel.h>
#include <linux/mod_devicetable.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>

#include <linux/mfd/88pm886.h>

static const int regs[] = {
	PM886_REG_GPADC_VSC,
	PM886_REG_GPADC_VCHG_PWR,
	PM886_REG_GPADC_VCF_OUT,
	PM886_REG_GPADC_TINT,

	PM886_REG_GPADC_GPADC0,
	PM886_REG_GPADC_GPADC1,
	PM886_REG_GPADC_GPADC2,

	PM886_REG_GPADC_VBAT,
	PM886_REG_GPADC_GNDDET1,
	PM886_REG_GPADC_GNDDET2,
	PM886_REG_GPADC_VBUS,
	PM886_REG_GPADC_GPADC3,

	PM886_REG_GPADC_MIC_DET,
	PM886_REG_GPADC_VBAT_SLP,
};

enum pm886_gpadc_channel {
	VSC_CHAN,
	VCHG_PWR_CHAN,
	VCF_OUT_CHAN,
	TINT_CHAN,

	GPADC0_CHAN,
	GPADC1_CHAN,
	GPADC2_CHAN,

	VBAT_CHAN,
	GNDDET1_CHAN,
	GNDDET2_CHAN,
	VBUS_CHAN,
	GPADC3_CHAN,

	MIC_DET_CHAN,
	VBAT_SLP_CHAN,

	GPADC0_RES_CHAN,
	GPADC1_RES_CHAN,
	GPADC2_RES_CHAN,
	GPADC3_RES_CHAN,
};

#define ADC_CHANNEL(index, lsb, _type, name) {	\
	.type = _type, \
	.indexed = 1, \
	.channel = index, \
	.address = lsb, \
	.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) | \
			      BIT(IIO_CHAN_INFO_PROCESSED), \
	.datasheet_name = name, \
}

static const struct iio_chan_spec pm886_adc_channels[] = {
	ADC_CHANNEL(VSC_CHAN, 1367, IIO_VOLTAGE, "vsc"),
	ADC_CHANNEL(VCHG_PWR_CHAN, 1709, IIO_VOLTAGE, "vchg_pwr"),
	ADC_CHANNEL(VCF_OUT_CHAN, 1367, IIO_VOLTAGE, "vcf_out"),
	ADC_CHANNEL(TINT_CHAN, 104, IIO_TEMP, "tint"),

	ADC_CHANNEL(GPADC0_CHAN, 342, IIO_VOLTAGE, "gpadc0"),
	ADC_CHANNEL(GPADC1_CHAN, 342, IIO_VOLTAGE, "gpadc1"),
	ADC_CHANNEL(GPADC2_CHAN, 342, IIO_VOLTAGE, "gpadc2"),

	ADC_CHANNEL(VBAT_CHAN, 1367, IIO_VOLTAGE, "vbat"),
	ADC_CHANNEL(GNDDET1_CHAN, 342, IIO_VOLTAGE, "gnddet1"),
	ADC_CHANNEL(GNDDET2_CHAN, 342, IIO_VOLTAGE, "gnddet2"),
	ADC_CHANNEL(VBUS_CHAN, 1709, IIO_VOLTAGE, "vbus"),
	ADC_CHANNEL(GPADC3_CHAN, 342, IIO_VOLTAGE, "gpadc3"),
	ADC_CHANNEL(MIC_DET_CHAN, 1367, IIO_VOLTAGE, "mic_det"),
	ADC_CHANNEL(VBAT_SLP_CHAN, 1367, IIO_VOLTAGE, "vbat_slp"),

	ADC_CHANNEL(GPADC0_RES_CHAN, 342, IIO_RESISTANCE, "gpadc0_res"),
	ADC_CHANNEL(GPADC1_RES_CHAN, 342, IIO_RESISTANCE, "gpadc1_res"),
	ADC_CHANNEL(GPADC2_RES_CHAN, 342, IIO_RESISTANCE, "gpadc2_res"),
	ADC_CHANNEL(GPADC3_RES_CHAN, 342, IIO_RESISTANCE, "gpadc3_res"),
};

static const struct regmap_config pm886_gpadc_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = PM886_REG_GPADC_VBAT_SLP + 1,
};

static int gpadc_get_raw(struct iio_dev *iio, enum pm886_gpadc_channel chan)
{
	struct regmap **map = iio_priv(iio);
	int val, ret;
	u8 buf[2];

	if (chan >= GPADC0_RES_CHAN)
		/* Resistor voltage drops are read from the corresponding voltage channel */
		chan -= GPADC0_RES_CHAN - GPADC0_CHAN;

	ret = regmap_bulk_read(*map, regs[chan], buf, 2);

	if (ret)
		return ret;

	val = ((buf[0] & 0xff) << 4) | (buf[1] & 0xf);
	val &= 0xfff;

	return val;
}

static int gpadc_enable_bias(struct regmap *map, enum pm886_gpadc_channel chan)
{
	int adcnum = chan - GPADC0_RES_CHAN, bits;

	if (adcnum < 0 || adcnum > 3)
		return -EINVAL;

	bits = BIT(adcnum + 4) | BIT(adcnum);

	return regmap_set_bits(map, PM886_REG_GPADC_CONFIG20, bits);
}

static int
gpadc_find_bias_current(struct iio_dev *iio, struct iio_chan_spec const *chan, int *volt,
			int *amp)
{
	struct regmap **map = iio_priv(iio);
	int adcnum = chan->channel - GPADC0_RES_CHAN;
	int reg = PM886_REG_GPADC_CONFIG11 + adcnum;
	int ret;

	for (int i = 0; i < 16; i++) {
		ret = regmap_update_bits(*map, reg, 0xf, i);
		if (ret)
			return ret;

		usleep_range(5000, 10000);

		*amp = 1 + i * 5;
		*volt = gpadc_get_raw(iio, chan->channel) * chan->address;

		/* Measured voltage should never exceed 1.25V */
		if (WARN_ON(*volt > 1250000))
			return -EIO;

		if (*volt < 300000) {
			dev_dbg(&iio->dev, "bad bias for chan %d: %duA @ %duV\n", chan->channel,
				*amp, *volt);
		} else {
			dev_dbg(&iio->dev, "good bias for chan %d: %duA @ %duV\n", chan->channel,
				*amp, *volt);
			return 0;
		}
	}

	dev_err(&iio->dev, "failed to find good bias for chan %d\n", chan->channel);
	return -EINVAL;
}

static int
gpadc_get_resistor(struct iio_dev *iio, struct iio_chan_spec const *chan)
{
	struct regmap **map = iio_priv(iio);
	int ret, volt, amp;

	ret = gpadc_enable_bias(*map, chan->channel);
	if (ret)
		return ret;

	ret = gpadc_find_bias_current(iio, chan, &volt, &amp);
	if (ret)
		return ret;

	return DIV_ROUND_CLOSEST(volt, amp);
}

static int
pm886_gpadc_read_raw(struct iio_dev *iio, struct iio_chan_spec const *chan, int *val, int *val2,
		     long mask)
{
	struct device *dev = iio->dev.parent;
	int raw, ret;

	ret = pm_runtime_resume_and_get(dev);
	if (ret)
		return ret;

	if (chan->type == IIO_RESISTANCE) {
		raw = gpadc_get_resistor(iio, chan);
		if (raw < 0) {
			ret = raw;
			goto out;
		}

		*val = raw;
		dev_dbg(&iio->dev, "chan: %d, %d Ohm\n", chan->channel, *val);
		ret = IIO_VAL_INT;
		goto out;
	}

	raw = gpadc_get_raw(iio, chan->channel);
	if (raw < 0) {
		ret = raw;
		goto out;
	}

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		*val = raw;
		dev_dbg(&iio->dev, "chan: %d, raw: %d\n", chan->channel, *val);
		ret = IIO_VAL_INT;
		break;
	case IIO_CHAN_INFO_PROCESSED: {
		*val = raw * chan->address;
		ret = IIO_VAL_INT;

		/*
		 * Voltage measurements are scaled into uV. Scale them back
		 * into the mV dimension.
		 */
		if (chan->type == IIO_VOLTAGE)
			*val = DIV_ROUND_CLOSEST(*val, 1000);

		dev_dbg(&iio->dev, "chan: %d, raw: %d, processed: %d\n", chan->channel, raw, *val);
		break;
	default:
		ret = -EINVAL;
	}
	}

out:
	pm_runtime_mark_last_busy(dev);
	pm_runtime_put_autosuspend(dev);
	return ret;
}

static int pm886_gpadc_setup(struct regmap *map, bool enable)
{
	const u8 config[] = {0xff, 0xfd, 0x1};
	int ret;

	/* Enable/disable the ADC block */
	ret = regmap_assign_bits(map, PM886_REG_GPADC_CONFIG6, BIT(0), enable);
	if (ret || !enable)
		return ret;

	/* If enabling, enable each individual ADC */
	return regmap_bulk_write(map, PM886_REG_GPADC_CONFIG1, config, ARRAY_SIZE(config));
}

static const struct iio_info pm886_gpadc_iio_info = {
	.read_raw = pm886_gpadc_read_raw,
};

static int pm886_gpadc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev, *parent = dev->parent;
	struct pm886_chip *chip = dev_get_drvdata(parent);
	struct i2c_client *client = chip->client, *page;
	struct regmap **map;
	struct iio_dev *iio;
	int ret;

	iio = devm_iio_device_alloc(dev, sizeof(*map));
	if (!iio)
		return -ENOMEM;
	map = iio_priv(iio);

	dev_set_drvdata(dev, iio);

	page = devm_i2c_new_dummy_device(dev, client->adapter,
					 client->addr + PM886_PAGE_OFFSET_GPADC);
	if (IS_ERR(page))
		return dev_err_probe(dev, PTR_ERR(page), "Failed to initialize GPADC page\n");

	*map = devm_regmap_init_i2c(page, &pm886_gpadc_regmap_config);
	if (IS_ERR(*map))
		return dev_err_probe(dev, PTR_ERR(*map),
				     "Failed to initialize GPADC regmap\n");

	iio->name = "88pm886-gpadc";
	iio->dev.parent = dev;
	iio->dev.of_node = parent->of_node;
	iio->modes = INDIO_DIRECT_MODE;
	iio->info = &pm886_gpadc_iio_info;
	iio->channels = pm886_adc_channels;
	iio->num_channels = ARRAY_SIZE(pm886_adc_channels);

	devm_pm_runtime_enable(dev);
	pm_runtime_set_autosuspend_delay(dev, 50);
	pm_runtime_use_autosuspend(dev);

	ret = devm_iio_device_register(dev, iio);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to register ADC\n");

	return 0;
}

static int pm886_gpadc_runtime_resume(struct device *dev)
{
	struct iio_dev *iio = dev_get_drvdata(dev);
	struct regmap **map = iio_priv(iio);

	return pm886_gpadc_setup(*map, true);
}

static int pm886_gpadc_runtime_suspend(struct device *dev)
{
	struct iio_dev *iio = dev_get_drvdata(dev);
	struct regmap **map = iio_priv(iio);

	return pm886_gpadc_setup(*map, false);
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
