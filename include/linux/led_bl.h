/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_LED_BL_H
#define _LINUX_LED_BL_H

#include <linux/kconfig.h>

struct device;
struct led_classdev;

#if IS_REACHABLE(CONFIG_BACKLIGHT_LED)
int devm_led_backlight_register(struct device *dev, struct led_classdev *led);
#else
static inline int devm_led_backlight_register(struct device *dev,
					      struct led_classdev *led)
{
	return 0;
}
#endif

#endif /* _LINUX_LED_BL_H */
