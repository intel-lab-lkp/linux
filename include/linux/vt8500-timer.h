/* SPDX-License-Identifier: GPL-2.0 */
#ifndef LINUX_VT8500_TIMER_H_
#define LINUX_VT8500_TIMER_H_

#include <linux/auxiliary_bus.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/types.h>

#define VT8500_TIMER_HZ		3000000

#define TIMER_MATCH_REG(x)	(4 * (x))
#define TIMER_COUNT_REG		0x0010	 /* clocksource counter */

#define TIMER_STATUS_REG	0x0014
#define TIMER_STATUS_MATCH(x)	BIT((x))
#define TIMER_STATUS_CLEARALL	(TIMER_STATUS_MATCH(0) | \
				 TIMER_STATUS_MATCH(1) | \
				 TIMER_STATUS_MATCH(2) | \
				 TIMER_STATUS_MATCH(3))

#define TIMER_WATCHDOG_EN_REG	0x0018
#define TIMER_WD_EN		BIT(0)

#define TIMER_INT_EN_REG	0x001c	 /* interrupt enable */
#define TIMER_INT_EN_MATCH(x)	BIT((x))

#define TIMER_CTRL_REG		0x0020
#define TIMER_CTRL_ENABLE	BIT(0)	 /* enable clocksource counter */
#define TIMER_CTRL_RD_REQ	BIT(1)	 /* request counter read */

#define TIMER_ACC_STS_REG	0x0024	 /* access status */
#define TIMER_ACC_WR_MATCH(x)	BIT((x)) /* writing Match (x) value */
#define TIMER_ACC_WR_COUNTER	BIT(4)	 /* writing clocksource counter */
#define TIMER_ACC_RD_COUNTER	BIT(5)	 /* reading clocksource counter */

#define msecs_to_loops(t) (loops_per_jiffy / 1000 * HZ * t)

#define MIN_OSCR_DELTA		16

struct vt8500_wdt_info {
	struct auxiliary_device auxdev;
	u64 (*timer_next)(u64 cycles);
	void __iomem *wdt_en;
	void __iomem *wdt_match;
};

#endif /* LINUX_VT8500_TIMER_H_ */
