// SPDX-License-Identifier: GPL-2.0-only
/*
 *  GPIO driver for Vortex86 SoCs
 *
 *  Author: Marcos Del Sol Vives <marcos@orca.pet>
 *
 *  Based on the it87xx GPIO driver by Diego Elio Pettenò
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/errno.h>
#include <linux/ioport.h>
#include <linux/gpio/driver.h>
#include <linux/spinlock.h>

#define GPIO_PORTS	5
#define GPIO_PER_PORT	8
#define GPIO_COUNT	(GPIO_PORTS * GPIO_PER_PORT)

#define GPIO_DATA_BASE		0x78
#define GPIO_DIRECTION_BASE	0x98

static DEFINE_SPINLOCK(gpio_lock);

static int vortex86_gpio_get(struct gpio_chip *chip, unsigned int gpio_num)
{
	uint8_t port = gpio_num / GPIO_PER_PORT;
	uint8_t bit  = gpio_num % GPIO_PER_PORT;
	uint8_t val;

	val = inb(GPIO_DATA_BASE + port);
	return !!(val & (1 << bit));
}

static int vortex86_gpio_direction_in(struct gpio_chip *chip, unsigned int gpio_num)
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

static void vortex86_gpio_set(struct gpio_chip *chip, unsigned int gpio_num, int value)
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
}

static int vortex86_gpio_direction_out(struct gpio_chip *chip, unsigned int gpio_num, int value)
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

static char labels[GPIO_COUNT][sizeof("vortex86_gpXY")];
static char *labels_table[GPIO_COUNT];

static struct gpio_chip gpio_chip = {
	.label			= KBUILD_MODNAME,
	.owner			= THIS_MODULE,
	.get			= vortex86_gpio_get,
	.direction_input	= vortex86_gpio_direction_in,
	.set			= vortex86_gpio_set,
	.direction_output	= vortex86_gpio_direction_out,
	.base			= -1,
	.ngpio			= GPIO_COUNT,
	.names			= (const char * const *)labels_table,
};

static int __init vortex86_gpio_init(void)
{
	int rc = 0, i;

	if (boot_cpu_data.x86_vendor != X86_VENDOR_VORTEX) {
		pr_err("Not a Vortex86 CPU, refusing to load\n");
		return -ENODEV;
	}

	/* Request I/O regions for data and direction registers */
	if (!request_region(GPIO_DATA_BASE, GPIO_PORTS, KBUILD_MODNAME))
		return -EBUSY;
	if (!request_region(GPIO_DIRECTION_BASE, GPIO_PORTS, KBUILD_MODNAME)) {
		release_region(GPIO_DATA_BASE, GPIO_PORTS);
		return -EBUSY;
	}

	/* Set up GPIO labels */
	for (i = 0; i < GPIO_COUNT; i++) {
		sprintf(labels[i], "vortex86_gp%u%u", i / 8, i % 8);
		labels_table[i] = &labels[i][0];
	}

	rc = gpiochip_add_data(&gpio_chip, &gpio_chip);
	if (rc) {
		release_region(GPIO_DATA_BASE, GPIO_PORTS);
		release_region(GPIO_DIRECTION_BASE, GPIO_PORTS);
		return rc;
	}

	return 0;
}

static void __exit vortex86_gpio_exit(void)
{
	gpiochip_remove(&gpio_chip);
	release_region(GPIO_DATA_BASE, GPIO_PORTS);
	release_region(GPIO_DIRECTION_BASE, GPIO_PORTS);
}

module_init(vortex86_gpio_init);
module_exit(vortex86_gpio_exit);

MODULE_AUTHOR("Marcos Del Sol Vives <marcos@orca.pet>");
MODULE_DESCRIPTION("GPIO driver for Vortex86 SoCs");
MODULE_LICENSE("GPL");
