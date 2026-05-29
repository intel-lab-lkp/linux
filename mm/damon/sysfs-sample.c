// SPDX-License-Identifier: FPL-2.0
/*
 * DAMON sample sysfs directory implementation
 */

#include <linux/slab.h>

#include "sysfs-common.h"

/*
 * access check report filter directory
 */

struct damon_sysfs_sample_filter {
	struct kobject kobj;
	enum damon_sample_filter_type type;
	bool matching;
	bool allow;
	cpumask_t cpumask;
	int *tid_arr;	/* first entry is the length of the array */
};

static struct damon_sysfs_sample_filter *damon_sysfs_sample_filter_alloc(void)
{
	return kzalloc(sizeof(struct damon_sysfs_sample_filter), GFP_KERNEL);
}

struct damon_sysfs_sample_filter_type_name {
	enum damon_sample_filter_type type;
	char *name;
};

static const struct damon_sysfs_sample_filter_type_name
damon_sysfs_sample_filter_type_names[] = {
	{
		.type = DAMON_FILTER_TYPE_CPUMASK,
		.name = "cpumask",
	},
	{
		.type = DAMON_FILTER_TYPE_THREADS,
		.name = "threads",
	},
	{
		.type = DAMON_FILTER_TYPE_WRITE,
		.name = "write",
	},
};

static ssize_t type_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	struct damon_sysfs_sample_filter *filter = container_of(kobj,
			struct damon_sysfs_sample_filter, kobj);
	int i = 0;

	for (; i < ARRAY_SIZE(damon_sysfs_sample_filter_type_names); i++) {
		const struct damon_sysfs_sample_filter_type_name *type_name;

		type_name = &damon_sysfs_sample_filter_type_names[i];
		if (type_name->type == filter->type)
			return sysfs_emit(buf, "%s\n", type_name->name);
	}
	return -EINVAL;
}

static ssize_t type_store(struct kobject *kobj,
		struct kobj_attribute *attr, const char *buf, size_t count)
{
	struct damon_sysfs_sample_filter *filter = container_of(kobj,
			struct damon_sysfs_sample_filter, kobj);
	ssize_t ret = -EINVAL;
	int i = 0;

	for (; i < ARRAY_SIZE(damon_sysfs_sample_filter_type_names); i++) {
		const struct damon_sysfs_sample_filter_type_name *type_name;

		type_name = &damon_sysfs_sample_filter_type_names[i];
		if (sysfs_streq(buf, type_name->name)) {
			filter->type = type_name->type;
			ret = count;
			break;
		}
	}
	return ret;
}

static ssize_t matching_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	struct damon_sysfs_sample_filter *filter = container_of(kobj,
			struct damon_sysfs_sample_filter, kobj);

	return sysfs_emit(buf, "%c\n", filter->matching ? 'Y' : 'N');
}

static ssize_t matching_store(struct kobject *kobj,
		struct kobj_attribute *attr, const char *buf, size_t count)
{
	struct damon_sysfs_sample_filter *filter = container_of(kobj,
			struct damon_sysfs_sample_filter, kobj);
	bool matching;
	int err = kstrtobool(buf, &matching);

	if (err)
		return err;

	filter->matching = matching;
	return count;
}

static ssize_t allow_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	struct damon_sysfs_sample_filter *filter = container_of(kobj,
			struct damon_sysfs_sample_filter, kobj);

	return sysfs_emit(buf, "%c\n", filter->allow ? 'Y' : 'N');
}

static ssize_t allow_store(struct kobject *kobj,
		struct kobj_attribute *attr, const char *buf, size_t count)
{
	struct damon_sysfs_sample_filter *filter = container_of(kobj,
			struct damon_sysfs_sample_filter, kobj);
	bool allow;
	int err = kstrtobool(buf, &allow);

	if (err)
		return err;

	filter->allow = allow;
	return count;
}

static ssize_t cpumask_show(struct kobject *kobj, struct kobj_attribute *attr,
		char *buf)
{
	struct damon_sysfs_sample_filter *filter = container_of(kobj,
			struct damon_sysfs_sample_filter, kobj);

	return sysfs_emit(buf, "%*pbl\n", cpumask_pr_args(&filter->cpumask));
}

static ssize_t cpumask_store(struct kobject *kobj, struct kobj_attribute *attr,
		const char *buf, size_t count)
{
	struct damon_sysfs_sample_filter *filter = container_of(kobj,
			struct damon_sysfs_sample_filter, kobj);
	cpumask_t cpumask;
	int err = cpulist_parse(buf, &cpumask);

	if (err)
		return err;
	filter->cpumask = cpumask;
	return count;
}

