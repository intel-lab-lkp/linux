// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * portwell-ec.c: Portwell embedded controller driver.
 *
 * Tested on:
 *  - Portwell NANO-6064
 *
 * This driver provides support for GPIO and Watchdog Timer
 * functionalities of the Portwell boards with ITE embedded controller (EC).
 * The EC is accessed through I/O ports and provides:
 *  - 8 GPIO pins for control and monitoring
 *  - Hardware watchdog with 1-15300 second timeout range
 *
 * It integrates with the Linux GPIO and Watchdog subsystems, allowing
 * userspace interaction with EC GPIO pins and watchdog control,
 * ensuring system stability and configurability.
 *
 * (C) Copyright 2025 Portwell, Inc.
 * Author: Yen-Chi Huang (jesse.huang@portwell.com.tw)
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/acpi.h>
#include <linux/bitfield.h>
#include <linux/dmi.h>
#include <linux/gpio/driver.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/sizes.h>
#include <linux/string.h>
#include <linux/watchdog.h>
#include <linux/hwmon.h>
#include <linux/hwmon-sysfs.h>
#include <linux/hwmon-vid.h>
#include <linux/err.h>

#define PORTWELL_EC_IOSPACE              0xe300
#define PORTWELL_EC_IOSPACE_LEN          SZ_256

#define PORTWELL_GPIO_PINS               8
#define PORTWELL_GPIO_DIR_REG            0x2b
#define PORTWELL_GPIO_VAL_REG            0x2c

#define PORTWELL_WDT_EC_CONFIG_ADDR      0x06
#define PORTWELL_WDT_CONFIG_ENABLE       0x1
#define PORTWELL_WDT_CONFIG_DISABLE      0x0
#define PORTWELL_WDT_EC_COUNT_MIN_ADDR   0x07
#define PORTWELL_WDT_EC_COUNT_SEC_ADDR   0x08
#define PORTWELL_WDT_EC_MAX_COUNT_SECOND (255 * 60)

#define PORTWELL_EC_FW_VENDOR_ADDRESS    0x4d
#define PORTWELL_EC_FW_VENDOR_LENGTH     3
#define PORTWELL_EC_FW_VENDOR_NAME       "PWG"

#define PORTWELL_EC_ADC_MAX              1023

static bool force;
module_param(force, bool, 0444);
MODULE_PARM_DESC(force, "Force loading EC driver without checking DMI boardname");

enum pwec_board_id {
	PWEC_BOARD_NANO6064,
	PWEC_BOARD_ID_MAX
};

struct pwec_hwmon_data {
	const char *label;
	u8 lsb_reg;
	u8 msb_reg;
	u32 scale;
};

struct pwec_data {
	const struct pwec_hwmon_data *hwmon_in_data;
	int hwmon_in_num;
	const struct pwec_hwmon_data *hwmon_temp_data;
	int hwmon_temp_num;
};

static const struct pwec_hwmon_data pwec_nano_hwmon_in[] = {
	{ "Vcore", 0x20, 0x21, 3000 },
	{ "VDIMM", 0x32, 0x33, 3000 },
	{ "3.3V",  0x22, 0x23, 6000 },
	{ "5V",    0x24, 0x25, 9600 },
	{ "12V",   0x30, 0x31, 19800 },
};

static const struct pwec_hwmon_data pwec_nano_hwmon_temp[] = {
	{ "System Temperature", 0x02, 0, 0 },
};

static const struct pwec_data pwec_board_data[] = {
	[PWEC_BOARD_NANO6064] = {
		.hwmon_in_data = pwec_nano_hwmon_in,
		.hwmon_in_num = ARRAY_SIZE(pwec_nano_hwmon_in),
		.hwmon_temp_data = pwec_nano_hwmon_temp,
		.hwmon_temp_num = ARRAY_SIZE(pwec_nano_hwmon_temp),
	},
};

