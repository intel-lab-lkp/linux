/*
 * governor.c - governor support
 *
 * (C) 2006-2007 Venkatesh Pallipadi <venkatesh.pallipadi@intel.com>
 *               Shaohua Li <shaohua.li@intel.com>
 *               Adam Belay <abelay@novell.com>
 *
 * This code is licenced under the GPL.
 */

#include <linux/atomic.h>
#include <linux/cpu.h>
#include <linux/cpuidle.h>
#include <linux/init.h>
#include <linux/mutex.h>
#include <linux/module.h>
#include <linux/notifier.h>
#include <linux/pm_qos.h>

#include "cpuidle.h"

char param_governor[CPUIDLE_NAME_LEN];

LIST_HEAD(cpuidle_governors);
struct cpuidle_governor *cpuidle_curr_governor;
struct cpuidle_governor *cpuidle_prev_governor;

/*
 * Per-CPU generation bumped to invalidate that CPU's cached latency
 * constraint.  Global QoS changes invalidate every CPU; per-CPU resume
 * latency changes invalidate only the affected CPU.
 *
 * Start at 1 so it does not match a zero-filled latency_req_cache and
 * falsely hit before the first real fill (which would return 0).
 */
static DEFINE_PER_CPU(atomic_t, latency_req_gen) = ATOMIC_INIT(1);

struct cpuidle_latency_req_cache {
	unsigned int gen;
	s64 latency_ns;
};

struct cpuidle_cpu_qos_nb {
	struct notifier_block nb;
	unsigned int cpu;
};

static DEFINE_PER_CPU(struct cpuidle_latency_req_cache, latency_req_cache);
static DEFINE_PER_CPU(struct cpuidle_cpu_qos_nb, cpuidle_cpu_qos_nb);

static void cpuidle_latency_req_invalidate_cpu(unsigned int cpu)
{
	atomic_inc(per_cpu_ptr(&latency_req_gen, cpu));
}

static void cpuidle_latency_req_invalidate_all(void)
{
	unsigned int cpu;

	for_each_possible_cpu(cpu)
		cpuidle_latency_req_invalidate_cpu(cpu);
}

static int cpuidle_global_qos_notify(struct notifier_block *nb,
				     unsigned long action, void *data)
{
	cpuidle_latency_req_invalidate_all();
	return NOTIFY_OK;
}

static struct notifier_block cpuidle_latency_qos_nb = {
	.notifier_call = cpuidle_global_qos_notify,
};

#ifdef CONFIG_PM_QOS_CPU_SYSTEM_WAKEUP
static struct notifier_block cpuidle_wakeup_qos_nb = {
	.notifier_call = cpuidle_global_qos_notify,
};
#endif

static int cpuidle_cpu_qos_notify(struct notifier_block *nb,
				 unsigned long action, void *data)
{
	struct cpuidle_cpu_qos_nb *qos_nb =
		container_of(nb, struct cpuidle_cpu_qos_nb, nb);

	cpuidle_latency_req_invalidate_cpu(qos_nb->cpu);
	return NOTIFY_OK;
}

int cpuidle_latency_req_notifier_register(unsigned int cpu)
{
	struct device *device = get_cpu_device(cpu);
	struct cpuidle_cpu_qos_nb *qos_nb =
		per_cpu_ptr(&cpuidle_cpu_qos_nb, cpu);

	if (!device)
		return -ENODEV;

	qos_nb->cpu = cpu;
	qos_nb->nb.notifier_call = cpuidle_cpu_qos_notify;
	return dev_pm_qos_add_notifier(device, &qos_nb->nb,
				       DEV_PM_QOS_RESUME_LATENCY);
}

void cpuidle_latency_req_notifier_unregister(unsigned int cpu)
{
	struct device *device = get_cpu_device(cpu);
	struct cpuidle_cpu_qos_nb *qos_nb =
		per_cpu_ptr(&cpuidle_cpu_qos_nb, cpu);

	if (!device)
		return;

	dev_pm_qos_remove_notifier(device, &qos_nb->nb,
				   DEV_PM_QOS_RESUME_LATENCY);
}

