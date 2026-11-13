/* SPDX-License-Identifier: GPL-2.0 */
#ifndef CLOCKSOURCE_VT8500_TIMER_H_
#define CLOCKSOURCE_VT8500_TIMER_H_

#include <linux/auxiliary_bus.h>
#include <linux/types.h>

#define VT8500_TIMER_HZ		3000000

/**
 * struct vt8500_wdt_info - timer resources shared with the watchdog
 * @auxdev: auxiliary device the watchdog driver binds to
 * @timer_next: returns the clocksource counter value @cycles from now
 * @regbase: timer register block; the watchdog uses match register 0
 */
struct vt8500_wdt_info {
	struct auxiliary_device auxdev;
	u64 (*timer_next)(u64 cycles);
	void __iomem *regbase;
};

#endif /* CLOCKSOURCE_VT8500_TIMER_H_ */
