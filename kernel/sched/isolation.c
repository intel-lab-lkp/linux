// SPDX-License-Identifier: GPL-2.0-only
/*
 *  Housekeeping management. Manage the targets for routine code that can run on
 *  any CPU: unbound workqueues, timers, kthreads and any offloadable work.
 *
 * Copyright (C) 2017 Red Hat, Inc., Frederic Weisbecker
 * Copyright (C) 2017-2018 SUSE, Frederic Weisbecker
 *
 */
#include <linux/sched/isolation.h>
#include <linux/capability.h>
#include <linux/mutex.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/slab.h>
#include <linux/ctype.h>
#include <linux/notifier.h>
#include <linux/topology.h>
#include "sched.h"

enum hk_flags {
	HK_FLAG_DOMAIN		= BIT(HK_TYPE_DOMAIN),
	HK_FLAG_MANAGED_IRQ	= BIT(HK_TYPE_MANAGED_IRQ),
	HK_FLAG_TICK		= BIT(HK_TYPE_TICK),
	HK_FLAG_TIMER		= BIT(HK_TYPE_TIMER),
	HK_FLAG_RCU		= BIT(HK_TYPE_RCU),
	HK_FLAG_MISC		= BIT(HK_TYPE_MISC),
	HK_FLAG_WQ		= BIT(HK_TYPE_WQ),
	HK_FLAG_KTHREAD		= BIT(HK_TYPE_KTHREAD),
};

#define HK_FLAG_KERNEL_NOISE (HK_FLAG_TICK | HK_FLAG_TIMER | HK_FLAG_RCU | \
			      HK_FLAG_MISC | HK_FLAG_WQ | HK_FLAG_KTHREAD)

static DEFINE_MUTEX(housekeeping_mutex);
static BLOCKING_NOTIFIER_HEAD(housekeeping_notifier_list);
DEFINE_STATIC_KEY_FALSE(housekeeping_overridden);
EXPORT_SYMBOL_GPL(housekeeping_overridden);

struct housekeeping {
	cpumask_var_t cpumasks[HK_TYPE_MAX];
	unsigned long flags;
};

static struct housekeeping housekeeping;
static bool housekeeping_smt_aware;

static ssize_t smt_aware_show(struct kobject *kobj,
			     struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%d\n", housekeeping_smt_aware);
}

static ssize_t smt_aware_store(struct kobject *kobj,
			      struct kobj_attribute *attr,
			      const char *buf, size_t count)
{
	bool val;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;

	if (kstrtobool(buf, &val))
		return -EINVAL;

	housekeeping_smt_aware = val;

	return count;
}

static struct kobj_attribute smt_aware_attr =
	__ATTR(smt_aware_mode, 0600, smt_aware_show, smt_aware_store);

bool housekeeping_enabled(enum hk_type type)
{
	return !!(housekeeping.flags & BIT(type));
}
EXPORT_SYMBOL_GPL(housekeeping_enabled);

int housekeeping_any_cpu(enum hk_type type)
{
	int cpu;

	if (static_branch_unlikely(&housekeeping_overridden)) {
		if (housekeeping.flags & BIT(type)) {
			cpu = sched_numa_find_closest(housekeeping.cpumasks[type], smp_processor_id());
			if (cpu < nr_cpu_ids)
				return cpu;

			cpu = cpumask_any_and_distribute(housekeeping.cpumasks[type], cpu_online_mask);
			if (likely(cpu < nr_cpu_ids))
				return cpu;
			/*
			 * Unless we have another problem this can only happen
			 * at boot time before start_secondary() brings the 1st
			 * housekeeping CPU up.
			 */
			WARN_ON_ONCE(system_state == SYSTEM_RUNNING ||
				     type != HK_TYPE_TIMER);
		}
	}
	return smp_processor_id();
}
EXPORT_SYMBOL_GPL(housekeeping_any_cpu);

const struct cpumask *housekeeping_cpumask(enum hk_type type)
{
	if (static_branch_unlikely(&housekeeping_overridden))
		if (housekeeping.flags & BIT(type))
			return housekeeping.cpumasks[type];
	return cpu_possible_mask;
}
EXPORT_SYMBOL_GPL(housekeeping_cpumask);

