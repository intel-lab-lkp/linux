// SPDX-License-Identifier: GPL-2.0-only
/*
 * Input driver for resistor ladder connected on ADC
 *
 * Copyright (c) 2016 Alexandre Belloni
 */

#include <linux/err.h>
#include <linux/iio/consumer.h>
#include <linux/iio/types.h>
#include <linux/input.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/slab.h>

struct adc_keys_button {
	u32 voltage;
	u32 code;
	u32 type;
};

struct adc_keys_state {
	struct iio_channel *channel;
	u32 num_keys;
	u32 last_key;
	u32 last_type;
	u32 keyup_voltage;
	const struct adc_keys_button *map;
};

static void adc_keys_poll(struct input_dev *input)
{
	struct adc_keys_state *st = input_get_drvdata(input);
	int i, value, ret;
	u32 diff, closest = 0xffffffff;
	u32 code = 0;
	u32 type = EV_KEY;

	ret = iio_read_channel_processed(st->channel, &value);
	if (unlikely(ret < 0)) {
		/* Forcibly release key if any was pressed */
		value = st->keyup_voltage;
	} else {
		for (i = 0; i < st->num_keys; i++) {
			diff = abs(st->map[i].voltage - value);
			if (diff < closest) {
				closest = diff;
				code = st->map[i].code;
				type = st->map[i].type;
			}
		}
	}

	if (abs(st->keyup_voltage - value) < closest)
		code = 0;

	if (st->last_key && st->last_key != code)
		input_event(input, st->last_type, st->last_key, 0);

	if (code)
		input_event(input, type, code, 1);

	input_sync(input);
	st->last_key = code;
	st->last_type = type;
}

static int adc_keys_load_keymap(struct device *dev, struct adc_keys_state *st)
{
	struct adc_keys_button *map;
	int i;

	st->num_keys = device_get_child_node_count(dev);
	if (!st->num_keys)
		return dev_err_probe(dev, -EINVAL, "keymap is missing\n");

	map = devm_kmalloc_array(dev, st->num_keys, sizeof(*map), GFP_KERNEL);
	if (!map)
		return -ENOMEM;

	i = 0;
	device_for_each_child_node_scoped(dev, child) {
		if (fwnode_property_read_u32(child, "press-threshold-microvolt",
					     &map[i].voltage))
			return dev_err_probe(dev, -EINVAL,
					     "Key with invalid or missing voltage\n");

		map[i].voltage /= 1000;

		if (fwnode_property_read_u32(child, "linux,code",
					     &map[i].code))
			return dev_err_probe(dev, -EINVAL,
					     "Key with invalid or missing linux,code\n");

		if (fwnode_property_read_u32(child, "linux,input-type",
					     &map[i].type))
			map[i].type = EV_KEY;

		if (map[i].type != EV_KEY && map[i].type != EV_SW)
			return dev_err_probe(dev, -EINVAL,
					     "Invalid linux,input-type: 0x%x\n",
					     map[i].type);

		i++;
	}

	st->map = map;
	return 0;
}

static int adc_keys_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct adc_keys_state *st;
	struct input_dev *input;
	enum iio_chan_type type;
	int i, value;
	int error;

	st = devm_kzalloc(dev, sizeof(*st), GFP_KERNEL);
	if (!st)
		return -ENOMEM;

	st->channel = devm_iio_channel_get(dev, "buttons");
	if (IS_ERR(st->channel))
		return dev_err_probe(dev, PTR_ERR(st->channel),
				     "Could not get iio channel\n");

	if (!st->channel->indio_dev)
		return -ENXIO;

	error = iio_get_channel_type(st->channel, &type);
	if (error < 0)
		return error;

	if (type != IIO_VOLTAGE)
		return dev_err_probe(dev, -EINVAL, "Incompatible channel type %d\n", type);

	if (device_property_read_u32(dev, "keyup-threshold-microvolt",
				     &st->keyup_voltage))
		return dev_err_probe(dev, -EINVAL, "Invalid or missing keyup voltage\n");

	st->keyup_voltage /= 1000;

	error = adc_keys_load_keymap(dev, st);
	if (error)
		return error;

	input = devm_input_allocate_device(dev);
	if (!input)
		return -ENOMEM;

	input_set_drvdata(input, st);

	input->name = pdev->name;
	input->phys = "adc-keys/input0";

	input->id.bustype = BUS_HOST;
	input->id.vendor = 0x0001;
	input->id.product = 0x0001;
	input->id.version = 0x0100;

	for (i = 0; i < st->num_keys; i++)
		input_set_capability(input, st->map[i].type, st->map[i].code);

	if (device_property_read_bool(dev, "autorepeat"))
		__set_bit(EV_REP, input->evbit);


	error = input_setup_polling(input, adc_keys_poll);
	if (error)
		return dev_err_probe(dev, error, "Unable to set up polling\n");

	if (!device_property_read_u32(dev, "poll-interval", &value))
		input_set_poll_interval(input, value);

	error = input_register_device(input);
	if (error)
		return dev_err_probe(dev, error, "Unable to register input device\n");

	return 0;
}

#ifdef CONFIG_OF
static const struct of_device_id adc_keys_of_match[] = {
	{ .compatible = "adc-keys", },
	{ }
};
MODULE_DEVICE_TABLE(of, adc_keys_of_match);
#endif

static struct platform_driver adc_keys_driver = {
	.driver = {
		.name = "adc_keys",
		.of_match_table = of_match_ptr(adc_keys_of_match),
	},
	.probe = adc_keys_probe,
};
module_platform_driver(adc_keys_driver);

MODULE_AUTHOR("Alexandre Belloni <alexandre.belloni@free-electrons.com>");
MODULE_DESCRIPTION("Input driver for resistor ladder connected on ADC");
MODULE_LICENSE("GPL v2");
