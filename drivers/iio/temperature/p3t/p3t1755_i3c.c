// SPDX-License-Identifier: GPL-2.0
/*
 * NXP P3T175x Temperature Sensor Driver
 *
 * Copyright 2025 NXP
 */
#include <linux/kernel.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/i3c/device.h>
#include <linux/i3c/master.h>
#include <linux/slab.h>
#include <linux/regmap.h>
#include <linux/of.h>
#include <linux/iio/iio.h>
#include <linux/iio/events.h>

#include "p3t1755.h"

static void p3t1755_ibi_handler(struct i3c_device *dev,
				const struct i3c_ibi_payload *payload)
{
	struct iio_dev *indio_dev = dev_get_drvdata(&dev->dev);

	dev_dbg(&dev->dev, "IBI received, handling threshold event\n");

	// Handle threshold event via helper
	p3t1755_push_thresh_event(indio_dev);
}

/*
 * Both P3T1755 and P3T1750 share the same I3C
 * PID (0x011B:0x152A), making runtime differentiation
 * impossible, so a common "p3t175x" name in sysfs
 * and IIO for I3C based instances.
 */
static const struct i3c_device_id p3t1755_i3c_ids[] = {
	I3C_DEVICE(0x011B, 0x152A, (void *)&p3t175x_channels_info),
	{ /* sentinel */ },
};

MODULE_DEVICE_TABLE(i3c, p3t1755_i3c_ids);

static int p3t1755_i3c_probe(struct i3c_device *i3cdev)
{
	const struct regmap_config p3t1755_i3c_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	};

	const struct i3c_device_id *id = i3c_device_match_id(i3cdev, p3t1755_i3c_ids);
	const struct p3t17xx_info *chip = &p3t175x_channels_info;
	struct device_node *np = i3cdev->dev.of_node;
	bool alert_active_high = false;
	struct i3c_ibi_setup ibi_setup;
	struct regmap *regmap;
	bool tm_mode = false;
	int fq_bits = -1;
	int ret;

	regmap = devm_regmap_init_i3c(i3cdev, &p3t1755_i3c_regmap_config);
	if (IS_ERR(regmap)) {
		dev_err_probe(&i3cdev->dev, PTR_ERR(regmap),
			      "Failed to register I3C regmap %ld\n", PTR_ERR(regmap));
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
				dev_err_probe(&i3cdev->dev, fq_bits,
					      "invalid nxp,fault-queue %u (1/2/4/6)\n", fq);
				return fq_bits;
			}
		}
	}

	dev_info(&i3cdev->dev, "Using TM mode: %s\n", tm_mode ? "Interrupt" : "Comparator");
	dev_info(&i3cdev->dev, "Alert polarity: %s\n",
		 alert_active_high ? "Active-High" : "Active-Low");

	if (id && id->data)
		chip = (const struct p3t17xx_info *)id->data;

	ret = p3t1755_probe(&i3cdev->dev, chip, regmap, tm_mode, alert_active_high, fq_bits);
	if (ret) {
		dev_err_probe(&i3cdev->dev, ret, "p3t175x probe failed: %d\n", ret);
		return ret;
	}

	if (!tm_mode) {
		dev_warn(&i3cdev->dev, "IBI not supported in comparator mode, skipping IBI registration\n");
		return 0;
	}

	ibi_setup.handler = p3t1755_ibi_handler;
	ibi_setup.num_slots = 4;
	ibi_setup.max_payload_len = 0;

	ret = i3c_device_request_ibi(i3cdev, &ibi_setup);
	if (ret) {
		dev_err_probe(&i3cdev->dev, ret, "Failed to request IBI: %d\n", ret);
		return ret;
	}

	ret = i3c_device_enable_ibi(i3cdev);
	if (ret) {
		dev_err_probe(&i3cdev->dev, ret, "Failed to enable IBI: %d\n", ret);
		i3c_device_free_ibi(i3cdev);
		return ret;
	}

	dev_info(&i3cdev->dev, "IBI successfully registered\n");
	return 0;
}

static void p3t1755_i3c_remove(struct i3c_device *i3cdev)
{
	/* Unwind IBI registration to ensure clean shutdown */
	i3c_device_disable_ibi(i3cdev);
	i3c_device_free_ibi(i3cdev);
}

static struct i3c_driver p3t1755_driver = {
	.driver = {
		.name = "p3t175x_i3c",
	},
	.probe = p3t1755_i3c_probe,
	.remove = p3t1755_i3c_remove,
	.id_table = p3t1755_i3c_ids,
};
module_i3c_driver(p3t1755_driver);

MODULE_AUTHOR("Lakshay Piplani <lakshay.piplani@nxp.com>");
MODULE_DESCRIPTION("NXP P3T175x I3C Driver");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS(IIO_P3T1755);