void housekeeping_affine(struct task_struct *t, enum hk_type type)
{
	if (static_branch_unlikely(&housekeeping_overridden))
		if (housekeeping.flags & BIT(type))
			set_cpus_allowed_ptr(t, housekeeping.cpumasks[type]);
}
EXPORT_SYMBOL_GPL(housekeeping_affine);

bool housekeeping_test_cpu(int cpu, enum hk_type type)
{
	if (static_branch_unlikely(&housekeeping_overridden))
		if (housekeeping.flags & BIT(type))
			return cpumask_test_cpu(cpu, housekeeping.cpumasks[type]);
	return true;
}
EXPORT_SYMBOL_GPL(housekeeping_test_cpu);

int housekeeping_register_notifier(struct notifier_block *nb)
{
	return blocking_notifier_chain_register(&housekeeping_notifier_list, nb);
}
EXPORT_SYMBOL_GPL(housekeeping_register_notifier);

int housekeeping_unregister_notifier(struct notifier_block *nb)
{
	return blocking_notifier_chain_unregister(&housekeeping_notifier_list, nb);
}
EXPORT_SYMBOL_GPL(housekeeping_unregister_notifier);

static int housekeeping_update_notify(enum hk_type type, const struct cpumask *new_mask)
{
	struct housekeeping_update update = {
		.type = type,
		.new_mask = new_mask,
	};

	return blocking_notifier_call_chain(&housekeeping_notifier_list, HK_UPDATE_MASK, &update);
}

static const char * const hk_type_names[] = {
	[HK_TYPE_TIMER]		= "timer",
	[HK_TYPE_RCU]		= "rcu",
	[HK_TYPE_MISC]		= "misc",
	[HK_TYPE_TICK]		= "tick",
	[HK_TYPE_DOMAIN]	= "domain",
	[HK_TYPE_WQ]		= "workqueue",
	[HK_TYPE_MANAGED_IRQ]	= "managed_irq",
	[HK_TYPE_KTHREAD]	= "kthread",
};

struct hk_attribute {
	struct kobj_attribute kattr;
	enum hk_type type;
};

#define to_hk_attr(_kattr) container_of(_kattr, struct hk_attribute, kattr)

static ssize_t housekeeping_show(struct kobject *kobj,
				struct kobj_attribute *attr, char *buf)
{
	struct hk_attribute *hk_attr = to_hk_attr(attr);
	const struct cpumask *mask = housekeeping_cpumask(hk_attr->type);

	return cpumap_print_to_pagebuf(true, buf, mask);
}

static ssize_t housekeeping_store(struct kobject *kobject,
				 struct kobj_attribute *attr,
				 const char *buf, size_t count)
{
	struct hk_attribute *hk_attr = to_hk_attr(attr);
	enum hk_type type = hk_attr->type;
	cpumask_var_t new_mask;
	int err;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;

	if (!alloc_cpumask_var(&new_mask, GFP_KERNEL))
		return -ENOMEM;

	err = cpulist_parse(buf, new_mask);
	if (err)
		goto out_free;

	if (cpumask_empty(new_mask) ||
	    !cpumask_intersects(new_mask, cpu_online_mask)) {
		err = -EINVAL;
		goto out_free;
	}

	mutex_lock(&housekeeping_mutex);

	if (housekeeping_smt_aware) {
		int cpu;

		for_each_cpu(cpu, new_mask) {
			if (!cpumask_subset(topology_sibling_cpumask(cpu),
					    new_mask)) {
				err = -EINVAL;
				goto out_unlock;
			}
		}
	}

	if (!housekeeping.cpumasks[type]) {
		if (!alloc_cpumask_var(&housekeeping.cpumasks[type], GFP_KERNEL)) {
			err = -ENOMEM;
			goto out_unlock;
		}
	}

	if (cpumask_equal(housekeeping.cpumasks[type], new_mask)) {
		err = 0;
		goto out_unlock;
	}

	cpumask_copy(housekeeping.cpumasks[type], new_mask);
	housekeeping.flags |= BIT(type);
	static_branch_enable(&housekeeping_overridden);

	housekeeping_update_notify(type, new_mask);

	err = count;

out_unlock:
	mutex_unlock(&housekeeping_mutex);
out_free:
	free_cpumask_var(new_mask);
	return err < 0 ? err : count;
}

static struct hk_attribute housekeeping_attrs[HK_TYPE_MAX];
static struct attribute *housekeeping_attr_ptr[HK_TYPE_MAX + 2];

