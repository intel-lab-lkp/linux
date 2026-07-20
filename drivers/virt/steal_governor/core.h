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
#include <linux/kconfig.h>
#include <linux/kernel_stat.h>
#include <linux/topology.h>
#include <linux/sched/isolation.h>
#include <linux/cleanup.h>
#include <linux/math64.h>

struct steal_governor {
	struct delayed_work	work;
	ktime_t			time;
	u64			steal;
	unsigned int		interval_ms;
	unsigned int		high_threshold;
	unsigned int		low_threshold;
};

#endif /* __VIRT_STEAL_CORE_H */