static const struct dmi_system_id pwec_dmi_table[] = {
	{
		.ident = "NANO-6064 series",
		.matches = {
			DMI_MATCH(DMI_BOARD_NAME, "NANO-6064"),
		},
		.driver_data = (void *)&pwec_board_data[PWEC_BOARD_NANO6064],
	},
	{ }
};
MODULE_DEVICE_TABLE(dmi, pwec_dmi_table);

/* Functions for access EC via IOSPACE */

static void pwec_write(u8 index, u8 data)
{
	outb(data, PORTWELL_EC_IOSPACE + index);
}

static u8 pwec_read(u8 address)
{
	return inb(PORTWELL_EC_IOSPACE + address);
}

static u16 pwec_read16_stable(u8 lsb_reg, u8 msb_reg)
{
	u8 lsb, msb, old_msb;

	do {
		old_msb = pwec_read(msb_reg);
		lsb = pwec_read(lsb_reg);
		msb = pwec_read(msb_reg);
	} while (msb != old_msb);

	return (msb << 8) | lsb;
}

/* GPIO functions */

static int pwec_gpio_get(struct gpio_chip *chip, unsigned int offset)
{
	return pwec_read(PORTWELL_GPIO_VAL_REG) & BIT(offset) ? 1 : 0;
}

static int pwec_gpio_set_rv(struct gpio_chip *chip, unsigned int offset, int val)
{
	u8 tmp = pwec_read(PORTWELL_GPIO_VAL_REG);

	if (val)
		tmp |= BIT(offset);
	else
		tmp &= ~BIT(offset);
	pwec_write(PORTWELL_GPIO_VAL_REG, tmp);

	return 0;
}

static int pwec_gpio_get_direction(struct gpio_chip *chip, unsigned int offset)
{
	u8 direction = pwec_read(PORTWELL_GPIO_DIR_REG) & BIT(offset);

	if (direction)
		return GPIO_LINE_DIRECTION_IN;

	return GPIO_LINE_DIRECTION_OUT;
}

/*
 * Changing direction causes issues on some boards,
 * so direction_input and direction_output are disabled for now.
 */

static int pwec_gpio_direction_input(struct gpio_chip *gc, unsigned int offset)
{
	return -EOPNOTSUPP;
}

static int pwec_gpio_direction_output(struct gpio_chip *gc, unsigned int offset, int value)
{
	return -EOPNOTSUPP;
}

static struct gpio_chip pwec_gpio_chip = {
	.label = "portwell-ec-gpio",
	.get_direction = pwec_gpio_get_direction,
	.direction_input = pwec_gpio_direction_input,
	.direction_output = pwec_gpio_direction_output,
	.get = pwec_gpio_get,
	.set_rv = pwec_gpio_set_rv,
	.base = -1,
	.ngpio = PORTWELL_GPIO_PINS,
};

/* Watchdog functions */

static void pwec_wdt_write_timeout(unsigned int timeout)
{
	pwec_write(PORTWELL_WDT_EC_COUNT_MIN_ADDR, timeout / 60);
	pwec_write(PORTWELL_WDT_EC_COUNT_SEC_ADDR, timeout % 60);
}

static int pwec_wdt_trigger(struct watchdog_device *wdd)
{
	pwec_wdt_write_timeout(wdd->timeout);
	pwec_write(PORTWELL_WDT_EC_CONFIG_ADDR, PORTWELL_WDT_CONFIG_ENABLE);

	return 0;
}

static int pwec_wdt_start(struct watchdog_device *wdd)
{
	return pwec_wdt_trigger(wdd);
}

static int pwec_wdt_stop(struct watchdog_device *wdd)
{
	pwec_write(PORTWELL_WDT_EC_CONFIG_ADDR, PORTWELL_WDT_CONFIG_DISABLE);
	return 0;
}

static int pwec_wdt_set_timeout(struct watchdog_device *wdd, unsigned int timeout)
{
	wdd->timeout = timeout;
	pwec_wdt_write_timeout(wdd->timeout);

	return 0;
}