static ssize_t tid_arr_show(struct kobject *kobj, struct kobj_attribute *attr,
		char *buf)
{
	struct damon_sysfs_sample_filter *sample_filter = container_of(kobj,
			struct damon_sysfs_sample_filter, kobj);
	char *str;
	int nr_tids, *tid_arr;
	int i, ret;

	if (!sample_filter->tid_arr)
		return sysfs_emit(buf, "\n");

	str = kcalloc(2048, sizeof(*str), GFP_KERNEL);
	if (!str)
		return -ENOMEM;
	nr_tids = sample_filter->tid_arr[0];
	tid_arr = &sample_filter->tid_arr[1];
	for (i = 0; i < nr_tids; i++) {
		snprintf(&str[strlen(str)], 2048 - strlen(str), "%d",
				tid_arr[i]);
		if (i < nr_tids - 1)
			snprintf(&str[strlen(str)], 2048 - strlen(str), ",");
	}
	ret = sysfs_emit(buf, "%s\n", str);
	kfree(str);
	return ret;
}

static ssize_t tid_arr_store(struct kobject *kobj, struct kobj_attribute *attr,
		const char *buf, size_t count)
{
	struct damon_sysfs_sample_filter *sample_filter = container_of(kobj,
			struct damon_sysfs_sample_filter, kobj);
	int err;

	err = parse_int_array(buf, count, &sample_filter->tid_arr);
	if (err)
		return err;
	return count;
}

static void damon_sysfs_sample_filter_release(struct kobject *kobj)
{
	struct damon_sysfs_sample_filter *filter = container_of(kobj,
			struct damon_sysfs_sample_filter, kobj);

	kfree(filter);
}

static struct kobj_attribute damon_sysfs_sample_filter_type_attr =
		__ATTR_RW_MODE(type, 0600);

static struct kobj_attribute damon_sysfs_sample_filter_matching_attr =
		__ATTR_RW_MODE(matching, 0600);

static struct kobj_attribute damon_sysfs_sample_filter_allow_attr =
		__ATTR_RW_MODE(allow, 0600);

static struct kobj_attribute damon_sysfs_sample_filter_cpumask_attr =
		__ATTR_RW_MODE(cpumask, 0600);

static struct kobj_attribute damon_sysfs_sample_filter_tid_arr_attr =
		__ATTR_RW_MODE(tid_arr, 0600);

static struct attribute *damon_sysfs_sample_filter_attrs[] = {
	&damon_sysfs_sample_filter_type_attr.attr,
	&damon_sysfs_sample_filter_matching_attr.attr,
	&damon_sysfs_sample_filter_allow_attr.attr,
	&damon_sysfs_sample_filter_cpumask_attr.attr,
	&damon_sysfs_sample_filter_tid_arr_attr.attr,
	NULL,
};
ATTRIBUTE_GROUPS(damon_sysfs_sample_filter);

static const struct kobj_type damon_sysfs_sample_filter_ktype = {
	.release = damon_sysfs_sample_filter_release,
	.sysfs_ops = &kobj_sysfs_ops,
	.default_groups = damon_sysfs_sample_filter_groups,
};

/*
 * access check report filters directory
 */

struct damon_sysfs_sample_filters {
	struct kobject kobj;
	struct damon_sysfs_sample_filter **filters_arr;
	int nr;
};

static struct damon_sysfs_sample_filters *
damon_sysfs_sample_filters_alloc(void)
{
	return kzalloc(sizeof(struct damon_sysfs_sample_filters), GFP_KERNEL);
}

static void damon_sysfs_sample_filters_rm_dirs(
		struct damon_sysfs_sample_filters *filters)
{
	struct damon_sysfs_sample_filter **filters_arr = filters->filters_arr;
	int i;

	for (i = 0; i < filters->nr; i++)
		kobject_put(&filters_arr[i]->kobj);
	filters->nr = 0;
	kfree(filters_arr);
	filters->filters_arr = NULL;
}

static int damon_sysfs_sample_filters_add_dirs(
		struct damon_sysfs_sample_filters *filters, int nr_filters)
{
	struct damon_sysfs_sample_filter **filters_arr, *filter;
	int err, i;

	damon_sysfs_sample_filters_rm_dirs(filters);
	if (!nr_filters)
		return 0;

	filters_arr = kmalloc_array(nr_filters, sizeof(*filters_arr),
			GFP_KERNEL | __GFP_NOWARN);
	if (!filters_arr)
		return -ENOMEM;
	filters->filters_arr = filters_arr;

	for (i = 0; i < nr_filters; i++) {
		filter = damon_sysfs_sample_filter_alloc();
		if (!filter) {
			damon_sysfs_sample_filters_rm_dirs(filters);
			return -ENOMEM;
		}

		err = kobject_init_and_add(&filter->kobj,
				&damon_sysfs_sample_filter_ktype,
				&filters->kobj, "%d", i);
		if (err) {
			kobject_put(&filter->kobj);
			damon_sysfs_sample_filters_rm_dirs(filters);
			return err;
		}

		filters_arr[i] = filter;
		filters->nr++;
	}
	return 0;
}

