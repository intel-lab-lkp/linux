// SPDX-License-Identifier: GPL-2.0-only
/*
 *  GPIO driver for Vortex86 SoCs
 *
 *  Author: Marcos Del Sol Vives <marcos@orca.pet>
 *
 *  Based on the it87xx GPIO driver by Diego Elio Pettenò
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/types.h>
#include <linux/errno.h>
#include <linux/module.h>
#include <linux/ioport.h>
#include <linux/spinlock.h>
#include <linux/gpio/driver.h>
#include <linux/platform_device.h>

#define GPIO_PORTS	5
#define GPIO_PER_PORT	8
#define GPIO_COUNT	(GPIO_PORTS * GPIO_PER_PORT)

#define GPIO_DATA_BASE		0x78
#define GPIO_DIRECTION_BASE	0x98

static struct platform_device *pdev;

static DEFINE_SPINLOCK(gpio_lock);

static int vortex_gpio_get(struct gpio_chip *chip, unsigned int gpio_num)
{
	uint8_t port = gpio_num / GPIO_PER_PORT;
	uint8_t bit  = gpio_num % GPIO_PER_PORT;
	uint8_t val;

	val = inb(GPIO_DATA_BASE + port);
	return !!(val & (1 << bit));
}

static int vortex_gpio_direction_in(struct gpio_chip *chip, unsigned int gpio_num)
{
	uint8_t port = gpio_num / GPIO_PER_PORT;
	uint8_t bit  = gpio_num % GPIO_PER_PORT;
	unsigned long flags;
	uint8_t dir;

	spin_lock_irqsave(&gpio_lock, flags);

	dir = inb(GPIO_DIRECTION_BASE + port);
	dir &= ~(1 << bit); /* 0 = input */
	outb(dir, GPIO_DIRECTION_BASE + port);

	spin_unlock_irqrestore(&gpio_lock, flags);

	return 0;
}

static int vortex_gpio_set(struct gpio_chip *chip, unsigned int gpio_num, int value)
{
	uint8_t port = gpio_num / GPIO_PER_PORT;
	uint8_t bit  = gpio_num % GPIO_PER_PORT;
	unsigned long flags;
	uint8_t dat;

	spin_lock_irqsave(&gpio_lock, flags);

	dat = inb(GPIO_DATA_BASE + port);
	if (value)
		dat |= (1 << bit);
	else
		dat &= ~(1 << bit);
	outb(dat, GPIO_DATA_BASE + port);

	spin_unlock_irqrestore(&gpio_lock, flags);

	return 0;
}

static int vortex_gpio_direction_out(struct gpio_chip *chip, unsigned int gpio_num, int value)
{
	uint8_t port = gpio_num / GPIO_PER_PORT;
	uint8_t bit  = gpio_num % GPIO_PER_PORT;
	unsigned long flags;
	uint8_t dir, dat;

	spin_lock_irqsave(&gpio_lock, flags);

	/* Have to set direction first. Else writes to data are ignored. */
	dir = inb(GPIO_DIRECTION_BASE + port);
	dir |= (1 << bit); /* 1 = output */
	outb(dir, GPIO_DIRECTION_BASE + port);

	dat = inb(GPIO_DATA_BASE + port);
	if (value)
		dat |= (1 << bit);
	else
		dat &= ~(1 << bit);
	outb(dat, GPIO_DATA_BASE + port);

	spin_unlock_irqrestore(&gpio_lock, flags);

	return 0;
}

static char labels[GPIO_COUNT][sizeof("vortex_gpXY")];
static char *labels_table[GPIO_COUNT];

static struct gpio_chip gpio_chip = {
	.label			= KBUILD_MODNAME,
	.owner			= THIS_MODULE,
	.get			= vortex_gpio_get,
	.direction_input	= vortex_gpio_direction_in,
	.set_rv			= vortex_gpio_set,
	.direction_output	= vortex_gpio_direction_out,
	.base			= -1,
	.ngpio			= GPIO_COUNT,
	.names			= (const char * const *)labels_table,
};

static int vortex_gpio_probe(struct platform_device *pdev)
{
	/* Set up GPIO labels */
	for (int i = 0; i < GPIO_COUNT; i++) {
		sprintf(labels[i], "vortex_gp%u%u", i / 8, i % 8);
		labels_table[i] = &labels[i][0];
	}

	return devm_gpiochip_add_data(&pdev->dev, &gpio_chip, NULL);
}

static struct platform_driver vortex_gpio_driver = {
	.driver = {
		.name = KBUILD_MODNAME,
		.owner = THIS_MODULE,
	},
	.probe = vortex_gpio_probe,
};

static struct resource vortex_gpio_resources[] = {
	DEFINE_RES_IO_NAMED(GPIO_DATA_BASE, GPIO_PORTS, KBUILD_MODNAME " data"),
	DEFINE_RES_IO_NAMED(GPIO_DIRECTION_BASE, GPIO_PORTS, KBUILD_MODNAME " dir"),
};

static int __init vortex_gpio_init(void)
{
	if (boot_cpu_data.x86_vendor != X86_VENDOR_VORTEX) {
		pr_err("Not a Vortex86 CPU, refusing to load\n");
		return -ENODEV;
	}

	pdev = platform_create_bundle(&vortex_gpio_driver, vortex_gpio_probe,
			vortex_gpio_resources, ARRAY_SIZE(vortex_gpio_resources),
			NULL, 0);
	return PTR_ERR_OR_ZERO(pdev);
}

static void __exit vortex_gpio_exit(void)
{
	platform_device_unregister(pdev);
	platform_driver_unregister(&vortex_gpio_driver);
}

module_init(vortex_gpio_init);
module_exit(vortex_gpio_exit);

MODULE_AUTHOR("Marcos Del Sol Vives <marcos@orca.pet>");
MODULE_DESCRIPTION("GPIO driver for Vortex86 SoCs");
MODULE_LICENSE("GPL");