/* Ensure consistent min/sec read in case of second rollover. */
static unsigned int pwec_wdt_get_timeleft(struct watchdog_device *wdd)
{
	u8 sec, min, old_min;

	do {
		old_min = pwec_read(PORTWELL_WDT_EC_COUNT_MIN_ADDR);
		sec = pwec_read(PORTWELL_WDT_EC_COUNT_SEC_ADDR);
		min = pwec_read(PORTWELL_WDT_EC_COUNT_MIN_ADDR);
	} while (min != old_min);

	return min * 60 + sec;
}

static const struct watchdog_ops pwec_wdt_ops = {
	.owner = THIS_MODULE,
	.start = pwec_wdt_start,
	.stop = pwec_wdt_stop,
	.ping = pwec_wdt_trigger,
	.set_timeout = pwec_wdt_set_timeout,
	.get_timeleft = pwec_wdt_get_timeleft,
};

static struct watchdog_device ec_wdt_dev = {
	.info = &(struct watchdog_info){
		.options = WDIOF_SETTIMEOUT | WDIOF_KEEPALIVEPING | WDIOF_MAGICCLOSE,
		.identity = "Portwell EC watchdog",
	},
	.ops = &pwec_wdt_ops,
	.timeout = 60,
	.min_timeout = 1,
	.max_timeout = PORTWELL_WDT_EC_MAX_COUNT_SECOND,
};

/* HWMON functions */

static umode_t pwec_hwmon_is_visible(const void *data, enum hwmon_sensor_types type,
		u32 attr, int channel)
{
	const struct pwec_data *d = data;

	switch (type) {
	case hwmon_temp:
		if (channel < d->hwmon_temp_num)
			return 0444;
		break;
	case hwmon_in:
		if (channel < d->hwmon_in_num)
			return 0444;
		break;
	default:
		break;
	}

	return 0;
}

static int pwec_hwmon_read(struct device *dev, enum hwmon_sensor_types type,
			   u32 attr, int channel, long *val)
{
	struct pwec_data *data = dev_get_drvdata(dev);
	u16 tmp;

	switch (type) {
	case hwmon_temp:
		if (channel < data->hwmon_temp_num) {
			*val = pwec_read(data->hwmon_temp_data[channel].lsb_reg) * 1000;
			return 0;
		}
		break;
	case hwmon_in:
		if (channel < data->hwmon_in_num) {
			tmp = pwec_read16_stable(data->hwmon_in_data[channel].lsb_reg,
						 data->hwmon_in_data[channel].msb_reg);
			*val = (data->hwmon_in_data[channel].scale * tmp) / PORTWELL_EC_ADC_MAX;
			return 0;
		}
		break;
	default:
		break;
	}

	return -EOPNOTSUPP;
}

static int pwec_hwmon_read_string(struct device *dev, enum hwmon_sensor_types type,
				  u32 attr, int channel, const char **str)
{
	struct pwec_data *data = dev_get_drvdata(dev);

	switch (type) {
	case hwmon_temp:
		if (channel < data->hwmon_temp_num) {
			*str = data->hwmon_temp_data[channel].label;
			return 0;
		}
		break;
	case hwmon_in:
		if (channel < data->hwmon_in_num) {
			*str = data->hwmon_in_data[channel].label;
			return 0;
		}
		break;
	default:
		break;
	}

	return -EOPNOTSUPP;
}

static const struct hwmon_channel_info *pwec_hwmon_info[] = {
	HWMON_CHANNEL_INFO(temp, HWMON_T_INPUT | HWMON_T_LABEL),
	HWMON_CHANNEL_INFO(in,
			   HWMON_I_INPUT | HWMON_I_LABEL,
			   HWMON_I_INPUT | HWMON_I_LABEL,
			   HWMON_I_INPUT | HWMON_I_LABEL,
			   HWMON_I_INPUT | HWMON_I_LABEL,
			   HWMON_I_INPUT | HWMON_I_LABEL),
	NULL
};

static const struct hwmon_ops pwec_hwmon_ops = {
	.is_visible = pwec_hwmon_is_visible,
	.read = pwec_hwmon_read,
	.read_string = pwec_hwmon_read_string,
};

static const struct hwmon_chip_info pwec_chip_info = {
	.ops = &pwec_hwmon_ops,
	.info = pwec_hwmon_info,
};

