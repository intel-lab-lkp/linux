/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __VIRT_STEAL_CORE_H
#define __VIRT_STEAL_CORE_H

#include <linux/types.h>

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/cpuhplock.h>
#include <linux/cpumask.h>
#include <linux/workqueue.h>
#include <linux/ktime.h>

struct steal_monitor {
	struct delayed_work	work;
	u64			prev_steal;
	int			prev_direction;
	unsigned int		interval_ms;
	unsigned int		high_threshold;
	unsigned int		low_threshold;
	ktime_t			prev_time;
};

extern struct steal_monitor sm_core_ctx;

#endif /* __VIRT_STEAL_CORE_H */
