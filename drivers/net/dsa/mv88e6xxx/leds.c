// SPDX-License-Identifier: GPL-2.0-or-later
#include <net/dsa.h>

#include "chip.h"
#include "port.h"

struct mv88e6xxx_led {
	struct mv88e6xxx_chip *chip;
	int port;
	u8 index;

	struct led_classdev ldev;
};

struct mv88e6xxx_led_ops {
	int (*brightness_set)(struct mv88e6xxx_led *led,
			      enum led_brightness brightness);
	int (*blink_set)(struct mv88e6xxx_led *led,
			 unsigned long *delay_on, unsigned long *delay_off);
	int (*hw_control_is_supported)(struct mv88e6xxx_led *led,
				       unsigned long flags);
	int (*hw_control_set)(struct mv88e6xxx_led *led, unsigned long flags);
	int (*hw_control_get)(struct mv88e6xxx_led *led, unsigned long *flags);
};

static int mv88e6xxx_led_brightness_set(struct led_classdev *ldev,
					enum led_brightness brightness)
{
	const struct mv88e6xxx_led_ops *ops;
	struct mv88e6xxx_led *led;

	led = container_of(ldev, struct mv88e6xxx_led, ldev);
	ops = led->chip->info->ops->led_ops;

	if (!ops->brightness_set)
		return -EOPNOTSUPP;

	return ops->brightness_set(led, brightness);
}

static int mv88e6xxx_led_blink_set(struct led_classdev *ldev,
				   unsigned long *delay_on,
				   unsigned long *delay_off)
{
	const struct mv88e6xxx_led_ops *ops;
	struct mv88e6xxx_led *led;

	led = container_of(ldev, struct mv88e6xxx_led, ldev);
	ops = led->chip->info->ops->led_ops;

	if (!ops->blink_set)
		return -EOPNOTSUPP;

	return ops->blink_set(led, delay_on, delay_off);
}

static int mv88e6xxx_led_hw_control_is_supported(struct led_classdev *ldev,
						 unsigned long flags)
{
	const struct mv88e6xxx_led_ops *ops;
	struct mv88e6xxx_led *led;

	led = container_of(ldev, struct mv88e6xxx_led, ldev);
	ops = led->chip->info->ops->led_ops;

	if (!ops->hw_control_is_supported)
		return -EOPNOTSUPP;

	return ops->hw_control_is_supported(led, flags);
}

static int mv88e6xxx_led_hw_control_set(struct led_classdev *ldev,
					unsigned long flags)
{
	const struct mv88e6xxx_led_ops *ops;
	struct mv88e6xxx_led *led;

	led = container_of(ldev, struct mv88e6xxx_led, ldev);
	ops = led->chip->info->ops->led_ops;

	if (!ops->hw_control_set)
		return -EOPNOTSUPP;

	return ops->hw_control_set(led, flags);
}

static int mv88e6xxx_led_hw_control_get(struct led_classdev *ldev,
					unsigned long *flags)
{
	const struct mv88e6xxx_led_ops *ops;
	struct mv88e6xxx_led *led;

	led = container_of(ldev, struct mv88e6xxx_led, ldev);
	ops = led->chip->info->ops->led_ops;

	if (!ops->hw_control_get)
		return -EOPNOTSUPP;

	return ops->hw_control_get(led, flags);
}

static struct device *mv88e6xxx_led_hw_control_get_device(struct led_classdev *ldev)
{
	struct mv88e6xxx_led *led;
	struct dsa_port *dp;

	led = container_of(ldev, struct mv88e6xxx_led, ldev);
	dp = dsa_to_port(led->chip->ds, led->port);

	if (dp && dp->user)
		return &dp->user->dev;

	return NULL;
}

static int mv88e6xxx_port_setup_led(struct mv88e6xxx_chip *chip, int port,
				    struct device_node *np)
{
	struct led_init_data init_data = {};
	struct mv88e6xxx_led *led;
	char *devname;
	u32 index;
	int err;

	err = of_property_read_u32(np, "reg", &index);
	if (err)
		return err;

	if (index >= 2)
		return -EINVAL;

	led = devm_kzalloc(chip->dev, sizeof(*led), GFP_KERNEL);
	if (!led)
		return -ENOMEM;

	*led = (struct mv88e6xxx_led) {
		.chip = chip,
		.port = port,
		.index = index,

		.ldev = {
			.max_brightness = 1,
			.brightness_set_blocking = mv88e6xxx_led_brightness_set,
			.blink_set = mv88e6xxx_led_blink_set,

#ifdef CONFIG_LEDS_TRIGGERS
			.hw_control_trigger = "netdev",
			.hw_control_get_device = mv88e6xxx_led_hw_control_get_device,

			.hw_control_is_supported = mv88e6xxx_led_hw_control_is_supported,
			.hw_control_set = mv88e6xxx_led_hw_control_set,
			.hw_control_get = mv88e6xxx_led_hw_control_get,
#endif
		},
	};

	devname = devm_kasprintf(chip->dev, GFP_KERNEL, "%s.%d",
				 dev_name(chip->dev), port);
	if (!devname)
		return -ENOMEM;

	init_data = (struct led_init_data) {
		.fwnode = of_fwnode_handle(np),
		.devname_mandatory = true,
		.devicename = devname,
	};

	return devm_led_classdev_register_ext(chip->dev, &led->ldev, &init_data);
}

int mv88e6xxx_port_setup_leds(struct dsa_switch *ds, int port)
{
	struct dsa_port *dp = dsa_to_port(ds, port);
	struct mv88e6xxx_chip *chip = ds->priv;
	struct device_node *pnp, *np;
	int err;

	if (!chip->info->ops->led_ops)
		return 0;

	if (!dp->dn)
		return 0;

	pnp = of_get_child_by_name(dp->dn, "leds");
	if (!pnp)
		return 0;

	for_each_available_child_of_node(pnp, np) {
		err = mv88e6xxx_port_setup_led(chip, port, np);
		if (err)
			return err;
	}

	return 0;
}
