#ifndef _LINUX_SCHED_ISOLATION_H
#define _LINUX_SCHED_ISOLATION_H

#include <linux/cpumask.h>
#include <linux/cpuset.h>
#include <linux/init.h>
#include <linux/tick.h>
#include <linux/sched/housekeeping.h>

static inline bool cpu_is_isolated(int cpu)
{
	return !housekeeping_test_cpu(cpu, HK_TYPE_DOMAIN) ||
	       !housekeeping_test_cpu(cpu, HK_TYPE_TICK) ||
	       cpuset_cpu_is_isolated(cpu);
}

#endif /* _LINUX_SCHED_ISOLATION_H */
