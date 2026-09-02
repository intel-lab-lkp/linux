// SPDX-License-Identifier: GPL-2.0-only
/*
 * LED driver for PCA995x I2C LED drivers
 *
 * Copyright 2011 bct electronic GmbH
 * Copyright 2013 Qtechnology/AS
 * Copyright 2022 NXP
 * Copyright 2023 Marek Vasut
 */

#include <linux/bits.h>
#include <linux/gpio.h>
#include <linux/i2c.h>
#include <linux/leds.h>
#include <linux/module.h>
#include <linux/mod_devicetable.h>
#include <linux/property.h>
#include <linux/regmap.h>

/* Register definition */
#define PCA995X_MODE1			0x00
#define PCA995X_MODE2			0x01
#define PCA995X_LEDOUT0			0x02

/* Auto-increment disabled. Normal mode */
#define PCA995X_MODE1_CFG		0x00

#define PCA995X_MODE2_CLRERR		BIT(4)
#define PCA995X_MODE2_ERROR		BIT(6)

/* LED select registers determine the source that drives LED outputs */
#define PCA995X_LED_OFF			0x0
#define PCA995X_LED_ON			0x1
#define PCA995X_LED_PWM_MODE		0x2
#define PCA995X_LDRX_MASK		0x3
#define PCA995X_LDRX_BITS		2

#define PCA995X_MAX_OUTPUTS		24
#define PCA995X_OUTPUTS_PER_REG		4

#define PCA995X_IREFALL_FULL_CFG	0xFF
#define PCA995X_IREFALL_HALF_CFG	(PCA995X_IREFALL_FULL_CFG / 2)

#define PCA995X_EFLAG_BITS		2
#define PCA995X_EFLAG_MASK		GENMASK(1, 0)

#define ldev_to_led(c)	container_of(c, struct pca995x_led, ldev)

struct pca995x_chipdef {
	unsigned int num_leds;
	u8 pwm_base;
	u8 irefall;
	u8 eflag_base;
};

static const struct pca995x_chipdef pca9952_chipdef = {
	.num_leds	= 16,
	.pwm_base	= 0x0a,
	.irefall	= 0x43,
	.eflag_base	= 0x44,
};

static const struct pca995x_chipdef pca9955b_chipdef = {
	.num_leds	= 16,
	.pwm_base	= 0x08,
	.irefall	= 0x45,
	.eflag_base	= 0x46,
};

static const struct pca995x_chipdef pca9956b_chipdef = {
	.num_leds	= 24,
	.pwm_base	= 0x0a,
	.irefall	= 0x40,
	.eflag_base	= 0x41,
};

struct pca995x_led {
	unsigned int led_no;
	struct led_classdev ldev;
	struct pca995x_chip *chip;
};

struct pca995x_chip {
	struct regmap *regmap;
	struct pca995x_led leds[PCA995X_MAX_OUTPUTS];
	const struct pca995x_chipdef *chipdef;
};

static int pca995x_brightness_set(struct led_classdev *led_cdev,
				  enum led_brightness brightness)
{
	struct pca995x_led *led = ldev_to_led(led_cdev);
	struct pca995x_chip *chip = led->chip;
	const struct pca995x_chipdef *chipdef = chip->chipdef;
	u8 ledout_addr, pwmout_addr;
	int shift, ret;

	pwmout_addr = chipdef->pwm_base + led->led_no;
	ledout_addr = PCA995X_LEDOUT0 + (led->led_no / PCA995X_OUTPUTS_PER_REG);
	shift = PCA995X_LDRX_BITS * (led->led_no % PCA995X_OUTPUTS_PER_REG);

	switch (brightness) {
	case LED_FULL:
		return regmap_update_bits(chip->regmap, ledout_addr,
					  PCA995X_LDRX_MASK << shift,
					  PCA995X_LED_ON << shift);
	case LED_OFF:
		return regmap_update_bits(chip->regmap, ledout_addr,
					  PCA995X_LDRX_MASK << shift, 0);
	default:
		/* Adjust brightness as per user input by changing individual PWM */
		ret = regmap_write(chip->regmap, pwmout_addr, brightness);
		if (ret)
			return ret;

		/*
		 * Change LDRx configuration to individual brightness via PWM.
		 * LED will stop blinking if it's doing so.
		 */
		return regmap_update_bits(chip->regmap, ledout_addr,
					  PCA995X_LDRX_MASK << shift,
					  PCA995X_LED_PWM_MODE << shift);
	}
}

static ssize_t status_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	struct pca995x_led *led = ldev_to_led(led_cdev);
	struct pca995x_chip *chip = led->chip;
	const struct pca995x_chipdef *chipdef = chip->chipdef;
	const char *status = "unknown";
	unsigned int val;
	int shift, ret;
	u8 reg;

	reg = chipdef->eflag_base + (led->led_no / PCA995X_OUTPUTS_PER_REG);
	shift = PCA995X_EFLAG_BITS * (led->led_no % PCA995X_OUTPUTS_PER_REG);

	ret = regmap_read(chip->regmap, reg, &val);
	if (ret)
		return ret;

	switch ((val >> shift) & PCA995X_EFLAG_MASK) {
	case 0:
		status = "okay";
		break;
	case 1:
		status = "short-circuit";
		break;
	case 2:
		status = "open-circuit";
	}

	return sysfs_emit(buf, "%s\n", status);
}

static DEVICE_ATTR_RO(status);

static struct attribute *pca995x_led_attrs[] = {
	&dev_attr_status.attr,
	NULL,
};
ATTRIBUTE_GROUPS(pca995x_led);