static ssize_t nr_filters_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	struct damon_sysfs_sample_filters *filters = container_of(kobj,
			struct damon_sysfs_sample_filters, kobj);

	return sysfs_emit(buf, "%d\n", filters->nr);
}

static ssize_t nr_filters_store(struct kobject *kobj,
		struct kobj_attribute *attr, const char *buf, size_t count)
{
	struct damon_sysfs_sample_filters *filters;
	int nr, err = kstrtoint(buf, 0, &nr);

	if (err)
		return err;
	if (nr < 0)
		return -EINVAL;

	filters = container_of(kobj, struct damon_sysfs_sample_filters, kobj);

	if (!mutex_trylock(&damon_sysfs_lock))
		return -EBUSY;
	err = damon_sysfs_sample_filters_add_dirs(filters, nr);
	mutex_unlock(&damon_sysfs_lock);
	if (err)
		return err;

	return count;
}

static void damon_sysfs_sample_filters_release(struct kobject *kobj)
{
	kfree(container_of(kobj, struct damon_sysfs_sample_filters, kobj));
}

static struct kobj_attribute damon_sysfs_sample_filters_nr_attr =
		__ATTR_RW_MODE(nr_filters, 0600);

static struct attribute *damon_sysfs_sample_filters_attrs[] = {
	&damon_sysfs_sample_filters_nr_attr.attr,
	NULL,
};
ATTRIBUTE_GROUPS(damon_sysfs_sample_filters);

static const struct kobj_type damon_sysfs_sample_filters_ktype = {
	.release = damon_sysfs_sample_filters_release,
	.sysfs_ops = &kobj_sysfs_ops,
	.default_groups = damon_sysfs_sample_filters_groups,
};

/*
 * access check primitives directory
 */

struct damon_sysfs_primitives {
	struct kobject kobj;
	bool page_table;
	bool page_fault;
};

static struct damon_sysfs_primitives *damon_sysfs_primitives_alloc(
		bool page_table, bool page_fault)
{
	struct damon_sysfs_primitives *primitives = kmalloc(
			sizeof(*primitives), GFP_KERNEL);

	if (!primitives)
		return NULL;

	primitives->kobj = (struct kobject){};
	primitives->page_table = page_table;
	primitives->page_fault = page_fault;
	return primitives;
}

static ssize_t page_table_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	struct damon_sysfs_primitives *primitives = container_of(kobj,
			struct damon_sysfs_primitives, kobj);

	return sysfs_emit(buf, "%c\n", primitives->page_table ? 'Y' : 'N');
}

static ssize_t page_table_store(struct kobject *kobj,
		struct kobj_attribute *attr, const char *buf, size_t count)
{
	struct damon_sysfs_primitives *primitives = container_of(kobj,
			struct damon_sysfs_primitives, kobj);
	bool enable;
	int err = kstrtobool(buf, &enable);

	if (err)
		return err;
	primitives->page_table = enable;
	return count;
}

static ssize_t page_fault_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	struct damon_sysfs_primitives *primitives = container_of(kobj,
			struct damon_sysfs_primitives, kobj);

	return sysfs_emit(buf, "%c\n", primitives->page_fault ? 'Y' : 'N');
}

static ssize_t page_fault_store(struct kobject *kobj,
		struct kobj_attribute *attr, const char *buf, size_t count)
{
	struct damon_sysfs_primitives *primitives = container_of(kobj,
			struct damon_sysfs_primitives, kobj);
	bool enable;
	int err = kstrtobool(buf, &enable);

	if (err)
		return err;
	primitives->page_fault = enable;
	return count;
}

static void damon_sysfs_primitives_release(struct kobject *kobj)
{
	struct damon_sysfs_primitives *primitives = container_of(kobj,
			struct damon_sysfs_primitives, kobj);

	kfree(primitives);
}

static struct kobj_attribute damon_sysfs_primitives_page_table_attr =
		__ATTR_RW_MODE(page_table, 0600);

static struct kobj_attribute damon_sysfs_primitives_page_fault_attr =
		__ATTR_RW_MODE(page_fault, 0600);

static struct attribute *damon_sysfs_primitives_attrs[] = {
	&damon_sysfs_primitives_page_table_attr.attr,
	&damon_sysfs_primitives_page_fault_attr.attr,
	NULL,
};
ATTRIBUTE_GROUPS(damon_sysfs_primitives);

static const struct kobj_type damon_sysfs_primitives_ktype = {
	.release = damon_sysfs_primitives_release,
	.sysfs_ops = &kobj_sysfs_ops,
	.default_groups = damon_sysfs_primitives_groups,
};

/*
 * perf_event_attr directory
 */

