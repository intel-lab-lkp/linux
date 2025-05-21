/* SPDX-License-Identifier: GPL-2.0 */
#ifndef LINUX_VT8500_TIMER_H_
#define LINUX_VT8500_TIMER_H_

#include <linux/auxiliary_bus.h>
#include <linux/io.h>
#include <linux/types.h>

#define VT8500_TIMER_HZ		3000000

struct vt8500_wdt_info {
	struct auxiliary_device auxdev;
	u64 (*timer_next)(u64 cycles);
	void __iomem *wdt_en;
	void __iomem *wdt_match;
};

#endif /* LINUX_VT8500_TIMER_H_ */
