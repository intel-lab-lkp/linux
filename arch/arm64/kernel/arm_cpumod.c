// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2026, NVIDIA CORPORATION & AFFILIATES.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 */

#define pr_fmt(fmt) "cpumod: " fmt

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/sysfs.h>
#include <linux/device.h>
#include <linux/cpu.h>
#include <linux/mutex.h>
#include <linux/percpu.h>
#include <linux/slab.h>
#include <asm/cputype.h>
#include <asm/sysreg.h>
#include <linux/bits.h>
#include "arm_cpumod_internal.h"

#define DRIVER_DESC "CPU Modulation Kernel Module"

/* Disables hardware prefetching.
 * 0b0 Enables hardware prefetching. This is the default value.
 * 0b1 Disables hardware prefetching.
 */
#define CPUECTLR_PF_DIS				(15)
#define CPUECTLR_PF_DIS_MSK			BIT_ULL(15)

#define CPUECTLR_CMC_WAYS			(61)
#define CPUECTLR_CMC_WAYS_MSK			GENMASK_ULL(63, 61)

#define CPUECTLR2_PF_MODE			(11)
#define CPUECTLR2_PF_MODE_MSK			GENMASK_ULL(14, 11)

#define CPUECTLR2_CBUSY_FILTER_WINDOW		(9)
#define CPUECTLR2_CBUSY_FILTER_WINDOW_MSK	GENMASK_ULL(10, 9)

#define CPUECTLR2_CBUSY_FILTER_THRESHOLD	(7)
#define CPUECTLR2_CBUSY_FILTER_THRESHOLD_MSK	GENMASK_ULL(8, 7)

static enum cpuhp_state cpumod_hp_state;
static DEFINE_PER_CPU(struct cpumod_subsys *, cpumod_subsys);

struct cpumod_attr_call {
	struct cpumod_subsys *subsys;
	struct cpumod_attr *attr;
	u64 value;
};

struct cpumod_midr_call {
	u32 midr;
};

static const char *cpumod_profile_name(enum cpumod_profile profile)
{
	switch (profile) {
	case CPUMOD_PROFILE_GRACE:
		return "Grace";
	case CPUMOD_PROFILE_VERA:
		return "Vera";
	default:
		return "unknown";
	}
}

/* IMP defined CPU Extended Control Register */
#define IMP_CPUECTLR_EL1	sys_reg(3, 0, 15, 1, 4)
#define IMP_CPUECTLR2_EL1	sys_reg(3, 0, 15, 1, 5)

static enum cpumod_profile cpumod_profile_from_midr(u32 midr)
{
	switch (midr & MIDR_CPU_MODEL_MASK) {
	case MIDR_NEOVERSE_V2:
		return CPUMOD_PROFILE_GRACE;
	case MIDR_NVIDIA_OLYMPUS:
		return CPUMOD_PROFILE_VERA;
	default:
		return CPUMOD_PROFILE_UNKNOWN;
	}
}

static void cpumod_read_midr_remote(void *info)
{
	struct cpumod_midr_call *call = info;

	call->midr = read_cpuid_id();
}

static int cpumod_detect_profile(struct cpumod_subsys *subsys)
{
	struct cpumod_midr_call call;
	int ret;

	ret = smp_call_function_single(subsys->cpu, cpumod_read_midr_remote,
				       &call, 1);
	if (ret) {
		pr_err("Failed to read CPU%u MIDR: %d\n", subsys->cpu, ret);
		return ret;
	}

	subsys->profile = cpumod_profile_from_midr(call.midr);
	if (subsys->profile == CPUMOD_PROFILE_UNKNOWN) {
		pr_debug("Unsupported CPU%u MIDR 0x%08x\n", subsys->cpu,
			 call.midr);
	}

	return 0;
}

static u64 cpumod_read_reg(enum cpumod_reg_id reg)
{
	switch (reg) {
	case CPUMOD_REG_CPUECTLR:
		return read_sysreg_s(IMP_CPUECTLR_EL1);
	case CPUMOD_REG_CPUECTLR2:
		return read_sysreg_s(IMP_CPUECTLR2_EL1);
	default:
		return 0;
	}
}

