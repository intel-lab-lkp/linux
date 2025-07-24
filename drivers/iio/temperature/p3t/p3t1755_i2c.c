// SPDX-License-Identifier: GPL-2.0-only
/*
 * NXP P3T175x Temperature Sensor Driver
 *
 * Copyright 2025 NXP
 */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/slab.h>
#include <linux/regmap.h>
#include <linux/of.h>
#include <linux/iio/iio.h>
#include <linux/iio/events.h>

#include "p3t1755.h"

static const struct regmap_config p3t1755_i2c_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
};

static irqreturn_t p3t1755_irq_handler(int irq, void *dev_id)
{
	struct iio_dev *indio_dev = dev_id;

	dev_dbg(&indio_dev->dev, "IRQ triggered, processing threshold event\n");

	// Handle threshold event via helper
	p3t1755_push_thresh_event(indio_dev);

	return IRQ_HANDLED;
}

static const struct of_device_id p3t1755_i2c_of_match[] = {
	{ .compatible = "nxp,p3t1755", .data = &p3t1755_channels_info },
	{ .compatible = "nxp,p3t1750", .data = &p3t1750_channels_info },
	{ }
};
MODULE_DEVICE_TABLE(of, p3t1755_i2c_of_match);

static const struct i2c_device_id p3t1755_i2c_id_table[] = {
	{ "p3t1755", (kernel_ulong_t)&p3t1755_channels_info },
	{ "p3t1750", (kernel_ulong_t)&p3t1750_channels_info},
	{ }
};
MODULE_DEVICE_TABLE(i2c, p3t1755_i2c_id_table);

static int p3t1755_i2c_probe(struct i2c_client *client)
{
	struct device_node *np = client->dev.of_node;
	bool alert_active_high = false;
	const struct p3t17xx_info *chip;
	struct p3t1755_data *data;
	struct iio_dev *iio_dev;
	unsigned long irq_flags;
	struct regmap *regmap;
	bool tm_mode = false;
	int fq_bits = -1;
	int ret;

	regmap = devm_regmap_init_i2c(client, &p3t1755_i2c_regmap_config);
	if (IS_ERR(regmap)) {
		dev_err_probe(&client->dev, PTR_ERR(regmap),
			      "Failed to register i2c regmap %ld\n", PTR_ERR(regmap));
		return PTR_ERR(regmap);
	}

	/* Parse optional device tree property for alert polarity */
	alert_active_high = of_property_read_bool(np, "nxp,alert-active-high");

	/* Parse optional device tree property for thermostat mode */
	tm_mode = of_property_read_bool(np, "nxp,interrupt-mode");

	/* Optional fault queue length */
	if (np) {
		u32 fq;

		if (!of_property_read_u32(np, "nxp,fault-queue", &fq)) {
			fq_bits = p3t1755_fault_queue_to_bits(fq);
			if (fq_bits < 0) {
				dev_err_probe(&client->dev, fq_bits,
					      "invalid nxp,fault-queue %u (1/2/4/6)\n", fq);
				return fq_bits;
			}
		}
	}

	dev_info(&client->dev, "Using TM mode: %s\n",
		 tm_mode ? "Interrupt" : "Comparator");
	dev_info(&client->dev, "Alert polarity: %s\n",
		 alert_active_high ? "Active-High" : "Active-Low");

	chip = device_get_match_data(&client->dev);
	if (!chip)
		chip = (const struct p3t17xx_info *)i2c_match_id(p3t1755_i2c_id_table,
			client)->driver_data;

	dev_info(&client->dev, "Registering p3t175x temperature sensor");

	ret = p3t1755_probe(&client->dev, chip, regmap,
			    tm_mode, alert_active_high, fq_bits);

	if (ret) {
		dev_err_probe(&client->dev, ret, "p3t175x probe failed: %d\n", ret);
		return ret;
	}

	/* Setup IRQ if available */
	if (client->irq > 0) {
		iio_dev = dev_get_drvdata(&client->dev);
		data = iio_priv(iio_dev);

		if (tm_mode)
			irq_flags = alert_active_high ? IRQF_TRIGGER_RISING : IRQF_TRIGGER_FALLING;
		else
			irq_flags = (IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING);

		ret = devm_request_threaded_irq(&client->dev, client->irq, NULL,
						p3t1755_irq_handler, irq_flags | IRQF_ONESHOT,
						"p3t175x", iio_dev);
		if (ret)
			dev_err_probe(&client->dev, ret, "Failed to request IRQ: %d\n", ret);
		}

		return ret;
}

static struct i2c_driver p3t1755_driver = {
	.driver = {
		.name = "p3t175x_i2c",
		.of_match_table = p3t1755_i2c_of_match,
	},
	.probe = p3t1755_i2c_probe,
	.id_table = p3t1755_i2c_id_table,
};
module_i2c_driver(p3t1755_driver);

MODULE_AUTHOR("Lakshay Piplani <lakshay.piplani@nxp.com>");
MODULE_DESCRIPTION("NXP P3T175x I2C Driver");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS(IIO_P3T1755);
