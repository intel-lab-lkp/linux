/* SPDX-License-Identifier: GPL-2.0 */
/*
 * include/linux/cpuhplock.h - CPU hotplug locking
 *
 * Locking functions for CPU hotplug.
 */
#ifndef _LINUX_CPUHPLOCK_H_
#define _LINUX_CPUHPLOCK_H_

#include <linux/cleanup.h>
#include <linux/errno.h>
#include <linux/cpumask_types.h>

typedef int (*cpuhp_cb_t)(void *arg);
struct device;

extern int lockdep_is_cpus_held(void);
extern int lockdep_is_cpus_write_held(void);

#ifdef CONFIG_HOTPLUG_CPU
void cpus_write_lock(void);
void cpus_write_unlock(void);
void cpus_read_lock(void);
void cpus_read_unlock(void);
int  cpus_read_trylock(void);
void lockdep_assert_cpus_held(void);
void cpu_hotplug_disable_offlining(void);
void cpu_hotplug_disable(void);
void cpu_hotplug_enable(void);
void clear_tasks_mm_cpumask(int cpu);
int remove_cpu(unsigned int cpu);
int cpu_device_down(struct device *dev);
void smp_shutdown_nonboot_cpus(unsigned int primary_cpu);
int cpuhp_offline_cb(struct cpumask *mask, cpuhp_cb_t func, void *arg);
extern bool cpuhp_offline_cb_mode;

#else /* CONFIG_HOTPLUG_CPU */

static inline void cpus_write_lock(void) { }
static inline void cpus_write_unlock(void) { }
static inline void cpus_read_lock(void) { }
static inline void cpus_read_unlock(void) { }
static inline int  cpus_read_trylock(void) { return true; }
static inline void lockdep_assert_cpus_held(void) { }
static inline void cpu_hotplug_disable_offlining(void) { }
static inline void cpu_hotplug_disable(void) { }
static inline void cpu_hotplug_enable(void) { }
static inline int remove_cpu(unsigned int cpu) { return -EPERM; }
static inline void smp_shutdown_nonboot_cpus(unsigned int primary_cpu) { }
static inline int cpuhp_offline_cb(struct cpumask *mask, cpuhp_cb_t func, void *arg)
{
	return -EPERM;
}
#define cpuhp_offline_cb_mode	false
#endif	/* !CONFIG_HOTPLUG_CPU */

DEFINE_LOCK_GUARD_0(cpus_read_lock, cpus_read_lock(), cpus_read_unlock())

#endif /* _LINUX_CPUHPLOCK_H_ */
