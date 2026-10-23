// SPDX-License-Identifier: GPL-2.0
/*
 * Hardware monitoring fan driver for Raspberry Pi RP1
 */

#include <linux/module.h>
#include <linux/device.h>
#include <linux/platform_device.h>
#include <linux/mod_devicetable.h>
#include <linux/hwmon.h>
#include <linux/err.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <soc/bcm2835/raspberrypi-pwm-rp1.h>

struct rp1_fan_data {
	struct device *pwm_dev;
};

static int rp1_fan_read(struct device *dev, enum hwmon_sensor_types type,
			u32 attr, int channel, long *val)
{
	struct rp1_fan_data *data = dev_get_drvdata(dev);
	int rpm;

	if (type == hwmon_fan && attr == hwmon_fan_input) {
		rpm = rp1_pwm_read_tachometer(data->pwm_dev);
		if (rpm < 0)
			return rpm;

		*val = rpm;
		return 0;
	}

	return -EOPNOTSUPP;
}

static const struct hwmon_channel_info * const rp1_fan_info[] = {
	HWMON_CHANNEL_INFO(fan, HWMON_F_INPUT),
	NULL
};

static const struct hwmon_ops rp1_fan_hwmon_ops = {
	.visible = 0444,
	.read = rp1_fan_read,
};

static const struct hwmon_chip_info rp1_fan_chip_info = {
	.ops = &rp1_fan_hwmon_ops,
	.info = rp1_fan_info,
};

static void rp1_fan_put_device(void *data)
{
	put_device(data);
}

static int rp1_fan_probe(struct platform_device *pdev)
{
	struct platform_device *pwm_pdev;
	struct device *dev = &pdev->dev;
	struct device_node *pwm_node;
	struct rp1_fan_data *data;
	struct device_link *link;
	struct device *hwmon_dev;
	int ret;

	pwm_node = of_parse_phandle(dev->of_node, "raspberrypi,pwm-tachometer", 0);
	if (!pwm_node)
		return dev_err_probe(dev, -ENODEV,
				     "RP1 PWM node not specified in Device Tree (requires 'raspberrypi,pwm-tachometer')\n");

	if (!of_device_is_available(pwm_node)) {
		of_node_put(pwm_node);
		return dev_err_probe(dev, -ENODEV,
				     "RP1 PWM node is disabled\n");
	}

	pwm_pdev = of_find_device_by_node(pwm_node);
	of_node_put(pwm_node);
	if (!pwm_pdev)
		return dev_err_probe(dev, -EPROBE_DEFER,
				     "RP1 PWM platform device not populated yet\n");

	scoped_guard(device, &pwm_pdev->dev) {
		if (!device_is_bound(&pwm_pdev->dev)) {
			put_device(&pwm_pdev->dev);
			return dev_err_probe(dev, -EPROBE_DEFER,
					     "RP1 PWM driver is not bound yet\n");
		}
	}

	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data) {
		put_device(&pwm_pdev->dev);
		return -ENOMEM;
	}

	ret = devm_add_action_or_reset(dev, rp1_fan_put_device, &pwm_pdev->dev);
	if (ret)
		return ret;

	data->pwm_dev = &pwm_pdev->dev;

	link = device_link_add(dev, &pwm_pdev->dev, DL_FLAG_AUTOREMOVE_CONSUMER);
	if (!link)
		return dev_err_probe(dev, -EINVAL,
				     "Cannot create the device_link to PWM controller\n");

	hwmon_dev = devm_hwmon_device_register_with_info(dev, "rp1_fan",
							 data,
							 &rp1_fan_chip_info,
							 NULL);

	return PTR_ERR_OR_ZERO(hwmon_dev);
}

static const struct of_device_id rp1_fan_of_match[] = {
	{ .compatible = "raspberrypi,rp1-fan" },
	{ }
};
MODULE_DEVICE_TABLE(of, rp1_fan_of_match);

static struct platform_driver rp1_fan_driver = {
	.probe = rp1_fan_probe,
	.driver = {
		.name = "rp1-fan",
		.of_match_table = rp1_fan_of_match,
	},
};
module_platform_driver(rp1_fan_driver);

MODULE_AUTHOR("Andrea della Porta <andrea.porta@suse.com>");
MODULE_DESCRIPTION("Raspberry Pi RP1 HWMon Fan Driver");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("RP1_PWM");
