// SPDX-License-Identifier: GPL-2.0-only

#include <linux/kobject.h>
#include <linux/slab.h>
#include <linux/sysfs.h>
#include "cleancache_sysfs.h"

static atomic64_t stats[CLEANCACHE_STAT_NR];

void cleancache_stat_inc(enum cleancache_stat type)
{
	atomic64_inc(&stats[type]);
}

void cleancache_stat_add(enum cleancache_stat type, unsigned long delta)
{
	atomic64_add(delta, &stats[type]);
}

void cleancache_pool_stat_inc(struct cleancache_pool_stats *pool_stats,
			      enum cleancache_pool_stat type)
{
	atomic64_inc(&pool_stats->stats[type]);
}

void cleancache_pool_stat_dec(struct cleancache_pool_stats *pool_stats,
			      enum cleancache_pool_stat type)
{
	atomic64_dec(&pool_stats->stats[type]);
}

void cleancache_pool_stat_add(struct cleancache_pool_stats *pool_stats,
			      enum cleancache_pool_stat type, long delta)
{
	atomic64_add(delta, &pool_stats->stats[type]);
}

#define CLEANCACHE_ATTR_RO(_name) \
	static struct kobj_attribute _name##_attr = __ATTR_RO(_name)

static inline struct cleancache_pool_stats *kobj_to_stats(struct kobject *kobj)
{
	return container_of(kobj, struct cleancache_pool_stats, kobj);
}

static ssize_t stored_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%llu\n", (u64)atomic64_read(&stats[STORED]));
}
CLEANCACHE_ATTR_RO(stored);

static ssize_t skipped_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%llu\n", (u64)atomic64_read(&stats[SKIPPED]));
}
CLEANCACHE_ATTR_RO(skipped);

static ssize_t restored_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%llu\n", (u64)atomic64_read(&stats[RESTORED]));
}
CLEANCACHE_ATTR_RO(restored);

static ssize_t missed_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%llu\n", (u64)atomic64_read(&stats[MISSED]));
}
CLEANCACHE_ATTR_RO(missed);

static ssize_t reclaimed_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%llu\n", (u64)atomic64_read(&stats[RECLAIMED]));
}
CLEANCACHE_ATTR_RO(reclaimed);

static ssize_t recalled_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%llu\n", (u64)atomic64_read(&stats[RECALLED]));
}
CLEANCACHE_ATTR_RO(recalled);

static ssize_t invalidated_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%llu\n", (u64)atomic64_read(&stats[INVALIDATED]));
}
CLEANCACHE_ATTR_RO(invalidated);

static ssize_t cached_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	s64 dropped = atomic64_read(&stats[INVALIDATED]) +
			atomic64_read(&stats[RECLAIMED]) +
			atomic64_read(&stats[RECALLED]);

	return sysfs_emit(buf, "%llu\n", (u64)(atomic64_read(&stats[STORED]) - dropped));
}
CLEANCACHE_ATTR_RO(cached);

static struct attribute *cleancache_attrs[] = {
	&stored_attr.attr,
	&skipped_attr.attr,
	&restored_attr.attr,
	&missed_attr.attr,
	&reclaimed_attr.attr,
	&recalled_attr.attr,
	&invalidated_attr.attr,
	&cached_attr.attr,
	NULL,
};
ATTRIBUTE_GROUPS(cleancache);

#define CLEANCACHE_POOL_ATTR_RO(_name) \
	static struct kobj_attribute _name##_pool_attr = {		\
		.attr	= { .name = __stringify(_name), .mode = 0444 },	\
		.show	= _name##_pool_show,				\
}

static ssize_t size_pool_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%llu\n",
		(u64)atomic64_read(&kobj_to_stats(kobj)->stats[POOL_SIZE]));
}
CLEANCACHE_POOL_ATTR_RO(size);

static ssize_t cached_pool_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%llu\n",
		(u64)atomic64_read(&kobj_to_stats(kobj)->stats[POOL_CACHED]));
}
CLEANCACHE_POOL_ATTR_RO(cached);

static ssize_t recalled_pool_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%llu\n",
		(u64)atomic64_read(&kobj_to_stats(kobj)->stats[POOL_RECALLED]));
}
CLEANCACHE_POOL_ATTR_RO(recalled);


static struct attribute *cleancache_pool_attrs[] = {
	&size_pool_attr.attr,
	&cached_pool_attr.attr,
	&recalled_pool_attr.attr,
	NULL,
};
ATTRIBUTE_GROUPS(cleancache_pool);

static void cleancache_pool_release(struct kobject *kobj)
{
	kfree(kobj_to_stats(kobj));
}

static const struct kobj_type cleancache_pool_ktype = {
	.release = &cleancache_pool_release,
	.sysfs_ops = &kobj_sysfs_ops,
	.default_groups = cleancache_pool_groups,
};

struct cleancache_pool_stats *cleancache_create_pool_stats(int pool_id)
{
	struct cleancache_pool_stats *pool_stats;

	pool_stats = kzalloc(sizeof(*pool_stats), GFP_KERNEL);
	if (!pool_stats)
		return ERR_PTR(-ENOMEM);

	pool_stats->pool_id = pool_id;

	return pool_stats;
}

struct kobject * __init cleancache_sysfs_create_root(void)
{
	struct kobject *kobj;
	int err;

	kobj = kobject_create_and_add("cleancache", mm_kobj);
	if (unlikely(!kobj)) {
		pr_err("Failed to create cleancache kobject\n");
		return ERR_PTR(-ENOMEM);
	}

	err = sysfs_create_group(kobj, cleancache_groups[0]);
	if (err) {
		kobject_put(kobj);
		pr_err("Failed to create cleancache group kobject\n");
		return ERR_PTR(err);
	}

	return kobj;
}

int cleancache_sysfs_create_pool(struct kobject *root_kobj,
				 struct cleancache_pool_stats *pool_stats,
				 const char *name)
{
	return kobject_init_and_add(&pool_stats->kobj, &cleancache_pool_ktype,
				    root_kobj, name);
}