struct damon_sysfs_perf_event_attr {
	struct kobject kobj;
	u32 type;
	u64 config;
	u64 config1;
	u64 config2;
	bool sample_phys_addr;
	bool sample_weight_struct;
	bool exclude_kernel;
	bool exclude_hv;
	bool freq;
	u64 sample_freq;
	u64 sample_period;
	u32 wakeup_events;
	u32 precise_ip;
};

static struct damon_sysfs_perf_event_attr *
damon_sysfs_perf_event_attr_alloc(void)
{
	struct damon_sysfs_perf_event_attr *attr =
		kzalloc(sizeof(*attr), GFP_KERNEL);

	if (!attr)
		return NULL;
	attr->wakeup_events = 1;
	attr->precise_ip = 2;
	attr->freq = true;
	attr->exclude_kernel = true;
	attr->exclude_hv = true;
	return attr;
}

static ssize_t attr_type_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	struct damon_sysfs_perf_event_attr *perf_event_attr = container_of(kobj,
			struct damon_sysfs_perf_event_attr, kobj);

	return sysfs_emit(buf, "0x%x\n", perf_event_attr->type);
}

static ssize_t attr_type_store(struct kobject *kobj,
		struct kobj_attribute *attr, const char *buf, size_t count)
{
	struct damon_sysfs_perf_event_attr *perf_event_attr = container_of(kobj,
			struct damon_sysfs_perf_event_attr, kobj);
	int err = kstrtou32(buf, 0, &perf_event_attr->type);

	if (err)
		return -EINVAL;
	return count;
}

static ssize_t config_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	struct damon_sysfs_perf_event_attr *perf_event_attr = container_of(kobj,
			struct damon_sysfs_perf_event_attr, kobj);

	return sysfs_emit(buf, "0x%llx\n", perf_event_attr->config);
}

static ssize_t config_store(struct kobject *kobj,
		struct kobj_attribute *attr, const char *buf, size_t count)
{
	struct damon_sysfs_perf_event_attr *perf_event_attr = container_of(kobj,
			struct damon_sysfs_perf_event_attr, kobj);
	int err = kstrtou64(buf, 0, &perf_event_attr->config);

	if (err)
		return -EINVAL;
	return count;
}

static ssize_t config1_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	struct damon_sysfs_perf_event_attr *perf_event_attr = container_of(kobj,
			struct damon_sysfs_perf_event_attr, kobj);

	return sysfs_emit(buf, "0x%llx\n", perf_event_attr->config1);
}

static ssize_t config1_store(struct kobject *kobj,
		struct kobj_attribute *attr, const char *buf, size_t count)
{
	struct damon_sysfs_perf_event_attr *perf_event_attr = container_of(kobj,
			struct damon_sysfs_perf_event_attr, kobj);
	int err = kstrtou64(buf, 0, &perf_event_attr->config1);

	if (err)
		return -EINVAL;
	return count;
}

static ssize_t config2_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	struct damon_sysfs_perf_event_attr *perf_event_attr = container_of(kobj,
			struct damon_sysfs_perf_event_attr, kobj);

	return sysfs_emit(buf, "0x%llx\n", perf_event_attr->config2);
}

static ssize_t config2_store(struct kobject *kobj,
		struct kobj_attribute *attr, const char *buf, size_t count)
{
	struct damon_sysfs_perf_event_attr *perf_event_attr = container_of(kobj,
			struct damon_sysfs_perf_event_attr, kobj);
	int err = kstrtou64(buf, 0, &perf_event_attr->config2);

	if (err)
		return -EINVAL;
	return count;
}

static ssize_t sample_phys_addr_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	struct damon_sysfs_perf_event_attr *perf_event_attr = container_of(kobj,
			struct damon_sysfs_perf_event_attr, kobj);

	return sysfs_emit(buf, "%d\n", perf_event_attr->sample_phys_addr);
}

static ssize_t sample_phys_addr_store(struct kobject *kobj,
		struct kobj_attribute *attr, const char *buf, size_t count)
{
	struct damon_sysfs_perf_event_attr *perf_event_attr = container_of(kobj,
			struct damon_sysfs_perf_event_attr, kobj);
	bool sample_phys_addr;
	int err = kstrtobool(buf, &sample_phys_addr);

	if (err)
		return -EINVAL;

	perf_event_attr->sample_phys_addr = sample_phys_addr;
	return count;
}

static ssize_t sample_weight_struct_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	struct damon_sysfs_perf_event_attr *perf_event_attr = container_of(kobj,
			struct damon_sysfs_perf_event_attr, kobj);

	return sysfs_emit(buf, "%d\n", perf_event_attr->sample_weight_struct);
}

static ssize_t sample_weight_struct_store(struct kobject *kobj,
		struct kobj_attribute *attr, const char *buf, size_t count)
{
	struct damon_sysfs_perf_event_attr *perf_event_attr = container_of(kobj,
			struct damon_sysfs_perf_event_attr, kobj);
	bool sample_weight_struct;
	int err = kstrtobool(buf, &sample_weight_struct);