static const struct attribute_group housekeeping_attr_group = {
	.attrs = housekeeping_attr_ptr,
};

static int __init housekeeping_sysfs_init(void)
{
	struct kobject *housekeeping_kobj;
	int i, j = 0;
	int ret;

	housekeeping_kobj = kobject_create_and_add("housekeeping", kernel_kobj);
	if (!housekeeping_kobj)
		return -ENOMEM;

	for (i = 0; i < HK_TYPE_MAX; i++) {
		if (!hk_type_names[i])
			continue;

		housekeeping_attrs[i].type = i;
		sysfs_attr_init(&housekeeping_attrs[i].kattr.attr);
		housekeeping_attrs[i].kattr.attr.name = hk_type_names[i];
		housekeeping_attrs[i].kattr.attr.mode = 0600;
		housekeeping_attrs[i].kattr.show = housekeeping_show;
		housekeeping_attrs[i].kattr.store = housekeeping_store;
		housekeeping_attr_ptr[j++] = &housekeeping_attrs[i].kattr.attr;
	}

	housekeeping_attr_ptr[j++] = &smt_aware_attr.attr;
	housekeeping_attr_ptr[j] = NULL;

	ret = sysfs_create_group(housekeeping_kobj, &housekeeping_attr_group);
	if (ret) {
		kobject_put(housekeeping_kobj);
		return ret;
	}

	return 0;
}
late_initcall(housekeeping_sysfs_init);

void __init housekeeping_init(void)
{
	enum hk_type type;

	if (!housekeeping.flags)
		return;

	static_branch_enable(&housekeeping_overridden);

	for_each_set_bit(type, &housekeeping.flags, HK_TYPE_MAX) {
		/* We need at least one CPU to handle housekeeping work */
		WARN_ON_ONCE(cpumask_empty(housekeeping.cpumasks[type]));
	}
}

static void __init housekeeping_setup_type(enum hk_type type,
					   cpumask_var_t housekeeping_staging)
{
	unsigned int gfp = GFP_KERNEL;

	if (!slab_is_available())
		gfp = GFP_NOWAIT;

	if (!housekeeping.cpumasks[type]) {
		if (!alloc_cpumask_var(&housekeeping.cpumasks[type], gfp)) {
			pr_err("housekeeping: failed to allocate cpumask for type %d\n", type);
			return;
		}
	}

	cpumask_copy(housekeeping.cpumasks[type],
		     housekeeping_staging);
}