static int pwec_hwmon_init(struct device *dev)
{
	struct pwec_data *data = dev_get_platdata(dev);
	void *hwmon;
	int ret;

	if (!IS_REACHABLE(CONFIG_HWMON))
		return 0;

	hwmon = devm_hwmon_device_register_with_info(dev, "portwell_ec", data, &pwec_chip_info,
						     NULL);
	ret = PTR_ERR_OR_ZERO(hwmon);
	if (ret)
		dev_err(dev, "Failed to register hwmon_dev: %d\n", ret);

	return ret;
}

static int pwec_firmware_vendor_check(void)
{
	u8 buf[PORTWELL_EC_FW_VENDOR_LENGTH + 1];
	u8 i;

	for (i = 0; i < PORTWELL_EC_FW_VENDOR_LENGTH; i++)
		buf[i] = pwec_read(PORTWELL_EC_FW_VENDOR_ADDRESS + i);
	buf[PORTWELL_EC_FW_VENDOR_LENGTH] = '\0';

	return !strcmp(PORTWELL_EC_FW_VENDOR_NAME, buf) ? 0 : -ENODEV;
}

static int pwec_probe(struct platform_device *pdev)
{
	int ret;

	if (!devm_request_region(&pdev->dev, PORTWELL_EC_IOSPACE,
				PORTWELL_EC_IOSPACE_LEN, dev_name(&pdev->dev))) {
		dev_err(&pdev->dev, "failed to get IO region\n");
		return -EBUSY;
	}

	ret = pwec_firmware_vendor_check();
	if (ret < 0)
		return ret;

	ret = devm_gpiochip_add_data(&pdev->dev, &pwec_gpio_chip, NULL);
	if (ret < 0) {
		dev_err(&pdev->dev, "failed to register Portwell EC GPIO\n");
		return ret;
	}

	ret = devm_watchdog_register_device(&pdev->dev, &ec_wdt_dev);
	if (ret < 0) {
		dev_err(&pdev->dev, "failed to register Portwell EC Watchdog\n");
		return ret;
	}

	ret = pwec_hwmon_init(&pdev->dev);
	if (ret < 0)
		return ret;

	return 0;
}

static int pwec_suspend(struct platform_device *pdev, pm_message_t message)
{
	if (watchdog_active(&ec_wdt_dev))
		return pwec_wdt_stop(&ec_wdt_dev);

	return 0;
}

static int pwec_resume(struct platform_device *pdev)
{
	if (watchdog_active(&ec_wdt_dev))
		return pwec_wdt_start(&ec_wdt_dev);

	return 0;
}

static struct platform_driver pwec_driver = {
	.driver = {
		.name = "portwell-ec",
	},
	.probe = pwec_probe,
	.suspend = pm_ptr(pwec_suspend),
	.resume = pm_ptr(pwec_resume),
};

static struct platform_device *pwec_dev;

static int __init pwec_init(void)
{
	const struct dmi_system_id *match;
	int ret;

	match = dmi_first_match(pwec_dmi_table);
	if (!match) {
		if (!force)
			return -ENODEV;
		match = &pwec_dmi_table[0];
		pr_warn("force load portwell-ec without DMI check\n");
	}

	ret = platform_driver_register(&pwec_driver);
	if (ret)
		return ret;

	pwec_dev = platform_device_register_data(NULL, "portwell-ec", -1, match->driver_data,
						 sizeof(struct pwec_data));
	if (IS_ERR(pwec_dev)) {
		platform_driver_unregister(&pwec_driver);
		return PTR_ERR(pwec_dev);
	}

	return 0;
}

static void __exit pwec_exit(void)
{
	platform_device_unregister(pwec_dev);
	platform_driver_unregister(&pwec_driver);
}

module_init(pwec_init);
module_exit(pwec_exit);

MODULE_AUTHOR("Yen-Chi Huang <jesse.huang@portwell.com.tw>");
MODULE_DESCRIPTION("Portwell EC Driver");
MODULE_LICENSE("GPL");