static void cpumod_write_reg(enum cpumod_reg_id reg, u64 value)
{
	switch (reg) {
	case CPUMOD_REG_CPUECTLR:
		write_sysreg_s(value, IMP_CPUECTLR_EL1);
		/* The remote callback executes this barrier on the target CPU. */
		isb();
		break;
	case CPUMOD_REG_CPUECTLR2:
		write_sysreg_s(value, IMP_CPUECTLR2_EL1);
		/* The remote callback executes this barrier on the target CPU. */
		isb();
		break;
	default:
		return;
	}
}

static u64 cpumod_attr_read_reg(const struct cpumod_attr *attr)
{
	return cpumod_attr_unpack_value(attr, cpumod_read_reg(attr->reg));
}

static void cpumod_attr_write_reg(const struct cpumod_attr *attr, u64 value)
{
	u64 reg = cpumod_read_reg(attr->reg);
	u64 field_value = cpumod_attr_pack_value(attr, value);

	cpumod_write_reg(attr->reg, (reg & ~attr->field_mask) | field_value);
}

static void cpumod_attr_read_remote(void *info)
{
	struct cpumod_attr_call *call = info;
	u64 *value = cpumod_attr_value_ptr(call->subsys, call->attr);

	*value = cpumod_attr_read_reg(call->attr);
}

static void cpumod_attr_write_remote(void *info)
{
	struct cpumod_attr_call *call = info;
	struct cpumod_subsys *subsys = call->subsys;
	struct cpumod_attr *attr = call->attr;
	u64 readback;

	cpumod_attr_write_reg(attr, call->value);
	readback = cpumod_attr_read_reg(attr);
	pr_debug("cpu%u %s: %llx\n",
		 subsys->cpu,
		 attr->debug_name,
		 (unsigned long long)readback);
}

static ssize_t cpumod_attr_show(struct kobject *kobj,
				struct kobj_attribute *kattr, char *buf)
{
	struct cpumod_subsys *subsys = cpumod_subsys_from_kobj(kobj);
	struct cpumod_attr *attr = cpumod_attr_from_kobj_attr(kattr);
	struct cpumod_attr_call call = {
		.subsys = subsys,
		.attr = attr,
	};
	int ret;

	guard(mutex)(&subsys->lock);
	ret = smp_call_function_single(subsys->cpu,
				       cpumod_attr_read_remote, &call, 1);
	if (ret)
		return ret;

	ret = sysfs_emit(buf, "%llu\n",
			 (unsigned long long)*cpumod_attr_value_ptr(subsys, attr));

	return ret;
}

static ssize_t cpumod_attr_store(struct kobject *kobj,
				 struct kobj_attribute *kattr,
				 const char *buf, size_t count)
{
	struct cpumod_subsys *subsys = cpumod_subsys_from_kobj(kobj);
	struct cpumod_attr *attr = cpumod_attr_from_kobj_attr(kattr);
	struct cpumod_attr_call call = {
		.subsys = subsys,
		.attr = attr,
	};
	unsigned int val;
	int ret;

	ret = kstrtouint(buf, 10, &val);
	if (ret)
		return -EINVAL;
	if (!cpumod_attr_value_valid(attr, val))
		return -EINVAL;

	call.value = cpumod_attr_mask_value(attr, val);

	guard(mutex)(&subsys->lock);
	ret = smp_call_function_single(subsys->cpu,
				       cpumod_attr_write_remote, &call, 1);
	if (ret)
		return ret;
	cpumod_attr_set_value(subsys, attr, call.value);

	return count;
}

static ssize_t affected_cpus_show(struct kobject *kobj,
				  struct kobj_attribute *attr, char *buf)
{
	struct cpumod_subsys *subsys = cpumod_subsys_from_kobj(kobj);

	return sysfs_emit(buf, "%u\n", subsys->cpu);
}

static struct kobj_attribute affected_cpus_attr = __ATTR_RO(affected_cpus);

