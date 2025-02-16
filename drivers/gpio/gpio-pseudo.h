/* SPDX-License-Identifier: GPL-2.0 */

#ifndef GPIO_PSEUDO_H
#define GPIO_PSEUDO_H

#include <linux/completion.h>
#include <linux/platform_device.h>

struct pseudo_gpio_common {
	struct platform_device *pdev;
	const char *name;

	/* Synchronize with probe */
	struct notifier_block bus_notifier;
	struct completion probe_completion;
	bool driver_bound;
};

void pseudo_gpio_init(struct pseudo_gpio_common *common);
int pseudo_gpio_register(struct pseudo_gpio_common *common,
			 struct platform_device_info *pdevinfo);
void pseudo_gpio_unregister(struct pseudo_gpio_common *common);

#endif /* GPIO_PSEUDO_H */
