// SPDX-License-Identifier: GPL-2.0
/*
 * Renesas Technology Europe RSK+ 7203 Support.
 *
 * Copyright (C) 2008 - 2010  Paul Mundt
 */
#include <linux/err.h>
#include <linux/init.h>
#include <linux/types.h>
#include <linux/platform_device.h>
#include <linux/interrupt.h>
#include <linux/smsc911x.h>
#include <linux/input.h>
#include <linux/io.h>
#include <linux/gpio.h>
#include <linux/gpio/machine.h>
#include <linux/gpio/property.h>
#include <asm/machvec.h>
#include <cpu/pfc.h>
#include <cpu/sh7203.h>

static struct smsc911x_platform_config smsc911x_config = {
	.phy_interface	= PHY_INTERFACE_MODE_MII,
	.irq_polarity	= SMSC911X_IRQ_POLARITY_ACTIVE_LOW,
	.irq_type	= SMSC911X_IRQ_TYPE_OPEN_DRAIN,
	.flags		= SMSC911X_USE_32BIT | SMSC911X_SWAP_FIFO,
};

static struct resource smsc911x_resources[] = {
	[0] = {
		.start		= 0x24000000,
		.end		= 0x240000ff,
		.flags		= IORESOURCE_MEM,
	},
	[1] = {
		.start		= 64,
		.end		= 64,
		.flags		= IORESOURCE_IRQ,
	},
};

static const struct software_node rsk7203_gpio_leds_node = {
	.name = "rsk7203-gpio-leds",
};

static const struct software_node rsk7203_green_led_node = {
	.name = "green",
	.parent = &rsk7203_gpio_leds_node,
	.properties = (const struct property_entry[]) {
		PROPERTY_ENTRY_STRING("label", "green"),
		PROPERTY_ENTRY_GPIO("gpios", &pfc_gpiochip_node,
				    GPIO_PE10, GPIO_ACTIVE_LOW),
		{ }
	},
};

static const struct software_node rsk7203_orange_led_node = {
	.name = "orange",
	.parent = &rsk7203_gpio_leds_node,
	.properties = (const struct property_entry[]) {
		PROPERTY_ENTRY_STRING("label", "orange"),
		PROPERTY_ENTRY_STRING("linux,default-trigger", "nand-disk"),
		PROPERTY_ENTRY_GPIO("gpios", &pfc_gpiochip_node,
				    GPIO_PE12, GPIO_ACTIVE_LOW),
		{ }
	},
};

static const struct software_node rsk7203_red1_led_node = {
	.name = "red:timer",
	.parent = &rsk7203_gpio_leds_node,
	.properties = (const struct property_entry[]) {
		PROPERTY_ENTRY_STRING("label", "red:timer"),
		PROPERTY_ENTRY_STRING("linux,default-trigger", "timer"),
		PROPERTY_ENTRY_GPIO("gpios", &pfc_gpiochip_node,
				    GPIO_PC14, GPIO_ACTIVE_LOW),
		{ }
	},
};

static const struct software_node rsk7203_red2_led_node = {
	.name = "red:heartbeat",
	.parent = &rsk7203_gpio_leds_node,
	.properties = (const struct property_entry[]) {
		PROPERTY_ENTRY_STRING("label", "red:heartbeat"),
		PROPERTY_ENTRY_STRING("linux,default-trigger", "heartbeat"),
		PROPERTY_ENTRY_GPIO("gpios", &pfc_gpiochip_node,
				    GPIO_PE11, GPIO_ACTIVE_LOW),
		{ }
	},
};

static const struct software_node rsk7203_gpio_keys_node = {
	.name = "rsk7203-gpio-keys",
	.properties = (const struct property_entry[]) {
		PROPERTY_ENTRY_U32("poll-interval", 50),
		{ }
	},
};

static const struct software_node rsk7203_sw1_key_node = {
	.parent = &rsk7203_gpio_keys_node,
	.properties = (const struct property_entry[]) {
		PROPERTY_ENTRY_U32("linux,code", BTN_0),
		PROPERTY_ENTRY_GPIO("gpios", &pfc_gpiochip_node,
				    GPIO_PB0, GPIO_ACTIVE_LOW),
		PROPERTY_ENTRY_STRING("label", "SW1"),
		{ }
	},
};

static const struct software_node rsk7203_sw2_key_node = {
	.parent = &rsk7203_gpio_keys_node,
	.properties = (const struct property_entry[]) {
		PROPERTY_ENTRY_U32("linux,code", BTN_1),
		PROPERTY_ENTRY_GPIO("gpios", &pfc_gpiochip_node,
				    GPIO_PB1, GPIO_ACTIVE_LOW),
		PROPERTY_ENTRY_STRING("label", "SW2"),
		{ }
	},
};

static const struct software_node rsk7203_sw3_key_node = {
	.parent = &rsk7203_gpio_keys_node,
	.properties = (const struct property_entry[]) {
		PROPERTY_ENTRY_U32("linux,code", BTN_2),
		PROPERTY_ENTRY_GPIO("gpios", &pfc_gpiochip_node,
				    GPIO_PB2, GPIO_ACTIVE_LOW),
		PROPERTY_ENTRY_STRING("label", "SW3"),
		{ }
	},
};

static const struct software_node * const rsk7203_swnodes[] __initconst = {
	&rsk7203_gpio_leds_node,
	&rsk7203_green_led_node,
	&rsk7203_orange_led_node,
	&rsk7203_red1_led_node,
	&rsk7203_red2_led_node,
	&rsk7203_gpio_keys_node,
	&rsk7203_sw1_key_node,
	&rsk7203_sw2_key_node,
	&rsk7203_sw3_key_node,
	NULL
};

static const struct platform_device_info rsk7203_devices[] __initconst = {
	{
		.name		= "smsc911x",
		.id		= PLATFORM_DEVID_NONE,
		.res		= smsc911x_resources,
		.num_res	= ARRAY_SIZE(smsc911x_resources),
		.data		= &smsc911x_config,
		.size_data	= sizeof(smsc911x_config),
	},
	{
		.name		= "leds-gpio",
		.id		= PLATFORM_DEVID_NONE,
		.swnode		= &rsk7203_gpio_leds_node,
	},
	{
		.name		= "gpio-keys-polled",
		.id		= PLATFORM_DEVID_NONE,
		.swnode		= &rsk7203_gpio_keys_node,
	},
};

static int __init rsk7203_devices_setup(void)
{
	struct platform_device *pd;
	int error;
	int i;

	/* Select pins for SCIF0 */
	gpio_request(GPIO_FN_TXD0, NULL);
	gpio_request(GPIO_FN_RXD0, NULL);

	/* Setup LAN9118: CS1 in 16-bit Big Endian Mode, IRQ0 at Port B */
	__raw_writel(0x36db0400, 0xfffc0008); /* CS1BCR */
	gpio_request(GPIO_FN_IRQ0_PB, NULL);

	error = software_node_register_node_group(rsk7203_swnodes);
	if (error) {
		pr_err("failed to register software nodes: %d\n", error);
		return error;
	}

	for (i = 0; i < ARRAY_SIZE(rsk7203_devices); i++) {
		pd = platform_device_register_full(&rsk7203_devices[i]);
		error = PTR_ERR_OR_ZERO(pd);
		if (error) {
			pr_err("failed to create platform device %s: %d\n",
			       rsk7203_devices[i].name, error);
			return error;
		}
	}

	return 0;
}
device_initcall(rsk7203_devices_setup);