static ssize_t has_errors_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct pca995x_chip *chip = i2c_get_clientdata(to_i2c_client(dev));
	unsigned int val;
	int ret;

	ret = regmap_read(chip->regmap, PCA995X_MODE2, &val);
	if (ret)
		return ret;


	return sysfs_emit(buf, "%d\n", !!(val & PCA995X_MODE2_ERROR));
}

static ssize_t has_errors_store(struct device *dev, struct device_attribute *attr,
				const char *buf, size_t count)
{
	struct pca995x_chip *chip = i2c_get_clientdata(to_i2c_client(dev));
	int ret;

	if (strcmp(buf, "clear\n"))
		return -EINVAL;

	ret = regmap_update_bits(chip->regmap, PCA995X_MODE2,
				 PCA995X_MODE2_CLRERR, PCA995X_MODE2_CLRERR);

	return ret ?: count;
}

static DEVICE_ATTR_RW(has_errors);

static struct attribute *pca995x_attrs[] = {
	&dev_attr_has_errors.attr,
	NULL,
};
ATTRIBUTE_GROUPS(pca995x);

static const struct regmap_config pca995x_regmap = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = 0x49,
};

static int pca995x_probe(struct i2c_client *client)
{
	struct fwnode_handle *led_fwnodes[PCA995X_MAX_OUTPUTS] = { 0 };
	struct device *dev = &client->dev;
	const struct pca995x_chipdef *chipdef;
	struct gpio_desc *reset_gpio;
	struct pca995x_chip *chip;
	struct pca995x_led *led;
	int i, j, reg, ret;
	u32 iref;

	chipdef = device_get_match_data(&client->dev);

	if (!dev_fwnode(dev))
		return -ENODEV;

	reset_gpio = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(reset_gpio))
		return dev_err_probe(dev, PTR_ERR(reset_gpio),
				     "failed to request reset GPIO\n");
	if (reset_gpio) {
		usleep_range(3, 4);
		gpiod_set_value_cansleep(reset_gpio, 0);
		usleep_range(1500, 1600);
	}

	chip = devm_kzalloc(dev, sizeof(*chip), GFP_KERNEL);
	if (!chip)
		return -ENOMEM;

	chip->chipdef = chipdef;
	chip->regmap = devm_regmap_init_i2c(client, &pca995x_regmap);
	if (IS_ERR(chip->regmap))
		return PTR_ERR(chip->regmap);

	i2c_set_clientdata(client, chip);

	if (device_property_read_u32(dev, "output-gain", &iref))
		iref = PCA995X_IREFALL_HALF_CFG;
	else if (iref > PCA995X_IREFALL_FULL_CFG)
		return dev_err_probe(dev, -EINVAL, "invalid output-gain\n");

	device_for_each_child_node_scoped(dev, child) {
		ret = fwnode_property_read_u32(child, "reg", &reg);
		if (ret)
			return ret;

		if (reg < 0 || reg >= PCA995X_MAX_OUTPUTS || led_fwnodes[reg])
			return -EINVAL;

		led = &chip->leds[reg];
		led_fwnodes[reg] = fwnode_handle_get(child);
		led->chip = chip;
		led->led_no = reg;
		led->ldev.brightness_set_blocking = pca995x_brightness_set;
		led->ldev.max_brightness = 255;
		led->ldev.groups = pca995x_led_groups;
	}

	for (i = 0; i < PCA995X_MAX_OUTPUTS; i++) {
		struct led_init_data init_data = {};

		if (!led_fwnodes[i])
			continue;

		init_data.fwnode = led_fwnodes[i];

		ret = devm_led_classdev_register_ext(dev,
						     &chip->leds[i].ldev,
						     &init_data);
		if (ret < 0) {
			for (j = i; j < PCA995X_MAX_OUTPUTS; j++)
				fwnode_handle_put(led_fwnodes[j]);
			return dev_err_probe(dev, ret,
					     "Could not register LED %s\n",
					     chip->leds[i].ldev.name);
		}
	}

	/* Disable LED all-call address and set normal mode */
	ret = regmap_write(chip->regmap, PCA995X_MODE1, PCA995X_MODE1_CFG);
	if (ret)
		return ret;

	/* IREF Output current value for all LEDn outputs */
	ret = regmap_write(chip->regmap, chipdef->irefall, iref);
	if (ret)
		return ret;

	return sysfs_create_groups(&dev->kobj, pca995x_groups);
}

static void pca995x_remove(struct i2c_client *client)
{
	struct device *dev = &client->dev;

	return sysfs_remove_groups(&dev->kobj, pca995x_groups);
}

static const struct i2c_device_id pca995x_id[] = {
	{ .name = "pca9952", .driver_data = (kernel_ulong_t)&pca9952_chipdef },
	{ .name = "pca9955b", .driver_data = (kernel_ulong_t)&pca9955b_chipdef },
	{ .name = "pca9956b", .driver_data = (kernel_ulong_t)&pca9956b_chipdef },
	{ }
};
MODULE_DEVICE_TABLE(i2c, pca995x_id);

static const struct of_device_id pca995x_of_match[] = {
	{ .compatible = "nxp,pca9952", .data = &pca9952_chipdef },
	{ .compatible = "nxp,pca9955b", .data = &pca9955b_chipdef },
	{ .compatible = "nxp,pca9956b", .data = &pca9956b_chipdef },
	{},
};
MODULE_DEVICE_TABLE(of, pca995x_of_match);

static struct i2c_driver pca995x_driver = {
	.driver = {
		.name = "leds-pca995x",
		.of_match_table = pca995x_of_match,
	},
	.probe = pca995x_probe,
	.remove = pca995x_remove,
	.id_table = pca995x_id,
};
module_i2c_driver(pca995x_driver);

MODULE_AUTHOR("Isai Gaspar <isaiezequiel.gaspar@nxp.com>");
MODULE_DESCRIPTION("PCA995x LED driver");
MODULE_LICENSE("GPL");
