/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_CONTEXT_TRACKING_WORK_H
#define _LINUX_CONTEXT_TRACKING_WORK_H

#include <linux/bitops.h>

enum {
	CONTEXT_WORK_n_OFFSET,
	CONTEXT_WORK_MAX_OFFSET
};

enum ct_work {
	CONTEXT_WORK_n        = BIT(CONTEXT_WORK_n_OFFSET),
	CONTEXT_WORK_MAX      = BIT(CONTEXT_WORK_MAX_OFFSET)
};

#include <asm/context_tracking_work.h>

#ifdef CONFIG_CONTEXT_TRACKING_WORK
extern bool ct_set_cpu_work(unsigned int cpu, unsigned int work);
#else
static inline bool
ct_set_cpu_work(unsigned int cpu, unsigned int work) { return false; }
#endif

#endif