static int __init cpuidle_latency_req_init(void)
{
	int ret;

	ret = cpu_latency_qos_add_notifier(&cpuidle_latency_qos_nb);
	if (ret)
		return ret;

#ifdef CONFIG_PM_QOS_CPU_SYSTEM_WAKEUP
	ret = cpu_wakeup_latency_qos_add_notifier(&cpuidle_wakeup_qos_nb);
	if (ret) {
		cpu_latency_qos_remove_notifier(&cpuidle_latency_qos_nb);
		return ret;
	}
#endif
	return 0;
}
core_initcall(cpuidle_latency_req_init);

/**
 * cpuidle_find_governor - finds a governor of the specified name
 * @str: the name
 *
 * Must be called with cpuidle_lock acquired.
 */
struct cpuidle_governor *cpuidle_find_governor(const char *str)
{
	struct cpuidle_governor *gov;

	list_for_each_entry(gov, &cpuidle_governors, governor_list)
		if (!strncasecmp(str, gov->name, CPUIDLE_NAME_LEN))
			return gov;

	return NULL;
}

/**
 * cpuidle_switch_governor - changes the governor
 * @gov: the new target governor
 * Must be called with cpuidle_lock acquired.
 */
int cpuidle_switch_governor(struct cpuidle_governor *gov)
{
	struct cpuidle_device *dev;

	if (!gov)
		return -EINVAL;

	if (gov == cpuidle_curr_governor)
		return 0;

	cpuidle_uninstall_idle_handler();

	if (cpuidle_curr_governor) {
		list_for_each_entry(dev, &cpuidle_detected_devices, device_list)
			cpuidle_disable_device(dev);
	}

	cpuidle_curr_governor = gov;

	list_for_each_entry(dev, &cpuidle_detected_devices, device_list)
		cpuidle_enable_device(dev);

	cpuidle_install_idle_handler();
	pr_info("cpuidle: using governor %s\n", gov->name);

	return 0;
}

/**
 * cpuidle_register_governor - registers a governor
 * @gov: the governor
 */
int cpuidle_register_governor(struct cpuidle_governor *gov)
{
	int ret = -EEXIST;

	if (!gov || !gov->select)
		return -EINVAL;

	if (cpuidle_disabled())
		return -ENODEV;

	mutex_lock(&cpuidle_lock);
	if (cpuidle_find_governor(gov->name) == NULL) {
		ret = 0;
		list_add_tail(&gov->governor_list, &cpuidle_governors);
		if (!cpuidle_curr_governor ||
		    !strncasecmp(param_governor, gov->name, CPUIDLE_NAME_LEN) ||
		    (cpuidle_curr_governor->rating < gov->rating &&
		     strncasecmp(param_governor, cpuidle_curr_governor->name,
				 CPUIDLE_NAME_LEN)))
			cpuidle_switch_governor(gov);
	}
	mutex_unlock(&cpuidle_lock);

	return ret;
}

/**
 * cpuidle_aggregate_latency_req - Aggregate QoS latency constraints for @cpu
 * @cpu: Target CPU
 *
 * Combine the per-CPU resume latency with the global CPU latency and wakeup
 * latency QoS limits.  Called on a cache miss from
 * cpuidle_governor_latency_req().
 */
static s64 cpuidle_aggregate_latency_req(unsigned int cpu)
{
	struct device *device = get_cpu_device(cpu);
	int device_req = dev_pm_qos_raw_resume_latency(device);
	int global_req = cpu_latency_qos_limit();
	int global_wake_req = cpu_wakeup_latency_qos_limit();

	if (global_req > global_wake_req)
		global_req = global_wake_req;

	if (device_req > global_req)
		device_req = global_req;

	return (s64)device_req * NSEC_PER_USEC;
}

/**
 * cpuidle_governor_latency_req - Cached latency constraint for CPU
 * @cpu: Target CPU (must be the local CPU; callers are idle governors)
 *
 * The per-CPU cache is only accessed by this CPU's idle ->select() path.
 * Remote CPUs may bump latency_req_gen via QoS notifiers, but they do not
 * touch the cache fields.
 */
s64 cpuidle_governor_latency_req(unsigned int cpu)
{
	struct cpuidle_latency_req_cache *cache;
	unsigned int gen;
	s64 latency_ns;

	cache = per_cpu_ptr(&latency_req_cache, cpu);
	gen = atomic_read(per_cpu_ptr(&latency_req_gen, cpu));

	if (likely(cache->gen == gen))
		return cache->latency_ns;

	latency_ns = cpuidle_aggregate_latency_req(cpu);
	cache->latency_ns = latency_ns;
	cache->gen = gen;

	return latency_ns;
}
