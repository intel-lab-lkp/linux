// SPDX-License-Identifier: GPL-2.0
/* Author: Hans de Goede <hdegoede@redhat.com> */

#include <linux/acpi.h>
#include <linux/gpio/consumer.h>
#include <linux/leds.h>
#include <linux/platform_data/x86/int3472.h>

static int int3472_pled_set(struct led_classdev *led_cdev,
			    enum led_brightness brightness)
{
	struct int3472_pled *pled = container_of(led_cdev, struct int3472_pled, classdev);

	gpiod_set_value_cansleep(pled->gpio, brightness);
	return 0;
}

int skl_int3472_register_pled(struct int3472_discrete_device *int3472, struct gpio_desc *gpio)
{
	struct int3472_pled *pled = &int3472->pled;
	char *p;
	int ret;

	if (pled->classdev.dev)
		return -EBUSY;

	pled->gpio = gpio;

	/* Generate the name, replacing the ':' in the ACPI devname with '_' */
	snprintf(pled->name, sizeof(pled->name),
		 "%s::privacy_led", acpi_dev_name(int3472->sensor));
	p = strchr(pled->name, ':');
	if (p)
		*p = '_';

	pled->classdev.name = pled->name;
	pled->classdev.max_brightness = 1;
	pled->classdev.brightness_set_blocking = int3472_pled_set;

	ret = led_classdev_register(int3472->dev, &pled->classdev);
	if (ret)
		return ret;

	pled->lookup.provider = pled->name;
	pled->lookup.dev_id = int3472->sensor_name;
	pled->lookup.con_id = "privacy";
	led_add_lookup(&pled->lookup);

	return 0;
}

void skl_int3472_unregister_pled(struct int3472_discrete_device *int3472)
{
	struct int3472_pled *pled = &int3472->pled;

	if (IS_ERR_OR_NULL(pled->classdev.dev))
		return;

	led_remove_lookup(&pled->lookup);
	led_classdev_unregister(&pled->classdev);
	gpiod_put(pled->gpio);
}
