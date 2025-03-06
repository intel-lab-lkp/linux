/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_BH_H
#define _LINUX_BH_H

#include <linux/instruction_pointer.h>
#include <linux/preempt.h>
#include <linux/lockdep.h>

#if defined(CONFIG_PREEMPT_RT) || defined(CONFIG_TRACE_IRQFLAGS)
extern void __local_bh_disable_ip(unsigned long ip, unsigned int cnt);
#else
static __always_inline void __local_bh_disable_ip(unsigned long ip, unsigned int cnt)
{
	preempt_count_add(cnt);
	barrier();
}
#endif

#ifdef CONFIG_DEBUG_LOCK_ALLOC
extern struct lockdep_map bh_lock_map;
#endif

static inline void local_bh_disable(void)
{
	lock_map_acquire_read(&bh_lock_map);
	__local_bh_disable_ip(_THIS_IP_, SOFTIRQ_DISABLE_OFFSET);
}

extern void _local_bh_enable(void);
extern void __local_bh_enable_ip(unsigned long ip, unsigned int cnt);

static inline void local_bh_enable_ip(unsigned long ip)
{
	lock_map_release(&bh_lock_map);
	__local_bh_enable_ip(ip, SOFTIRQ_DISABLE_OFFSET);
}

static inline void local_bh_enable(void)
{
	lock_map_release(&bh_lock_map);
	__local_bh_enable_ip(_THIS_IP_, SOFTIRQ_DISABLE_OFFSET);
}

#ifdef CONFIG_PREEMPT_RT
extern bool local_bh_blocked(void);
#else
static inline bool local_bh_blocked(void) { return false; }
#endif

#endif /* _LINUX_BH_H */
