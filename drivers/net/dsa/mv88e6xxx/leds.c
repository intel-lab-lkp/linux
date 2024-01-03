// SPDX-License-Identifier: GPL-2.0-or-later
#include <net/dsa.h>

#include "chip.h"
#include "port.h"

#define FLAG_ACT      (BIT(TRIGGER_NETDEV_RX) | BIT(TRIGGER_NETDEV_TX))
#define FLAG_LINK     BIT(TRIGGER_NETDEV_LINK)
#define FLAG_LINK_10  BIT(TRIGGER_NETDEV_LINK_10)
#define FLAG_LINK_100 BIT(TRIGGER_NETDEV_LINK_100)
#define FLAG_LINK_1G  BIT(TRIGGER_NETDEV_LINK_1000)
#define FLAG_FULL     BIT(TRIGGER_NETDEV_FULL_DUPLEX)

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

enum mv88e6393x_led_mode {
	MV88E6393X_LED_MODE_BLINK = 0xd,
	MV88E6393X_LED_MODE_OFF = 0xe,
	MV88E6393X_LED_MODE_ON = 0xf,

	MV88E6393X_LED_MODES = 0x10
};

static const unsigned long mv88e6393x_led_map_p1_p8[2][MV88E6393X_LED_MODES] = {
	{
		[0x1] = FLAG_ACT | FLAG_LINK_100 | FLAG_LINK_1G,
		[0x2] = FLAG_ACT | FLAG_LINK_1G,
		[0x3] = FLAG_ACT | FLAG_LINK,
		[0x6] = FLAG_FULL,
		[0x7] = FLAG_ACT | FLAG_LINK_10 | FLAG_LINK_1G,
		[0x8] = FLAG_LINK,
		[0x9] = FLAG_LINK_10,
		[0xa] = FLAG_ACT | FLAG_LINK_10,
		[0xb] = FLAG_LINK_100 | FLAG_LINK_1G,
	},
	{
		[0x1] = FLAG_ACT,
		[0x2] = FLAG_ACT | FLAG_LINK_10 | FLAG_LINK_100,
		[0x3] = FLAG_LINK_1G,
		[0x5] = FLAG_ACT | FLAG_LINK,
		[0x6] = FLAG_ACT | FLAG_LINK_10 | FLAG_LINK_1G,
		[0x7] = FLAG_LINK_10 | FLAG_LINK_1G,
		[0x9] = FLAG_LINK_100,
		[0xa] = FLAG_ACT | FLAG_LINK_100,
		[0xb] = FLAG_LINK_10 | FLAG_LINK_100,
	}
};

static const unsigned long mv88e6393x_led_map_p9_p10[2][MV88E6393X_LED_MODES] = {
	{
		[0x1] = FLAG_ACT | FLAG_LINK,
	},
	{
		[0x6] = FLAG_FULL,
		[0x7] = FLAG_ACT | FLAG_LINK,
		[0x8] = FLAG_LINK,
	}
};

const unsigned long *mv88e6393x_led_map(struct mv88e6xxx_led *led)
{
	switch (led->port) {
	case 1:
	case 2:
	case 3:
	case 4:
	case 5:
	case 6:
	case 7:
	case 8:
		return mv88e6393x_led_map_p1_p8[led->index];
	case 9:
	case 10:
		return mv88e6393x_led_map_p9_p10[led->index];
	}

	return NULL;
}

static int mv88e6393x_led_flags_to_mode(struct mv88e6xxx_led *led, unsigned long flags)
{
	const unsigned long *map = mv88e6393x_led_map(led);
	int i;

	if (!map)
		return -ENODEV;

	if (!flags)
		return MV88E6393X_LED_MODE_OFF;

	for (i = 0; i < MV88E6393X_LED_MODES; i++) {
		if (map[i] == flags)
			return i;
	}

	return -EINVAL;
}