	if (err)
		return -EINVAL;

	perf_event_attr->sample_weight_struct = sample_weight_struct;
	return count;
}

static ssize_t sample_freq_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	struct damon_sysfs_perf_event_attr *perf_event_attr = container_of(kobj,
			struct damon_sysfs_perf_event_attr, kobj);

	return sysfs_emit(buf, "%llu\n", perf_event_attr->sample_freq);
}

static ssize_t sample_freq_store(struct kobject *kobj,
		struct kobj_attribute *attr, const char *buf, size_t count)
{
	struct damon_sysfs_perf_event_attr *perf_event_attr = container_of(kobj,
			struct damon_sysfs_perf_event_attr, kobj);
	int err = kstrtou64(buf, 0, &perf_event_attr->sample_freq);

	if (err)
		return -EINVAL;
	return count;
}

static ssize_t wakeup_events_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	struct damon_sysfs_perf_event_attr *perf_event_attr = container_of(kobj,
			struct damon_sysfs_perf_event_attr, kobj);

	return sysfs_emit(buf, "%u\n", perf_event_attr->wakeup_events);
}

static ssize_t wakeup_events_store(struct kobject *kobj,
		struct kobj_attribute *attr, const char *buf, size_t count)
{
	struct damon_sysfs_perf_event_attr *perf_event_attr = container_of(kobj,
			struct damon_sysfs_perf_event_attr, kobj);
	int err = kstrtou32(buf, 0, &perf_event_attr->wakeup_events);

	if (err)
		return -EINVAL;
	return count;
}

static ssize_t precise_ip_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	struct damon_sysfs_perf_event_attr *perf_event_attr = container_of(kobj,
			struct damon_sysfs_perf_event_attr, kobj);

	return sysfs_emit(buf, "%u\n", perf_event_attr->precise_ip);
}

static ssize_t precise_ip_store(struct kobject *kobj,
		struct kobj_attribute *attr, const char *buf, size_t count)
{
	struct damon_sysfs_perf_event_attr *perf_event_attr = container_of(kobj,
			struct damon_sysfs_perf_event_attr, kobj);
	int err = kstrtou32(buf, 0, &perf_event_attr->precise_ip);

	if (err)
		return -EINVAL;
	return count;
}

static ssize_t freq_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	struct damon_sysfs_perf_event_attr *perf_event_attr = container_of(kobj,
			struct damon_sysfs_perf_event_attr, kobj);

	return sysfs_emit(buf, "%d\n", perf_event_attr->freq);
}

static ssize_t freq_store(struct kobject *kobj,
		struct kobj_attribute *attr, const char *buf, size_t count)
{
	struct damon_sysfs_perf_event_attr *perf_event_attr = container_of(kobj,
			struct damon_sysfs_perf_event_attr, kobj);
	bool freq;
	int err = kstrtobool(buf, &freq);

	if (err)
		return -EINVAL;
	perf_event_attr->freq = freq;
	return count;
}

static ssize_t sample_period_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	struct damon_sysfs_perf_event_attr *perf_event_attr = container_of(kobj,
			struct damon_sysfs_perf_event_attr, kobj);

	return sysfs_emit(buf, "%llu\n", perf_event_attr->sample_period);
}

static ssize_t sample_period_store(struct kobject *kobj,
		struct kobj_attribute *attr, const char *buf, size_t count)
{
	struct damon_sysfs_perf_event_attr *perf_event_attr = container_of(kobj,
			struct damon_sysfs_perf_event_attr, kobj);
	int err = kstrtou64(buf, 0, &perf_event_attr->sample_period);

	if (err)
		return -EINVAL;
	return count;
}

static ssize_t exclude_kernel_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	struct damon_sysfs_perf_event_attr *perf_event_attr = container_of(kobj,
			struct damon_sysfs_perf_event_attr, kobj);

	return sysfs_emit(buf, "%d\n", perf_event_attr->exclude_kernel);
}

static ssize_t exclude_kernel_store(struct kobject *kobj,
		struct kobj_attribute *attr, const char *buf, size_t count)
{
	struct damon_sysfs_perf_event_attr *perf_event_attr = container_of(kobj,
			struct damon_sysfs_perf_event_attr, kobj);
	bool v;
	int err = kstrtobool(buf, &v);

	if (err)
		return -EINVAL;
	perf_event_attr->exclude_kernel = v;
	return count;
}

static ssize_t exclude_hv_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	struct damon_sysfs_perf_event_attr *perf_event_attr = container_of(kobj,
			struct damon_sysfs_perf_event_attr, kobj);

	return sysfs_emit(buf, "%d\n", perf_event_attr->exclude_hv);
}

static ssize_t exclude_hv_store(struct kobject *kobj,
		struct kobj_attribute *attr, const char *buf, size_t count)
{
	struct damon_sysfs_perf_event_attr *perf_event_attr = container_of(kobj,
			struct damon_sysfs_perf_event_attr, kobj);
	bool v;
	int err = kstrtobool(buf, &v);

