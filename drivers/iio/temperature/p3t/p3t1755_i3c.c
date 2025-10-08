// SPDX-License-Identifier: GPL-2.0
/*
 * NXP P3T175x Temperature Sensor Driver
 *
 * Copyright 2025 NXP
 */
#include <linux/i3c/device.h>
#include <linux/module.h>
#include <linux/mod_devicetable.h>
#include <linux/regmap.h>

#include <linux/iio/iio.h>
#include <linux/iio/events.h>

#include "p3t1755.h"

static const struct regmap_config p3t1755_i3c_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
};

static void p3t1755_ibi_handler(struct i3c_device *dev,
				const struct i3c_ibi_payload *payload)
{
	struct iio_dev *indio_dev = dev_get_drvdata(&dev->dev);

	p3t1755_push_thresh_event(indio_dev);
}

/*
 * Both P3T1755 and P3T1750 share the same I3C PID (0x011B:0x152A),
 * making runtime differentiation impossible, so using "p3t1755" as
 * name in sysfs and IIO for I3C based instances.
 */
static const struct i3c_device_id p3t1755_i3c_ids[] = {
	I3C_DEVICE(0x011B, 0x152A, &p3t1755_channels_info),
	{ }
};
MODULE_DEVICE_TABLE(i3c, p3t1755_i3c_ids);

static void p3t1755_disable_ibi(void *data)
{
	i3c_device_disable_ibi(data);
}

static void p3t1755_free_ibi(void *data)
{
	i3c_device_free_ibi(data);
}

static int p3t1755_i3c_probe(struct i3c_device *i3cdev)
{
	const struct i3c_device_id *id = i3c_device_match_id(i3cdev, p3t1755_i3c_ids);
	const struct p3t1755_info *chip;
	struct device *dev = &i3cdev->dev;
	struct i3c_ibi_setup ibi_setup;
	struct regmap *regmap;
	int ret;

	chip = id ? id->data : NULL;

	regmap = devm_regmap_init_i3c(i3cdev, &p3t1755_i3c_regmap_config);
	if (IS_ERR(regmap))
		return dev_err_probe(&i3cdev->dev, PTR_ERR(regmap),
				     "Failed to register I3C regmap %ld\n", PTR_ERR(regmap));

	ret = p3t1755_probe(dev, chip, regmap, 0);
	if (ret)
		return dev_err_probe(dev, ret, "p3t175x probe failed: %d\n", ret);

	ibi_setup = (struct i3c_ibi_setup) {
		.handler = p3t1755_ibi_handler,
		.num_slots = 4,
		.max_payload_len = 0,
	};

	ret = i3c_device_request_ibi(i3cdev, &ibi_setup);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to request IBI\n");

	ret = devm_add_action_or_reset(dev, p3t1755_free_ibi, i3cdev);
	if (ret)
		return ret;

	ret = i3c_device_enable_ibi(i3cdev);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to enable IBI\n");

	ret = devm_add_action_or_reset(dev, p3t1755_disable_ibi, i3cdev);
	if (ret)
		return ret;

	return 0;
}

static struct i3c_driver p3t1755_driver = {
	.driver = {
		.name = "p3t1755_i3c",
	},
	.probe = p3t1755_i3c_probe,
	.id_table = p3t1755_i3c_ids,
};
module_i3c_driver(p3t1755_driver);

MODULE_AUTHOR("Lakshay Piplani <lakshay.piplani@nxp.com>");
MODULE_DESCRIPTION("NXP P3T1750/P3T1755 I3C Driver");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS(IIO_P3T1755);