static umode_t cpumod_attr_is_visible(struct kobject *kobj,
				      struct attribute *attr, int unused)
{
	struct cpumod_subsys *subsys = cpumod_subsys_from_kobj(kobj);
	struct cpumod_attr *cpumod_attr;

	if (attr == &affected_cpus_attr.attr)
		return attr->mode;

	cpumod_attr = container_of(attr, struct cpumod_attr, kattr.attr);
	if (!cpumod_attr_visible_for_profile(cpumod_attr, subsys->profile))
		return 0;

	return attr->mode;
}

#define CPUMOD_ATTR_RW(_name, _field, _reg, _shift, _field_mask,	\
		       _max_value, _debug_name,			\
		       _visible_profiles)				\
static struct cpumod_attr _name##_attr = {				\
	.kattr = __ATTR(_name, 0644, cpumod_attr_show, cpumod_attr_store), \
	.value_offset = offsetof(struct cpumod_subsys, _field),	\
	.reg = _reg,						\
	.shift = _shift,					\
	.field_mask = _field_mask,				\
	.max_value = _max_value,				\
	.debug_name = _debug_name,				\
	.visible_profiles = _visible_profiles,			\
}

CPUMOD_ATTR_RW(pf_dis, pf_dis, CPUMOD_REG_CPUECTLR, CPUECTLR_PF_DIS,
	       CPUECTLR_PF_DIS_MSK, 1, "PF_DIS",
	       CPUMOD_PROFILE_VISIBLE_ALL);
CPUMOD_ATTR_RW(pf_mode, pf_mode, CPUMOD_REG_CPUECTLR2, CPUECTLR2_PF_MODE,
	       CPUECTLR2_PF_MODE_MSK, 9, "PF_MODE",
	       CPUMOD_PROFILE_VISIBLE_ALL);
CPUMOD_ATTR_RW(cbusy_filter_threshold, cbusy_filter_threshold,
	       CPUMOD_REG_CPUECTLR2, CPUECTLR2_CBUSY_FILTER_THRESHOLD,
	       CPUECTLR2_CBUSY_FILTER_THRESHOLD_MSK, 3,
	       "CBUSY_FILTER_THRESHOLD",
	       CPUMOD_PROFILE_VISIBLE_GRACE);
CPUMOD_ATTR_RW(cbusy_filter_window, cbusy_filter_window,
	       CPUMOD_REG_CPUECTLR2, CPUECTLR2_CBUSY_FILTER_WINDOW,
	       CPUECTLR2_CBUSY_FILTER_WINDOW_MSK, 3,
	       "CBUSY_FILTER_WINDOW",
	       CPUMOD_PROFILE_VISIBLE_GRACE);
CPUMOD_ATTR_RW(cmc_min_ways, cmc_min_ways, CPUMOD_REG_CPUECTLR,
	       CPUECTLR_CMC_WAYS, CPUECTLR_CMC_WAYS_MSK, 7,
	       "CMC_MIN_WAYS", CPUMOD_PROFILE_VISIBLE_GRACE);
CPUMOD_ATTR_RW(l2spr_cmc_max_ways, l2spr_cmc_max_ways, CPUMOD_REG_CPUECTLR,
	       CPUECTLR_CMC_WAYS, CPUECTLR_CMC_WAYS_MSK, 7,
	       "L2SPR_CMC_MAX_WAYS",
	       CPUMOD_PROFILE_VISIBLE_VERA);

#undef CPUMOD_ATTR_RW

static struct attribute *cpumod_attrs[] = {
	&affected_cpus_attr.attr,
	&pf_dis_attr.kattr.attr,
	&pf_mode_attr.kattr.attr,
	&cbusy_filter_threshold_attr.kattr.attr,
	&cbusy_filter_window_attr.kattr.attr,
	&cmc_min_ways_attr.kattr.attr,
	&l2spr_cmc_max_ways_attr.kattr.attr,
	NULL,
};

static const struct attribute_group cpumod_group = {
	.attrs = cpumod_attrs,
	.is_visible = cpumod_attr_is_visible,
};