	if (err)
		return -EINVAL;
	perf_event_attr->exclude_hv = v;
	return count;
}

static void damon_sysfs_perf_event_attr_release(struct kobject *kobj)
{
	kfree(container_of(kobj, struct damon_sysfs_perf_event_attr, kobj));
}

static struct kobj_attribute damon_sysfs_perf_event_attr_type_attr =
		__ATTR(type, 0600, attr_type_show, attr_type_store);

static struct kobj_attribute damon_sysfs_perf_event_attr_config_attr =
		__ATTR_RW_MODE(config, 0600);

static struct kobj_attribute damon_sysfs_perf_event_attr_config1_attr =
		__ATTR_RW_MODE(config1, 0600);

static struct kobj_attribute damon_sysfs_perf_event_attr_config2_attr =
		__ATTR_RW_MODE(config2, 0600);

static struct kobj_attribute damon_sysfs_perf_event_attr_sample_phys_addr_attr =
		__ATTR_RW_MODE(sample_phys_addr, 0600);

static struct kobj_attribute
		damon_sysfs_perf_event_attr_sample_weight_struct_attr =
		__ATTR_RW_MODE(sample_weight_struct, 0600);

static struct kobj_attribute damon_sysfs_perf_event_attr_sample_freq_attr =
		__ATTR_RW_MODE(sample_freq, 0600);

static struct kobj_attribute damon_sysfs_perf_event_attr_wakeup_events_attr =
		__ATTR_RW_MODE(wakeup_events, 0600);

static struct kobj_attribute damon_sysfs_perf_event_attr_precise_ip_attr =
		__ATTR_RW_MODE(precise_ip, 0600);

static struct kobj_attribute damon_sysfs_perf_event_attr_freq_attr =
		__ATTR_RW_MODE(freq, 0600);

static struct kobj_attribute damon_sysfs_perf_event_attr_sample_period_attr =
		__ATTR_RW_MODE(sample_period, 0600);

static struct kobj_attribute damon_sysfs_perf_event_attr_exclude_kernel_attr =
		__ATTR_RW_MODE(exclude_kernel, 0600);

static struct kobj_attribute damon_sysfs_perf_event_attr_exclude_hv_attr =
		__ATTR_RW_MODE(exclude_hv, 0600);

static struct attribute *damon_sysfs_perf_event_attr_attrs[] = {
	&damon_sysfs_perf_event_attr_type_attr.attr,
	&damon_sysfs_perf_event_attr_config_attr.attr,
	&damon_sysfs_perf_event_attr_config1_attr.attr,
	&damon_sysfs_perf_event_attr_config2_attr.attr,
	&damon_sysfs_perf_event_attr_sample_phys_addr_attr.attr,
	&damon_sysfs_perf_event_attr_sample_weight_struct_attr.attr,
	&damon_sysfs_perf_event_attr_freq_attr.attr,
	&damon_sysfs_perf_event_attr_sample_freq_attr.attr,
	&damon_sysfs_perf_event_attr_sample_period_attr.attr,
	&damon_sysfs_perf_event_attr_wakeup_events_attr.attr,
	&damon_sysfs_perf_event_attr_precise_ip_attr.attr,
	&damon_sysfs_perf_event_attr_exclude_kernel_attr.attr,
	&damon_sysfs_perf_event_attr_exclude_hv_attr.attr,
	NULL,
};
ATTRIBUTE_GROUPS(damon_sysfs_perf_event_attr);

static const struct kobj_type damon_sysfs_perf_event_attr_ktype = {
	.release = damon_sysfs_perf_event_attr_release,
	.sysfs_ops = &kobj_sysfs_ops,
	.default_groups = damon_sysfs_perf_event_attr_groups,
};

/*
 * perf_events directory
 */

/*
 * Cap on the number of perf events per damon_ctx, to bound the sysfs
 * kobject footprint and prevent unbounded allocations from a careless
 * write to nr_perf_events.
 */
#define DAMON_SYSFS_PERF_EVENTS_MAX	64

struct damon_sysfs_perf_events {
	struct kobject kobj;
	struct damon_sysfs_perf_event_attr **attrs_arr;
	int nr;
};

static struct damon_sysfs_perf_events *damon_sysfs_perf_events_alloc(void)
{
	return kzalloc(sizeof(struct damon_sysfs_perf_events), GFP_KERNEL);
}

static void damon_sysfs_perf_events_rm_dirs(
		struct damon_sysfs_perf_events *events)
{
	struct damon_sysfs_perf_event_attr **attrs_arr = events->attrs_arr;
	int i;

	for (i = 0; i < events->nr; i++)
		kobject_put(&attrs_arr[i]->kobj);
	events->nr = 0;
	kfree(attrs_arr);
	events->attrs_arr = NULL;
}