static int mv88e6393x_led_mode_to_flags(struct mv88e6xxx_led *led, u8 mode,
					unsigned long *flags)
{
	const unsigned long *map = mv88e6393x_led_map(led);

	if (!map)
		return -ENODEV;

	if (mode == MV88E6393X_LED_MODE_OFF) {
		*flags = 0;
		return 0;
	}

	if (map[mode]) {
		*flags = map[mode];
		return 0;
	}

	return -EINVAL;
}

static int mv88e6393x_led_set(struct mv88e6xxx_led *led, int mode)
{
	u16 ctrl;
	int err;

	if (mode < 0)
		return mode;

	mv88e6xxx_reg_lock(led->chip);

	err = mv88e6393x_port_led_read(led->chip, led->port, 0, &ctrl);
	if (err)
		goto out;

	switch (led->index) {
	case 0:
		ctrl &= ~0x0f;
		ctrl |= mode;
		break;
	case 1:
		ctrl &= ~0xf0;
		ctrl |= mode << 4;
	}

	err = mv88e6393x_port_led_write(led->chip, led->port, 0, ctrl);
out:
	mv88e6xxx_reg_unlock(led->chip);
	return err;
}

static int mv88e6393x_led_get(struct mv88e6xxx_led *led)
{
	u16 ctrl;
	int err;

	mv88e6xxx_reg_lock(led->chip);
	err = mv88e6393x_port_led_read(led->chip, led->port, 0, &ctrl);
	mv88e6xxx_reg_unlock(led->chip);
	if (err)
		return err;

	switch (led->index) {
	case 0:
		return ctrl & 0xf;
	case 1:
		return (ctrl >> 4) & 0xf;
	}

	return -EINVAL;
}

static int mv88e6393x_led_brightness_set(struct mv88e6xxx_led *led,
					 enum led_brightness brightness)
{
	if (brightness == LED_OFF)
		return mv88e6393x_led_set(led, MV88E6393X_LED_MODE_OFF);

	return mv88e6393x_led_set(led, MV88E6393X_LED_MODE_ON);
}

static int mv88e6393x_led_blink_set(struct mv88e6xxx_led *led,
				    unsigned long *delay_on,
				    unsigned long *delay_off)
{
	int err;

	/* Defer anything other than 50% duty cycles to software */
	if (*delay_on != *delay_off)
		return -EINVAL;

	/* Reject values outside ~20% of our default rate (84ms) */
	if (*delay_on && ((*delay_on < 30) || (*delay_on > 50)))
		return -EINVAL;

	err = mv88e6393x_led_set(led, MV88E6393X_LED_MODE_BLINK);
	if (!err)
		*delay_on = *delay_off = 42;

	return err;
}

static int mv88e6393x_led_hw_control_is_supported(struct mv88e6xxx_led *led,
						  unsigned long flags)
{
	int mode = mv88e6393x_led_flags_to_mode(led, flags);

	return (mode < 0) ? mode : 0;
}

static int mv88e6393x_led_hw_control_set(struct mv88e6xxx_led *led,
					 unsigned long flags)
{
	int mode = mv88e6393x_led_flags_to_mode(led, flags);

	return mv88e6393x_led_set(led, mode);
}

static int mv88e6393x_led_hw_control_get(struct mv88e6xxx_led *led,
					 unsigned long *flags)
{
	int mode = mv88e6393x_led_get(led);

	if (mode < 0)
		return mode;

	return mv88e6393x_led_mode_to_flags(led, mode, flags);
}

const struct mv88e6xxx_led_ops mv88e6393x_led_ops = {
	.brightness_set = mv88e6393x_led_brightness_set,
	.blink_set = mv88e6393x_led_blink_set,
	.hw_control_is_supported = mv88e6393x_led_hw_control_is_supported,
	.hw_control_set = mv88e6393x_led_hw_control_set,
	.hw_control_get = mv88e6393x_led_hw_control_get,
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
