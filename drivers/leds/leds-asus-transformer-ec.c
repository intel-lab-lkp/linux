// SPDX-License-Identifier: GPL-2.0-only

#include <linux/err.h>
#include <linux/leds.h>
#include <linux/mfd/asus-transformer-ec.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

static void asus_ec_led_set_brightness_amber(struct led_classdev *led,
					     enum led_brightness brightness)
{
	const struct asusec_info *ec = dev_get_drvdata(led->dev->parent);

	if (brightness)
		asus_ec_set_ctl_bits(ec, ASUSEC_CTL_LED_AMBER);
	else
		asus_ec_clear_ctl_bits(ec, ASUSEC_CTL_LED_AMBER);
}

static void asus_ec_led_set_brightness_green(struct led_classdev *led,
					     enum led_brightness brightness)
{
	const struct asusec_info *ec = dev_get_drvdata(led->dev->parent);

	if (brightness)
		asus_ec_set_ctl_bits(ec, ASUSEC_CTL_LED_GREEN);
	else
		asus_ec_clear_ctl_bits(ec, ASUSEC_CTL_LED_GREEN);
}

static int asus_ec_led_probe(struct platform_device *pdev)
{
	struct asusec_info *ec = cell_to_ec(pdev);
	struct device *dev = &pdev->dev;
	struct led_classdev *amber_led, *green_led;
	int ret;

	platform_set_drvdata(pdev, ec);

	amber_led = devm_kzalloc(dev, sizeof(*amber_led), GFP_KERNEL);
	if (!amber_led)
		return -ENOMEM;

	amber_led->name = devm_kasprintf(dev, GFP_KERNEL, "%s::amber", ec->name);
	amber_led->max_brightness = 1;
	amber_led->flags = LED_CORE_SUSPENDRESUME | LED_RETAIN_AT_SHUTDOWN;
	amber_led->brightness_set = asus_ec_led_set_brightness_amber;

	ret = devm_led_classdev_register(dev, amber_led);
	if (ret)
		return dev_err_probe(dev, ret, "failed to register amber LED\n");

	green_led = devm_kzalloc(dev, sizeof(*green_led), GFP_KERNEL);
	if (!green_led)
		return -ENOMEM;

	green_led->name = devm_kasprintf(dev, GFP_KERNEL, "%s::green", ec->name);
	green_led->max_brightness = 1;
	green_led->flags = LED_CORE_SUSPENDRESUME | LED_RETAIN_AT_SHUTDOWN;
	green_led->brightness_set = asus_ec_led_set_brightness_green;

	ret = devm_led_classdev_register(dev, green_led);
	if (ret)
		return dev_err_probe(dev, ret, "failed to register green LED\n");

	return 0;
}

static struct platform_driver asus_ec_led_driver = {
	.driver.name = "asus-transformer-ec-led",
	.probe = asus_ec_led_probe,
};
module_platform_driver(asus_ec_led_driver);

MODULE_AUTHOR("Michał Mirosław <mirq-linux@rere.qmqm.pl>");
MODULE_AUTHOR("Svyatoslav Ryhel <clamor95@gmail.com>");
MODULE_DESCRIPTION("ASUS Transformer's charging LED driver");
MODULE_LICENSE("GPL");