static const struct attribute_group *cpumod_groups[] = {
	&cpumod_group,
	NULL,
};

static void cpumod_release(struct kobject *kobj)
{
	kfree(cpumod_subsys_from_kobj(kobj));
}

static const struct kobj_type cpumod_ktype = {
	.release = cpumod_release,
	.sysfs_ops = &kobj_sysfs_ops,
	.default_groups = cpumod_groups,
};

static int cpumod_create_subsys(struct device *cpu_dev, unsigned int cpu)
{
	struct cpumod_subsys *subsys;
	int ret;

	if (per_cpu(cpumod_subsys, cpu))
		return 0;

	subsys = kzalloc_obj(*subsys, GFP_KERNEL);
	if (!subsys)
		return -ENOMEM;

	subsys->cpu = cpu;
	mutex_init(&subsys->lock);
	ret = cpumod_detect_profile(subsys);
	if (ret) {
		kfree(subsys);
		return ret;
	}
	if (subsys->profile == CPUMOD_PROFILE_UNKNOWN) {
		kfree(subsys);
		return 0;
	}

	pr_debug("CPU%u detected %s profile\n", cpu,
		 cpumod_profile_name(subsys->profile));

	ret = kobject_init_and_add(&subsys->kobj, &cpumod_ktype,
				   &cpu_dev->kobj, "cpumod");
	if (ret) {
		pr_err("Failed to create cpumod sysfs subtree for CPU%u: %d\n",
		       cpu, ret);
		kobject_put(&subsys->kobj);
		return ret;
	}

	per_cpu(cpumod_subsys, cpu) = subsys;

	return 0;
}

static void cpumod_destroy_subsys(unsigned int cpu)
{
	struct cpumod_subsys *subsys = per_cpu(cpumod_subsys, cpu);

	if (!subsys)
		return;

	per_cpu(cpumod_subsys, cpu) = NULL;
	kobject_put(&subsys->kobj);
}

static void cpumod_destroy_all_subsys(void)
{
	unsigned int cpu;

	for_each_possible_cpu(cpu)
		cpumod_destroy_subsys(cpu);
}

static int cpumod_online_cpu(unsigned int cpu)
{
	struct device *cpu_dev = get_cpu_device(cpu);

	if (!cpu_dev) {
		pr_err("Failed to get CPU%u device\n", cpu);
		return -ENODEV;
	}

	return cpumod_create_subsys(cpu_dev, cpu);
}

static int cpumod_offline_cpu(unsigned int cpu)
{
	cpumod_destroy_subsys(cpu);

	return 0;
}

static int __init cpumod_init(void)
{
	unsigned int cpu;
	int ret;

	cpus_read_lock();
	for_each_online_cpu(cpu) {
		struct device *cpu_dev = get_cpu_device(cpu);

		if (!cpu_dev) {
			pr_err("Failed to get CPU%u device\n", cpu);
			ret = -ENODEV;
			goto err_unlock;
		}

		ret = cpumod_create_subsys(cpu_dev, cpu);
		if (ret)
			goto err_unlock;
	}

	ret = cpuhp_setup_state_nocalls_cpuslocked(CPUHP_AP_ONLINE_DYN,
						   "arm64/cpumod:online",
						   cpumod_online_cpu,
						   cpumod_offline_cpu);
	cpus_read_unlock();
	if (ret < 0)
		goto err_destroy;

	cpumod_hp_state = ret;
	pr_info("module loaded\n");

	return 0;

err_unlock:
	cpus_read_unlock();
err_destroy:
	cpumod_destroy_all_subsys();
	return ret;
}

static void __exit cpumod_exit(void)
{
	cpuhp_remove_state_nocalls(cpumod_hp_state);
	cpumod_destroy_all_subsys();
	pr_info("module unloaded\n");
}

module_init(cpumod_init);
module_exit(cpumod_exit);

MODULE_LICENSE("GPL");
MODULE_VERSION("1.0");
MODULE_AUTHOR("kobak@nvidia.com");
MODULE_DESCRIPTION(DRIVER_DESC);
