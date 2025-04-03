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
 *  - Hardware watchdog with 1-255 second timeout range
 *
 * It integrates with the Linux GPIO and Watchdog subsystems, allowing
 * userspace interaction with EC GPIO pins and watchdog control,
 * ensuring system stability and configurability.
 *
 * (C) Copyright 2025 Portwell, Inc.
 * Author: Yen-Chi Huang (jesse.huang@portwell.com.tw)
 *
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/gpio/driver.h>
#include <linux/watchdog.h>
#include <linux/io.h>
#include <linux/string.h>

#define PORTWELL_EC_IOSPACE 0xe300
#define PORTWELL_GPIO_PINS 8
#define PORTWELL_GPIO_DIR_REG 0x2b
#define PORTWELL_GPIO_VAL_REG 0x2c

#define PORTWELL_WDT_EC_CONFIG_ADDR    0x06
#define PORTWELL_WDT_EC_COUNT_MIN_ADDR 0x07
#define PORTWELL_WDT_EC_COUNT_SEC_ADDR 0x08
#define PORTWELL_WDT_EC_MAX_COUNT_SECOND 255
#define PORTWELL_EC_FW_VENDOR_ADDRESS  0x4d
#define PORTWELL_EC_FW_VENDOR_LENGTH   3
#define PORTWELL_EC_FW_VENDOR_NAME "PWG"

 /* Functions for access EC via IOSPACE*/
static void pwec_write(u8 index, u8 data)
{
	outb(data, PORTWELL_EC_IOSPACE + index);
}

static u8 pwec_read(u8 address)
{
	return inb(PORTWELL_EC_IOSPACE + address);
}

/* GPIO functions*/
static int pwec_gpio_get(struct gpio_chip *chip, unsigned int offset)
{
	return (pwec_read(PORTWELL_GPIO_VAL_REG) & (1 << offset)) ? 1 : 0;
}

static void pwec_gpio_set(struct gpio_chip *chip, unsigned int offset, int val)
{
	u8 tmp = pwec_read(PORTWELL_GPIO_VAL_REG);

	if (val)
		tmp |= (1 << offset);
	else
		tmp &= ~(1 << offset);
	pwec_write(PORTWELL_GPIO_VAL_REG, tmp);
}

static int pwec_gpio_get_direction(struct gpio_chip *chip, unsigned int offset)
{
	return (pwec_read(PORTWELL_GPIO_DIR_REG) & (1 << offset))
			? GPIO_LINE_DIRECTION_IN : GPIO_LINE_DIRECTION_OUT;
}

static int pwec_gpio_direction_input(struct gpio_chip *gc, unsigned int offset)
{
	/* Changing direction causes issues on some boards, so it's disabled for now. */
	return -EOPNOTSUPP;
}

static int pwec_gpio_direction_output(struct gpio_chip *gc, unsigned int offset, int value)
{
	/* Changing direction causes issues on some boards, so it's disabled for now. */
	return -EOPNOTSUPP;
}

static struct gpio_chip pwec_gpio_chip = {
	.label = "portwell-ec-gpio",
	.get_direction = pwec_gpio_get_direction,
	.direction_input = pwec_gpio_direction_input,
	.direction_output = pwec_gpio_direction_output,
	.get = pwec_gpio_get,
	.set = pwec_gpio_set,
	.ngpio = PORTWELL_GPIO_PINS,
};

/* Watchdog functions*/
static int pwec_wdt_start(struct watchdog_device *wdd)
{
	int retry = 10;
	u8 timeout;

	do {
		pwec_write(PORTWELL_WDT_EC_COUNT_SEC_ADDR, wdd->timeout);
		pwec_write(PORTWELL_WDT_EC_CONFIG_ADDR, 0x01); // WDTCFG[1:0]=01
		timeout = pwec_read(PORTWELL_WDT_EC_COUNT_SEC_ADDR);
		retry--;
	} while (timeout != wdd->timeout && retry > 0);
	pr_info("Portwell EC: Watchdog started with timeout: %d seconds\n", wdd->timeout);
	return (retry > 0) ? 0 : -EIO;
}