static int __init housekeeping_setup(char *str, unsigned long flags)
{
	cpumask_var_t non_housekeeping_mask, housekeeping_staging;
	unsigned int first_cpu;
	int err = 0;
	unsigned int gfp = GFP_KERNEL;

	if (!slab_is_available())
		gfp = GFP_NOWAIT;

	if ((flags & HK_FLAG_KERNEL_NOISE) && !(housekeeping.flags & HK_FLAG_KERNEL_NOISE)) {
		if (!IS_ENABLED(CONFIG_NO_HZ_FULL)) {
			pr_warn("Housekeeping: nohz unsupported."
				" Build with CONFIG_NO_HZ_FULL\n");
			return 0;
		}
	}

	if (!alloc_cpumask_var(&non_housekeeping_mask, gfp))
		return 0;

	if (cpulist_parse(str, non_housekeeping_mask) < 0) {
		pr_warn("Housekeeping: nohz_full= or isolcpus= incorrect CPU range\n");
		goto free_non_housekeeping_mask;
	}

	if (!alloc_cpumask_var(&housekeeping_staging, gfp))
		goto free_non_housekeeping_mask;

	cpumask_andnot(housekeeping_staging,
		       cpu_possible_mask, non_housekeeping_mask);

	first_cpu = cpumask_first_and(cpu_present_mask, housekeeping_staging);
	if (first_cpu >= nr_cpu_ids || first_cpu >= setup_max_cpus) {
		__cpumask_set_cpu(smp_processor_id(), housekeeping_staging);
		__cpumask_clear_cpu(smp_processor_id(), non_housekeeping_mask);
		if (!housekeeping.flags) {
			pr_warn("Housekeeping: must include one present CPU, "
				"using boot CPU:%d\n", smp_processor_id());
		}
	}

	if (cpumask_empty(non_housekeeping_mask))
		goto free_housekeeping_staging;

	if (!housekeeping.flags) {
		/* First setup call ("nohz_full=" or "isolcpus=") */
		enum hk_type type;

		for_each_set_bit(type, &flags, HK_TYPE_MAX)
			housekeeping_setup_type(type, housekeeping_staging);
	} else {
		/* Second setup call ("nohz_full=" after "isolcpus=" or the reverse) */
		enum hk_type type;
		unsigned long iter_flags = flags & housekeeping.flags;

		for_each_set_bit(type, &iter_flags, HK_TYPE_MAX) {
			if (!cpumask_equal(housekeeping_staging,
					   housekeeping.cpumasks[type])) {
				pr_warn("Housekeeping: nohz_full= must match isolcpus=\n");
				goto free_housekeeping_staging;
			}
		}

		/*
		 * Check the combination of nohz_full and isolcpus=domain,
		 * necessary to avoid problems with the timer migration
		 * hierarchy. managed_irq is ignored by this check since it
		 * isn't considered in the timer migration logic.
		 */
		iter_flags = housekeeping.flags & (HK_FLAG_KERNEL_NOISE | HK_FLAG_DOMAIN);
		type = find_first_bit(&iter_flags, HK_TYPE_MAX);
		/*
		 * Pass the check if none of these flags were previously set or
		 * are not in the current selection.
		 */
		iter_flags = flags & (HK_FLAG_KERNEL_NOISE | HK_FLAG_DOMAIN);
		first_cpu = (type == HK_TYPE_MAX || !iter_flags) ? 0 :
			    cpumask_first_and_and(cpu_present_mask,
				    housekeeping_staging, housekeeping.cpumasks[type]);
		if (first_cpu >= min(nr_cpu_ids, setup_max_cpus)) {
			pr_warn("Housekeeping: must include one present CPU "
				"neither in nohz_full= nor in isolcpus=domain, "
				"ignoring setting %s\n", str);
			goto free_housekeeping_staging;
		}

		iter_flags = flags & ~housekeeping.flags;

		for_each_set_bit(type, &iter_flags, HK_TYPE_MAX)
			housekeeping_setup_type(type, housekeeping_staging);
	}

	if ((flags & HK_FLAG_KERNEL_NOISE) && !(housekeeping.flags & HK_FLAG_KERNEL_NOISE))
		tick_nohz_full_setup(non_housekeeping_mask);

	housekeeping.flags |= flags;
	err = 1;

free_housekeeping_staging:
	free_cpumask_var(housekeeping_staging);
free_non_housekeeping_mask:
	free_cpumask_var(non_housekeeping_mask);

	return err;
}

static int __init housekeeping_nohz_full_setup(char *str)
{
	unsigned long flags;

	flags = HK_FLAG_KERNEL_NOISE;

	return housekeeping_setup(str, flags);
}
__setup("nohz_full=", housekeeping_nohz_full_setup);

static int __init housekeeping_isolcpus_setup(char *str)
{
	unsigned long flags = 0;
	bool illegal = false;
	char *par;
	int len;

	while (isalpha(*str)) {
		/*
		 * isolcpus=nohz is equivalent to nohz_full.
		 */
		if (!strncmp(str, "nohz,", 5)) {
			str += 5;
			flags |= HK_FLAG_KERNEL_NOISE;
			continue;
		}

		if (!strncmp(str, "domain,", 7)) {
			str += 7;
			flags |= HK_FLAG_DOMAIN;
			continue;
		}

		if (!strncmp(str, "managed_irq,", 12)) {
			str += 12;
			flags |= HK_FLAG_MANAGED_IRQ;
			continue;
		}

		/*
		 * Skip unknown sub-parameter and validate that it is not
		 * containing an invalid character.
		 */
		for (par = str, len = 0; *str && *str != ','; str++, len++) {
			if (!isalpha(*str) && *str != '_')
				illegal = true;
		}

		if (illegal) {
			pr_warn("isolcpus: Invalid flag %.*s\n", len, par);
			return 0;
		}

		pr_info("isolcpus: Skipped unknown flag %.*s\n", len, par);
		str++;
	}

	/* Default behaviour for isolcpus without flags */
	if (!flags)
		flags |= HK_FLAG_DOMAIN;

	return housekeeping_setup(str, flags);
}
__setup("isolcpus=", housekeeping_isolcpus_setup);