static int damon_sysfs_perf_events_add_dirs(
		struct damon_sysfs_perf_events *events, int nr_events)
{
	struct damon_sysfs_perf_event_attr **attrs_arr, *attr;
	int err, i;

	damon_sysfs_perf_events_rm_dirs(events);
	if (!nr_events)
		return 0;

	attrs_arr = kmalloc_array(nr_events, sizeof(*attrs_arr), GFP_KERNEL);
	if (!attrs_arr)
		return -ENOMEM;
	events->attrs_arr = attrs_arr;

	for (i = 0; i < nr_events; i++) {
		attr = damon_sysfs_perf_event_attr_alloc();
		if (!attr) {
			damon_sysfs_perf_events_rm_dirs(events);
			return -ENOMEM;
		}

		err = kobject_init_and_add(&attr->kobj,
				&damon_sysfs_perf_event_attr_ktype, &events->kobj,
				"%d", i);
		if (err) {
			kobject_put(&attr->kobj);
			damon_sysfs_perf_events_rm_dirs(events);
			return err;
		}
		attrs_arr[i] = attr;
		events->nr++;
	}
	return 0;
}

static ssize_t nr_perf_events_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	struct damon_sysfs_perf_events *events = container_of(kobj,
			struct damon_sysfs_perf_events, kobj);

	return sysfs_emit(buf, "%d\n", events->nr);
}

static ssize_t nr_perf_events_store(struct kobject *kobj,
		struct kobj_attribute *attr, const char *buf, size_t count)
{
	struct damon_sysfs_perf_events *events;
	int nr, err = kstrtoint(buf, 0, &nr);

	if (err)
		return err;
	if (nr < 0 || nr > DAMON_SYSFS_PERF_EVENTS_MAX)
		return -EINVAL;

	events = container_of(kobj, struct damon_sysfs_perf_events, kobj);

	if (!mutex_trylock(&damon_sysfs_lock))
		return -EBUSY;
	err = damon_sysfs_perf_events_add_dirs(events, nr);
	mutex_unlock(&damon_sysfs_lock);
	if (err)
		return err;

	return count;
}

static void damon_sysfs_perf_events_release(struct kobject *kobj)
{
	kfree(container_of(kobj, struct damon_sysfs_perf_events, kobj));
}

static struct kobj_attribute damon_sysfs_perf_events_nr_attr =
		__ATTR_RW_MODE(nr_perf_events, 0600);

static struct attribute *damon_sysfs_perf_events_attrs[] = {
	&damon_sysfs_perf_events_nr_attr.attr,
	NULL,
};
ATTRIBUTE_GROUPS(damon_sysfs_perf_events);

static const struct kobj_type damon_sysfs_perf_events_ktype = {
	.release = damon_sysfs_perf_events_release,
	.sysfs_ops = &kobj_sysfs_ops,
	.default_groups = damon_sysfs_perf_events_groups,
};

/*
 * sample directory
 */

struct damon_sysfs_sample *damon_sysfs_sample_alloc(void)
{
	struct damon_sysfs_sample *sample = kmalloc(
			sizeof(*sample), GFP_KERNEL);

	if (!sample)
		return NULL;
	sample->kobj = (struct kobject){};
	return sample;
}

int damon_sysfs_sample_add_dirs(struct damon_sysfs_sample *sample)
{
	struct damon_sysfs_primitives *primitives;
	struct damon_sysfs_sample_filters *filters;
	struct damon_sysfs_perf_events *perf_events;
	int err;

	primitives = damon_sysfs_primitives_alloc(true, false);
	if (!primitives)
		return -ENOMEM;
	err = kobject_init_and_add(&primitives->kobj,
			&damon_sysfs_primitives_ktype, &sample->kobj,
			"primitives");
	if (err)
		goto put_primitives_out;
	sample->primitives = primitives;

	filters = damon_sysfs_sample_filters_alloc();
	if (!filters) {
		err = -ENOMEM;
		goto put_primitives_out;
	}
	err = kobject_init_and_add(&filters->kobj,
			&damon_sysfs_sample_filters_ktype, &sample->kobj,
			"filters");
	if (err)
		goto put_filters_out;
	sample->filters = filters;

	perf_events = damon_sysfs_perf_events_alloc();
	if (!perf_events) {
		err = -ENOMEM;
		goto put_filters_out;
	}
	err = kobject_init_and_add(&perf_events->kobj,
			&damon_sysfs_perf_events_ktype, &sample->kobj,
			"perf_events");
	if (err)
		goto put_perf_events_out;
	sample->perf_events = perf_events;

	return 0;
put_perf_events_out:
	kobject_put(&perf_events->kobj);
	sample->perf_events = NULL;
put_filters_out:
	kobject_put(&filters->kobj);
	sample->filters = NULL;
put_primitives_out:
	kobject_put(&primitives->kobj);
	sample->primitives = NULL;
	return err;
}

