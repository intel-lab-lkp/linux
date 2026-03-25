#ifndef _LINUX_SCHED_ISOLATION_H
#define _LINUX_SCHED_ISOLATION_H

#include <linux/cpumask.h>
#include <linux/cpuset.h>
#include <linux/init.h>
#include <linux/tick.h>
#include <linux/notifier.h>

enum hk_type {
	HK_TYPE_DOMAIN,
	HK_TYPE_MANAGED_IRQ,
	HK_TYPE_TICK,
	HK_TYPE_TIMER,
	HK_TYPE_RCU,
	HK_TYPE_MISC,
	HK_TYPE_WQ,
	HK_TYPE_KTHREAD,
	HK_TYPE_MAX,

};

#define HK_TYPE_KERNEL_NOISE HK_TYPE_TICK

struct housekeeping_update {
	enum hk_type type;
	const struct cpumask *new_mask;
};

#define HK_UPDATE_MASK	0x01

#ifdef CONFIG_CPU_ISOLATION
DECLARE_STATIC_KEY_FALSE(housekeeping_overridden);
extern int housekeeping_any_cpu(enum hk_type type);
extern const struct cpumask *housekeeping_cpumask(enum hk_type type);
extern bool housekeeping_enabled(enum hk_type type);
extern void housekeeping_affine(struct task_struct *t, enum hk_type type);
extern bool housekeeping_test_cpu(int cpu, enum hk_type type);
extern void __init housekeeping_init(void);

extern int housekeeping_register_notifier(struct notifier_block *nb);
extern int housekeeping_unregister_notifier(struct notifier_block *nb);

#else

static inline int housekeeping_any_cpu(enum hk_type type)
{
	return smp_processor_id();
}

static inline const struct cpumask *housekeeping_cpumask(enum hk_type type)
{
	return cpu_possible_mask;
}

static inline bool housekeeping_enabled(enum hk_type type)
{
	return false;
}

static inline void housekeeping_affine(struct task_struct *t,
				       enum hk_type type) { }

static inline bool housekeeping_test_cpu(int cpu, enum hk_type type)
{
	return true;
}

static inline void housekeeping_init(void) { }

static inline int housekeeping_register_notifier(struct notifier_block *nb)
{
	return 0;
}

static inline int housekeeping_unregister_notifier(struct notifier_block *nb)
{
	return 0;
}
#endif /* CONFIG_CPU_ISOLATION */

static inline bool housekeeping_cpu(int cpu, enum hk_type type)
{
#ifdef CONFIG_CPU_ISOLATION
	if (static_branch_unlikely(&housekeeping_overridden))
		return housekeeping_test_cpu(cpu, type);
#endif
	return true;
}

static inline bool cpu_is_isolated(int cpu)
{
	return !housekeeping_test_cpu(cpu, HK_TYPE_DOMAIN) ||
	       !housekeeping_test_cpu(cpu, HK_TYPE_TICK) ||
	       cpuset_cpu_is_isolated(cpu);
}

#endif /* _LINUX_SCHED_ISOLATION_H */
