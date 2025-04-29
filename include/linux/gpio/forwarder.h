/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __LINUX_GPIO_FORWARDER_H
#define __LINUX_GPIO_FORWARDER_H

#include <linux/gpio/consumer.h>
#include <linux/gpio/driver.h>
#include <linux/mutex.h>
#include <linux/spinlock_types.h>
#include <linux/types.h>

struct gpiochip_fwd;

struct gpio_chip *gpio_fwd_get_gpiochip(struct gpiochip_fwd *fwd);

void *gpio_fwd_get_data(struct gpiochip_fwd *fwd);

int gpio_fwd_request(struct gpio_chip *chip, unsigned int offset);

int gpio_fwd_get_direction(struct gpio_chip *chip, unsigned int offset);

int gpio_fwd_direction_input(struct gpio_chip *chip, unsigned int offset);

int gpio_fwd_direction_output(struct gpio_chip *chip, unsigned int offset,
			      int value);

int gpio_fwd_get(struct gpio_chip *chip, unsigned int offset);

int gpio_fwd_get_multiple_locked(struct gpio_chip *chip, unsigned long *mask,
				 unsigned long *bits);

int gpio_fwd_set(struct gpio_chip *chip, unsigned int offset, int value);

int gpio_fwd_set_multiple_locked(struct gpio_chip *chip, unsigned long *mask,
				 unsigned long *bits);

int gpio_fwd_set_config(struct gpio_chip *chip, unsigned int offset,
			unsigned long config);

int gpio_fwd_to_irq(struct gpio_chip *chip, unsigned int offset);

struct gpiochip_fwd *devm_gpio_fwd_alloc(struct device *dev,
					 unsigned int ngpios);

int gpio_fwd_gpio_add(struct gpiochip_fwd *fwd,
		      struct gpio_desc *desc, unsigned int offset);

void gpio_fwd_gpio_free(struct gpiochip_fwd *fwd, unsigned int offset);

int gpio_fwd_register(struct gpiochip_fwd *fwd, void *data);

#endif