static int pwec_wdt_stop(struct watchdog_device *wdd)
{
	pwec_write(PORTWELL_WDT_EC_CONFIG_ADDR, 0x00);
	pr_info("Portwell EC: Watchdog stopped\n");
	return 0;
}

static int pwec_wdt_trigger(struct watchdog_device *wdd)
{
	int retry = 10;
	u8 timeout;

	pr_info("Portwell EC: Watchdog triggered with timeout: %d seconds\n", wdd->timeout);
	do {
		pwec_write(PORTWELL_WDT_EC_COUNT_SEC_ADDR, wdd->timeout);
		pwec_write(PORTWELL_WDT_EC_CONFIG_ADDR, 0x01); // WDTCFG[1:0]=01
		timeout = pwec_read(PORTWELL_WDT_EC_COUNT_SEC_ADDR);
		retry--;
	} while (timeout != wdd->timeout && retry > 0);
	return (retry > 0) ? 0 : -EIO;
}

static int pwec_wdt_set_timeout(struct watchdog_device *wdd, unsigned int timeout)
{
	if (timeout == 0 || timeout > PORTWELL_WDT_EC_MAX_COUNT_SECOND)
		return -EINVAL;
	wdd->timeout = timeout;
	pwec_write(PORTWELL_WDT_EC_COUNT_SEC_ADDR, wdd->timeout);
	pr_info("Portwell EC: Watchdog timeout is set: %d seconds\n", wdd->timeout);
	return 0;
}

static unsigned int pwec_wdt_get_timeleft(struct watchdog_device *wdd)
{
	unsigned int timeout;

	timeout = pwec_read(PORTWELL_WDT_EC_COUNT_SEC_ADDR);
	pr_info("Portwell EC: Watchdog timeout left: %d seconds\n", timeout);
	return timeout;
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
	.identity = "Portwell EC Watchdog",
	},
	.ops = &pwec_wdt_ops,
	.timeout = 10,
	.min_timeout = 1,
	.max_timeout = PORTWELL_WDT_EC_MAX_COUNT_SECOND,
};

static int pwec_firmware_vendor_check(void)
{
	u8 buf[PORTWELL_EC_FW_VENDOR_LENGTH+1];
	u8 i;

	for (i = 0; i < PORTWELL_EC_FW_VENDOR_LENGTH; i++)
		buf[i] = pwec_read(PORTWELL_EC_FW_VENDOR_ADDRESS+i);
	buf[PORTWELL_EC_FW_VENDOR_LENGTH] = '\0';
	return (strcmp(PORTWELL_EC_FW_VENDOR_NAME, buf) == 0) ? 0 : -ENODEV;
}

static int __init pwec_init(void)
{
	int result;

	result = pwec_firmware_vendor_check();
	if (result < 0)
		return result;

	result = gpiochip_add_data(&pwec_gpio_chip, NULL);
	if (result < 0) {
		pr_err("Failed to register Portwell EC GPIO\n");
		return result;
	}

	result = watchdog_register_device(&ec_wdt_dev);
	if (result < 0) {
		gpiochip_remove(&pwec_gpio_chip);
		pr_err("Failed to register Portwell EC Watchdog\n");
		return result;
	}

	pr_info("Portwell EC driver initialized\n");
	return 0;
}

static void __exit pwec_exit(void)
{
	watchdog_unregister_device(&ec_wdt_dev);
	gpiochip_remove(&pwec_gpio_chip);
	pr_info("Portwell EC driver removed\n");
}

module_init(pwec_init);
module_exit(pwec_exit);

MODULE_AUTHOR("Yen-Chi Huang");
MODULE_DESCRIPTION("Portwell EC Driver");
MODULE_LICENSE("GPL");
