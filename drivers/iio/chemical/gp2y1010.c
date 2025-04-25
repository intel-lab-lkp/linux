// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2025 Suraj Sonawane <surajsonawane0215@gmail.com>
 *
 * Sharp GP2Y1010AU0F Dust Sensor Driver
 *
 * Datasheet:
 * https://global.sharp/products/device/lineup/data/pdf/datasheet/gp2y1010au_appl_e.pdf
 *
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/iio/iio.h>
#include <linux/iio/sysfs.h>
#include <linux/iio/consumer.h>
#include <linux/of_device.h>
#include <linux/mod_devicetable.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/err.h>
#include <linux/regulator/consumer.h>
#include <linux/gpio/consumer.h>
#include <linux/delay.h>

#define GP2Y1010_LED_PULSE_US    280  /* LED on-time (280us) */
#define GP2Y1010_SAMPLE_DELAY_US  40  /* Wait 40us after LED on */
#define GP2Y1010_MEASUREMENT_US  200  /* Measure 200us after LED on */

struct gp2y1010_data {
	struct gpio_desc *led_gpio;
	struct iio_dev *indio_dev;
	struct iio_channel *adc_chan;
	struct regulator *vdd;
	int v_clean;  /* Calibration: voltage in clean air (mV) */
};

static int gp2y1010_power_on(struct gp2y1010_data *data)
{
	int ret;

	ret = regulator_enable(data->vdd);
	if (ret) {
		dev_err(&data->indio_dev->dev, "Failed to enable vdd regulator\n");
		return ret;
	}

	udelay(100); /* Short delay for regulator stability */
	return 0;
}

static void gp2y1010_power_off(struct gp2y1010_data *data)
{
	regulator_disable(data->vdd);
}

static int gp2y1010_read_raw(struct iio_dev *indio_dev,
							 struct iio_chan_spec const *chan,
							 int *val, int *val2, long mask)
{
	struct gp2y1010_data *data = iio_priv(indio_dev);
	int ret, voltage_mv;

	if (mask != IIO_CHAN_INFO_RAW)
		return -EINVAL;

    /* Turn on LED */
	gpiod_set_value(data->led_gpio, 1);

    /* Wait 40us (datasheet: delay after LED on) */
	udelay(GP2Y1010_SAMPLE_DELAY_US);

    /* Read ADC at 200us (peak sensitivity) */
	udelay(GP2Y1010_MEASUREMENT_US - GP2Y1010_SAMPLE_DELAY_US);
	ret = iio_read_channel_processed(data->adc_chan, &voltage_mv);
	if (ret < 0) {
		gpiod_set_value(data->led_gpio, 0);
		return ret;
	}

    /* Turn off LED (total pulse width = 280us) */
	gpiod_set_value(data->led_gpio, 0);

    /* Store raw voltage (for debugging) */
	*val = voltage_mv;

	return IIO_VAL_INT;
}

static const struct iio_info gp2y1010_info = {
	.read_raw = gp2y1010_read_raw,
};

static const struct iio_chan_spec gp2y1010_channels[] = {
	{
		.type = IIO_VOLTAGE,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),
		.extend_name = "dust",
	},
};

static int gp2y1010_probe(struct platform_device *pdev)
{
	struct gp2y1010_data *data;
	struct iio_dev *indio_dev;
	int ret;

	indio_dev = devm_iio_device_alloc(&pdev->dev, sizeof(*data));
	if (!indio_dev)
		return -ENOMEM;

	data = iio_priv(indio_dev);
	data->indio_dev = indio_dev;
	data->v_clean = 900;  /* Default calibration (adjust per sensor) */

	/* Get LED GPIO */
	data->led_gpio = devm_gpiod_get(&pdev->dev, "led", GPIOD_OUT_LOW);
	if (IS_ERR(data->led_gpio))
		return dev_err_probe(&pdev->dev, PTR_ERR(data->led_gpio),
							"Failed to get LED GPIO\n");

    /* Get regulator */
	data->vdd = devm_regulator_get(&pdev->dev, "vdd");
	if (IS_ERR(data->vdd))
		return dev_err_probe(&pdev->dev, PTR_ERR(data->vdd),
							"Failed to get regulator\n");

    /* Power on sensor */
	ret = gp2y1010_power_on(data);
	if (ret)
		return ret;

    /* Get ADC channel */
	data->adc_chan = devm_iio_channel_get(&pdev->dev, "dust");
	if (IS_ERR(data->adc_chan)) {
		gp2y1010_power_off(data);
		return dev_err_probe(&pdev->dev, PTR_ERR(data->adc_chan),
							"Failed to get ADC channel\n");
	}

	/* Setup IIO device */
	indio_dev->name = "gp2y1010";
	indio_dev->info = &gp2y1010_info;
	indio_dev->channels = gp2y1010_channels;
	indio_dev->num_channels = ARRAY_SIZE(gp2y1010_channels);
	indio_dev->modes = INDIO_DIRECT_MODE;

	ret = devm_iio_device_register(&pdev->dev, indio_dev);
	if (ret) {
		gp2y1010_power_off(data);
		return ret;
	}

	return 0;
}

static void gp2y1010_remove(struct platform_device *pdev)
{
	struct gp2y1010_data *data = platform_get_drvdata(pdev);

	gp2y1010_power_off(data);
}

static const struct of_device_id gp2y1010_of_match[] = {
	{ .compatible = "sharp,gp2y1010au0f", },
	{}
};
MODULE_DEVICE_TABLE(of, gp2y1010_of_match);

static struct platform_driver gp2y1010_driver = {
	.driver = {
		.name = "gp2y1010",
		.of_match_table = gp2y1010_of_match,
	},
	.probe = gp2y1010_probe,
	.remove = gp2y1010_remove,
};

module_platform_driver(gp2y1010_driver);

MODULE_AUTHOR("Suraj Sonawane <surajsonawane0215@gmail.com>");
MODULE_DESCRIPTION("Sharp GP2Y1010AU0F Dust Sensor Driver");
MODULE_LICENSE("GPL");
