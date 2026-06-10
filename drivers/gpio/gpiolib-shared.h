/* SPDX-License-Identifier: GPL-2.0 */

#ifndef __LINUX_GPIO_SHARED_H
#define __LINUX_GPIO_SHARED_H

#include <linux/cleanup.h>
#include <linux/lockdep.h>
#include <linux/mutex.h>

struct gpio_device;
struct gpio_desc;
struct device;
struct fwnode_handle;

#if IS_ENABLED(CONFIG_GPIO_SHARED)

int gpiochip_setup_shared(struct gpio_chip *gc);
void gpio_device_teardown_shared(struct gpio_device *gdev);
int gpio_shared_add_proxy_lookup(struct device *consumer,
				 struct fwnode_handle *fwnode,
				 const char *con_id, unsigned long lflags);

#else

static inline int gpiochip_setup_shared(struct gpio_chip *gc)
{
	return 0;
}

static inline void gpio_device_teardown_shared(struct gpio_device *gdev) { }

static inline int gpio_shared_add_proxy_lookup(struct device *consumer,
					       struct fwnode_handle *fwnode,
					       const char *con_id,
					       unsigned long lflags)
{
	return 0;
}

#endif /* CONFIG_GPIO_SHARED */

struct gpio_shared_desc {
	struct gpio_desc *desc;
	unsigned long cfg;
	unsigned int usecnt;
	unsigned int highcnt;
	struct mutex mutex; /* serializes all proxy operations on this descriptor */
};

struct gpio_shared_desc *devm_gpiod_shared_get(struct device *dev);

/*
 * Under this lock the proxy may call gpiod_set_config()/gpiod_direction_*(),
 * which can reach pinctrl paths that take a mutex (e.g. gpiod_set_config() ->
 * gpiochip_generic_config() -> pinctrl_gpio_set_config()), independent of the
 * underlying chip's can_sleep. A spinlock would run that sleeping call from
 * atomic context, so the descriptor lock must be a mutex and the proxy
 * gpiochip is therefore sleeping (can_sleep=true).
 */
DEFINE_LOCK_GUARD_1(gpio_shared_desc_lock, struct gpio_shared_desc,
	mutex_lock(&_T->lock->mutex),
	mutex_unlock(&_T->lock->mutex))

static inline void gpio_shared_lockdep_assert(struct gpio_shared_desc *shared_desc)
{
	lockdep_assert_held(&shared_desc->mutex);
}

#endif /* __LINUX_GPIO_SHARED_H */
