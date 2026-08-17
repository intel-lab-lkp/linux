/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Internal arm_cpumod data structures and helpers.
 */

#ifndef CPUMOD_INTERNAL_H
#define CPUMOD_INTERNAL_H

#include <linux/bits.h>
#include <linux/kernel.h>
#include <linux/kobject.h>
#include <linux/mutex.h>
#include <linux/sysfs.h>
#include <linux/types.h>

enum cpumod_profile {
	CPUMOD_PROFILE_GRACE,
	CPUMOD_PROFILE_VERA,
	CPUMOD_PROFILE_UNKNOWN,
};

enum cpumod_reg_id {
	CPUMOD_REG_CPUECTLR,
	CPUMOD_REG_CPUECTLR2,
};

#define CPUMOD_PROFILE_VISIBLE_GRACE	BIT(0)
#define CPUMOD_PROFILE_VISIBLE_VERA	BIT(1)
#define CPUMOD_PROFILE_VISIBLE_ALL	(CPUMOD_PROFILE_VISIBLE_GRACE | \
					 CPUMOD_PROFILE_VISIBLE_VERA)

struct cpumod_subsys {
	/* Per-CPU instance lock: serializes sysfs read/write for this CPU. */
	struct mutex lock;
	struct kobject kobj;
	unsigned int cpu;
	enum cpumod_profile profile;
	u64 pf_dis;
	u64 pf_mode;
	u64 cbusy_filter_threshold;
	u64 cbusy_filter_window;
	u64 cmc_min_ways;
	u64 l2spr_cmc_max_ways;
};

struct cpumod_attr {
	struct kobj_attribute kattr;
	size_t value_offset;
	enum cpumod_reg_id reg;
	u8 shift;
	u64 field_mask;
	u64 max_value;
	const char *debug_name;
	u8 visible_profiles;
};

static inline struct cpumod_subsys *cpumod_subsys_from_kobj(struct kobject *kobj)
{
	return container_of(kobj, struct cpumod_subsys, kobj);
}

static inline struct cpumod_attr *cpumod_attr_from_kobj_attr(struct kobj_attribute *kattr)
{
	return container_of(kattr, struct cpumod_attr, kattr);
}

static inline u64 *cpumod_attr_value_ptr(struct cpumod_subsys *subsys,
					 const struct cpumod_attr *attr)
{
	return (u64 *)((char *)subsys + attr->value_offset);
}

static inline u64 cpumod_attr_mask_value(const struct cpumod_attr *attr, u64 value)
{
	return value & (attr->field_mask >> attr->shift);
}

static inline bool cpumod_attr_value_valid(const struct cpumod_attr *attr, u64 value)
{
	return value <= attr->max_value;
}

static inline u64 cpumod_attr_unpack_value(const struct cpumod_attr *attr, u64 reg)
{
	return (reg & attr->field_mask) >> attr->shift;
}

static inline u64 cpumod_attr_pack_value(const struct cpumod_attr *attr, u64 value)
{
	return (value << attr->shift) & attr->field_mask;
}

static inline void cpumod_attr_set_value(struct cpumod_subsys *subsys,
					 const struct cpumod_attr *attr,
					 u64 value)
{
	*cpumod_attr_value_ptr(subsys, attr) = cpumod_attr_mask_value(attr, value);
}

static inline bool cpumod_attr_visible_for_profile(const struct cpumod_attr *attr,
						   enum cpumod_profile profile)
{
	switch (profile) {
	case CPUMOD_PROFILE_GRACE:
		return attr->visible_profiles & CPUMOD_PROFILE_VISIBLE_GRACE;
	case CPUMOD_PROFILE_VERA:
		return attr->visible_profiles & CPUMOD_PROFILE_VISIBLE_VERA;
	default:
		return false;
	}
}

#endif /* CPUMOD_INTERNAL_H */
