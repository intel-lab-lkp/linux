// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2025 Suraj Sonawane <surajsonawane0215@gmail.com>
 * Sharp GP2Y1010AU0F Dust Sensor Driver
 * Datasheet: https://global.sharp/products/device/lineup/data/pdf/datasheet/gp2y1010au_appl_e.pdf
 */

#include <linux/delay.h>
#include <linux/err.h>
#include <linux/gpio/consumer.h>
#include <linux/iio/consumer.h>
#include <linux/iio/iio.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regulator/consumer.h>

/* Timings based on GP2Y1010AU0F datasheet Section 6-1 */
#define GP2Y1010_LED_PULSE_US     320  /* Total LED ON time (0.32 ms) */
#define GP2Y1010_SAMPLE_DELAY_US  280  /* ADC sampling after LED ON (0.28 ms) */

struct gp2y1010_data {
	struct gpio_desc *led_gpio;
	struct iio_channel *adc_chan;
	int v_clean;  /* Calibration: voltage in clean air (mV) */
};

static int gp2y1010_read_raw(struct iio_dev *indio_dev,
							 struct iio_chan_spec const *chan,
							 int *val, int *val2, long mask)
{
	struct gp2y1010_data *data = iio_priv(indio_dev);
	int ret, voltage_mv;

	if (mask != IIO_CHAN_INFO_RAW)
		return -EINVAL;

	gpiod_set_value(data->led_gpio, 1);
	udelay(GP2Y1010_SAMPLE_DELAY_US);

	ret = iio_read_channel_processed(data->adc_chan, &voltage_mv);

	/* Wait remaining time to complete 320 µs total LED pulse width */
	udelay(GP2Y1010_LED_PULSE_US - GP2Y1010_SAMPLE_DELAY_US);
	gpiod_set_value(data->led_gpio, 0);

	if (ret < 0)
		return ret;

	*val = voltage_mv;
	return IIO_VAL_INT;
}

static const struct iio_info gp2y1010_info = {
	.read_raw = gp2y1010_read_raw,
};

static const struct iio_chan_spec gp2y1010_channels[] = {
	{
		.type = IIO_DENSITY,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),
	},
};

static int gp2y1010_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct iio_dev *indio_dev;
	struct gp2y1010_data *data;
	enum iio_chan_type ch_type;
	int ret;

	indio_dev = devm_iio_device_alloc(dev, sizeof(*data));
	if (!indio_dev)
		return -ENOMEM;

	data = iio_priv(indio_dev);
	data->v_clean = 900;

	data->led_gpio = devm_gpiod_get(dev, "led", GPIOD_OUT_LOW);
	if (IS_ERR(data->led_gpio))
		return dev_err_probe(dev, PTR_ERR(data->led_gpio), "Failed to get LED GPIO\n");

	ret = devm_regulator_get_enable(dev, "vdd");
	if (ret)
		return ret;
	udelay(100);

	data->adc_chan = devm_iio_channel_get(dev, "dust");
	if (IS_ERR(data->adc_chan))
		return dev_err_probe(dev, PTR_ERR(data->adc_chan), "Failed to get ADC channel\n");

	ret = iio_get_channel_type(data->adc_chan, &ch_type);
	if (ret < 0)
		return ret;
	if (ch_type != IIO_DENSITY)
		return dev_err_probe(dev, -EINVAL, "ADC channel is not density type\n");

	indio_dev->name = dev_name(dev);
	indio_dev->info = &gp2y1010_info;
	indio_dev->channels = gp2y1010_channels;
	indio_dev->num_channels = ARRAY_SIZE(gp2y1010_channels);
	indio_dev->modes = INDIO_DIRECT_MODE;

	return devm_iio_device_register(dev, indio_dev);
}

static const struct of_device_id gp2y1010_of_match[] = {
	{ .compatible = "sharp,gp2y1010au0f" },
	{ }
};
MODULE_DEVICE_TABLE(of, gp2y1010_of_match);

static struct platform_driver gp2y1010_driver = {
	.driver = {
		.name = "gp2y1010",
		.of_match_table = gp2y1010_of_match,
	},
	.probe = gp2y1010_probe,
};
module_platform_driver(gp2y1010_driver);

MODULE_AUTHOR("Suraj Sonawane <surajsonawane0215@gmail.com>");
MODULE_DESCRIPTION("Sharp GP2Y1010AU0F Dust Sensor Driver");
MODULE_LICENSE("GPL");