void damon_sysfs_sample_rm_dirs(struct damon_sysfs_sample *sample)
{
	if (sample->primitives)
		kobject_put(&sample->primitives->kobj);
	if (sample->filters) {
		damon_sysfs_sample_filters_rm_dirs(sample->filters);
		kobject_put(&sample->filters->kobj);
	}
	if (sample->perf_events) {
		damon_sysfs_perf_events_rm_dirs(sample->perf_events);
		kobject_put(&sample->perf_events->kobj);
	}
}

void damon_sysfs_sample_release(struct kobject *kobj)
{
	kfree(container_of(kobj, struct damon_sysfs_sample, kobj));
}

static struct attribute *damon_sysfs_sample_attrs[] = {
	NULL,
};
ATTRIBUTE_GROUPS(damon_sysfs_sample);

const struct kobj_type damon_sysfs_sample_ktype = {
	.release = damon_sysfs_sample_release,
	.sysfs_ops = &kobj_sysfs_ops,
	.default_groups = damon_sysfs_sample_groups,
};

static int damon_sysfs_set_threads_filter(struct damon_sample_filter *filter,
		int *sysfs_tid_arr)
{
	int nr_tids, i;
	pid_t *tid_arr;

	if (!sysfs_tid_arr)
		return -EINVAL;
	nr_tids = sysfs_tid_arr[0];
	tid_arr = kmalloc_array(nr_tids, sizeof(*tid_arr), GFP_KERNEL);
	if (!tid_arr)
		return -ENOMEM;
	for (i = 0; i < nr_tids; i++)
		tid_arr[i] = sysfs_tid_arr[i + 1];
	filter->tid_arr = tid_arr;
	filter->nr_tids = nr_tids;
	return 0;
}

static int damon_sysfs_set_sample_filters(
		struct damon_sample_control *control,
		struct damon_sysfs_sample_filters *sysfs_filters)
{
	int i, err;

	for (i = 0; i < sysfs_filters->nr; i++) {
		struct damon_sysfs_sample_filter *sysfs_filter =
			sysfs_filters->filters_arr[i];
		struct damon_sample_filter *filter;

		filter = damon_new_sample_filter(
				sysfs_filter->type, sysfs_filter->matching,
				sysfs_filter->allow);
		if (!filter)
			return -ENOMEM;
		switch (filter->type) {
		case DAMON_FILTER_TYPE_CPUMASK:
			filter->cpumask = sysfs_filter->cpumask;
			break;
		case DAMON_FILTER_TYPE_THREADS:
			err = damon_sysfs_set_threads_filter(filter,
					sysfs_filter->tid_arr);
			if (err)
				damon_free_sample_filter(filter);
			break;
		default:
			break;
		}
		damon_add_sample_filter(control, filter);
	}
	return 0;
}


int damon_sysfs_set_sample_control(
		struct damon_sample_control *control,
		struct damon_sysfs_sample *sysfs_sample)
{
	control->primitives_enabled.page_table =
		sysfs_sample->primitives->page_table;
	control->primitives_enabled.page_fault =
		sysfs_sample->primitives->page_fault;

	return damon_sysfs_set_sample_filters(control,
			sysfs_sample->filters);
}

static int damon_sysfs_add_perf_event(
		struct damon_sysfs_perf_event_attr *sys_attr,
		struct damon_ctx *ctx)
{
	struct damon_perf_event *event = kzalloc(sizeof(*event), GFP_KERNEL);

	if (!event)
		return -ENOMEM;

	event->attr.type = sys_attr->type;
	event->attr.config = sys_attr->config;
	event->attr.config1 = sys_attr->config1;
	event->attr.config2 = sys_attr->config2;
	event->attr.sample_phys_addr = sys_attr->sample_phys_addr;
	event->attr.sample_weight_struct = sys_attr->sample_weight_struct;
	event->attr.freq = sys_attr->freq;
	event->attr.sample_freq = sys_attr->sample_freq;
	event->attr.sample_period = sys_attr->sample_period;
	event->attr.wakeup_events = sys_attr->wakeup_events;
	event->attr.precise_ip = sys_attr->precise_ip;
	event->attr.exclude_kernel = sys_attr->exclude_kernel;
	event->attr.exclude_hv = sys_attr->exclude_hv;

	list_add_tail(&event->list, &ctx->perf_events);
	return 0;
}

int damon_sysfs_add_perf_events(struct damon_ctx *ctx,
		struct damon_sysfs_sample *sysfs_sample)
{
	struct damon_sysfs_perf_events *events = sysfs_sample->perf_events;
	int i, err;

	if (!events)
		return 0;

	for (i = 0; i < events->nr; i++) {
		err = damon_sysfs_add_perf_event(events->attrs_arr[i], ctx);
		if (err)
			return err;
	}
	return 0;
}
