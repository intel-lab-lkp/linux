// SPDX-License-Identifier: GPL-2.0
/* Author: Hans de Goede <hdegoede@redhat.com> */

#include <linux/acpi.h>
#include <linux/gpio/consumer.h>
#include <linux/leds.h>
#include <linux/platform_data/x86/int3472.h>

static int int3472_led_set(struct led_classdev *led_cdev,
			   enum led_brightness brightness)
{
	struct int3472_discrete_device *int3472 =
		container_of(led_cdev, struct int3472_discrete_device, led.classdev);

	gpiod_set_value_cansleep(int3472->led.gpio, brightness);
	return 0;
}

int skl_int3472_register_led(struct int3472_discrete_device *int3472,
			     struct gpio_desc *gpio)
{
	char *p;
	int ret;

	if (int3472->led.classdev.dev)
		return -EBUSY;

	int3472->led.gpio = gpio;

	/* Generate the name, replacing the ':' in the ACPI devname with '_' */
	snprintf(int3472->led.name, sizeof(int3472->led.name),
		 "%s::privacy_led", acpi_dev_name(int3472->sensor));
	p = strchr(int3472->led.name, ':');
	if (p)
		*p = '_';

	int3472->led.classdev.name = int3472->led.name;
	int3472->led.classdev.max_brightness = 1;
	int3472->led.classdev.brightness_set_blocking = int3472_led_set;

	ret = led_classdev_register(int3472->dev, &int3472->led.classdev);
	if (ret)
		return ret;

	int3472->led.lookup.provider = int3472->led.name;
	int3472->led.lookup.dev_id = int3472->sensor_name;
	int3472->led.lookup.con_id = "privacy";
	led_add_lookup(&int3472->led.lookup);

	return 0;
}

void skl_int3472_unregister_led(struct int3472_discrete_device *int3472)
{
	if (IS_ERR_OR_NULL(int3472->led.classdev.dev))
		return;

	led_remove_lookup(&int3472->led.lookup);
	led_classdev_unregister(&int3472->led.classdev);
	gpiod_put(int3472->led.gpio);
}
