// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright © 2024 Intel Corporation
 */

#define pr_fmt(fmt) "%s: " fmt, KBUILD_MODNAME

#include <linux/debugfs.h>
#include <linux/init.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/perf_event.h>
#include <linux/random.h>
#include <linux/seq_file.h>
#include <linux/types.h>

struct dummy_mod {
	struct dentry *debugfs_root;

	struct list_head device_list;
	struct mutex mutex;
};

struct dummy_pmu {
	struct pmu base;
	char *name;
	bool registered;
};

struct dummy_device {
	unsigned int instance;
	struct kref refcount;
	struct list_head mod_entry;
	struct dummy_pmu pmu;
};

static struct dummy_mod dm;

static void device_release(struct kref *ref);

static struct dummy_pmu *event_to_pmu(struct perf_event *event)
{
	return container_of(event->pmu, struct dummy_pmu, base);
}

static struct dummy_device *pmu_to_device(struct dummy_pmu *pmu)
{
	return container_of(pmu, struct dummy_device, pmu);
}

static struct dummy_pmu *pmu_to_dummy(struct pmu *pmu)
{
	return container_of(pmu, struct dummy_pmu, base);
}

static ssize_t dummy_pmu_events_sysfs_show(struct device *dev,
					   struct device_attribute *attr,
					   char *page)
{
	struct perf_pmu_events_attr *pmu_attr;

	pmu_attr = container_of(attr, struct perf_pmu_events_attr, attr);

	return sprintf(page, "event=0x%04llx\n", pmu_attr->id);
}

#define DUMMY_PMU_EVENT_ATTR(name, config)				\
	PMU_EVENT_ATTR_ID(name, dummy_pmu_events_sysfs_show, config)

PMU_FORMAT_ATTR(event, "config:0-63");

#define DUMMY1_CONFIG 0x01
#define DUMMY2_CONFIG 0x02

static struct attribute *dummy_pmu_event_attrs[] = {
	DUMMY_PMU_EVENT_ATTR(test-event-1, DUMMY1_CONFIG),
	DUMMY_PMU_EVENT_ATTR(test-event-2, DUMMY2_CONFIG),
	NULL,
};

static struct attribute *dummy_pmu_format_attrs[] = {
	&format_attr_event.attr,
	NULL,
};
static const struct attribute_group dummy_pmu_events_attr_group = {
	.name = "events",
	.attrs = dummy_pmu_event_attrs,
};
static const struct attribute_group dummy_pmu_format_attr_group = {
	.name = "format",
	.attrs = dummy_pmu_format_attrs,
};
static const struct attribute_group *attr_groups[] = {
	&dummy_pmu_format_attr_group,
	&dummy_pmu_events_attr_group,
	NULL,
};

static int dummy_pmu_event_init(struct perf_event *event)
{
	struct dummy_pmu *pmu = event_to_pmu(event);

	if (!pmu->registered)
		return -ENODEV;

	if (event->attr.type != event->pmu->type)
		return -ENOENT;

	/* unsupported modes and filters */
	if (event->attr.sample_period) /* no sampling */
		return -EINVAL;

	if (has_branch_stack(event))
		return -EOPNOTSUPP;

	if (event->cpu < 0)
		return -EINVAL;

	return 0;
}

static void dummy_pmu_event_start(struct perf_event *event, int flags)
{
	struct dummy_pmu *pmu = event_to_pmu(event);

	if (!pmu->registered)
		return;

	event->hw.state = 0;
}

static void dummy_pmu_event_read(struct perf_event *event)
{
	struct dummy_pmu *pmu = event_to_pmu(event);
	u8 buf;

	if (!pmu->registered) {
		event->hw.state = PERF_HES_STOPPED;
		return;
	}

	get_random_bytes(&buf, 1);
	buf %= 10;

	switch (event->attr.config & 0xf) {
	case DUMMY1_CONFIG:
		break;
	case DUMMY2_CONFIG:
		buf *= 2;
		break;
	}

	local64_add(buf, &event->count);
}

static void dummy_pmu_event_stop(struct perf_event *event, int flags)
{
	struct dummy_pmu *pmu = event_to_pmu(event);

	if (!pmu->registered)
		goto out;

	if (flags & PERF_EF_UPDATE)
		dummy_pmu_event_read(event);

out:
	event->hw.state = PERF_HES_STOPPED;
}

static int dummy_pmu_event_add(struct perf_event *event, int flags)
{
	struct dummy_pmu *pmu = event_to_pmu(event);

	if (!pmu->registered)
		return -ENODEV;

	if (flags & PERF_EF_START)
		dummy_pmu_event_start(event, flags);

	return 0;

}

static void dummy_pmu_event_del(struct perf_event *event, int flags)
{
	dummy_pmu_event_stop(event, PERF_EF_UPDATE);
}

static struct pmu *dummy_pmu_get(struct pmu *pmu)
{
	struct dummy_device *d = pmu_to_device(pmu_to_dummy(pmu));

	kref_get(&d->refcount);

	return pmu;
}

static void dummy_pmu_put(struct pmu *pmu)
{
	struct dummy_device *d = pmu_to_device(pmu_to_dummy(pmu));

	kref_put(&d->refcount, device_release);
}

static int device_init(struct dummy_device *d)
{
	int ret;

	if (WARN_ONCE(d->pmu.name, "Cannot re-register pmu.\n"))
		return -EINVAL;

	d->pmu.base = (struct pmu){
		.attr_groups	= attr_groups,
		.module		= THIS_MODULE,
		.task_ctx_nr	= perf_invalid_context,
		.event_init	= dummy_pmu_event_init,
		.add		= dummy_pmu_event_add,
		.del		= dummy_pmu_event_del,
		.start		= dummy_pmu_event_start,
		.stop		= dummy_pmu_event_stop,
		.read		= dummy_pmu_event_read,
		.get		= dummy_pmu_get,
		.put		= dummy_pmu_put,
	};

	d->pmu.name = kasprintf(GFP_KERNEL, "dummy_pmu_%u", d->instance);
	if (!d->pmu.name)
		return -ENOMEM;

	ret = perf_pmu_register(&d->pmu.base, d->pmu.name, -1);
	if (ret)
		goto fail;

	d->pmu.registered = true;
	pr_info("Device registered: %s\n", d->pmu.name);

	return 0;

fail:
	/*
	 * See device_release: if name is non-NULL, dummy_pmu was registered
	 * with perf and needs cleanup
	 */
	kfree(d->pmu.name);
	d->pmu.name = NULL;

	return ret;
}

static void device_exit(struct dummy_device *d)
{
	d->pmu.registered = false;
	perf_pmu_unregister(&d->pmu.base);

	pr_info("Device released: %s\n", d->pmu.name);
}

static void device_release(struct kref *ref)
{
	struct dummy_device *d = container_of(ref, struct dummy_device, refcount);

	if (d->pmu.name) {
		perf_pmu_free(&d->pmu.base);
		kfree(d->pmu.name);
	}
	kfree(d);
}

static struct dummy_device *find_device_locked(struct dummy_mod *m, unsigned int instance)
{
	struct dummy_device *d;

	list_for_each_entry(d, &m->device_list, mod_entry)
		if (d->instance == instance)
			return d;

	return NULL;
}

static int dummy_add_device(struct dummy_mod *m, unsigned int instance)
{
	struct dummy_device *d, *d2;
	int ret = 0;

	mutex_lock(&m->mutex);
	d = find_device_locked(m, instance);
	mutex_unlock(&m->mutex);
	if (d)
		return -EINVAL;

	d = kcalloc(1, sizeof(*d), GFP_KERNEL);
	if (!d)
		return -ENOMEM;

	kref_init(&d->refcount);
	d->instance = instance;

	ret = device_init(d);
	if (ret < 0)
		goto fail_put;

	mutex_lock(&m->mutex);
	d2 = find_device_locked(m, instance);
	if (d2) {
		mutex_unlock(&m->mutex);
		ret = -EINVAL;
		goto fail_exit;
	}
	list_add(&d->mod_entry, &m->device_list);
	mutex_unlock(&m->mutex);

	return 0;

fail_exit:
	device_exit(d);
fail_put:
	kref_put(&d->refcount, device_release);
	return ret;
}

static int dummy_del_device(struct dummy_mod *m, unsigned int instance)
{
	struct dummy_device *d, *found = NULL;

	mutex_lock(&m->mutex);
	list_for_each_entry(d, &m->device_list, mod_entry) {
		if (d->instance == instance) {
			list_del(&d->mod_entry);
			found = d;
			break;
		}
	}
	mutex_unlock(&m->mutex);

	if (!found)
		return -EINVAL;

	device_exit(found);
	kref_put(&found->refcount, device_release);

	return 0;
}

static int parse_device(const char __user *ubuf, size_t size, u32 *instance)
{
	char buf[16];
	ssize_t len;

	if (size > sizeof(buf) - 1)
		return -E2BIG;

	len = strncpy_from_user(buf, ubuf, sizeof(buf));
	if (len < 0 || len >= sizeof(buf) - 1)
		return -E2BIG;

	if (kstrtou32(buf, 0, instance))
		return -EINVAL;

	return size;
}

static int bind_show(struct seq_file *s, void *unused)
{
	struct dummy_mod *m = s->private;
	struct dummy_device *d;

	mutex_lock(&m->mutex);
	list_for_each_entry(d, &m->device_list, mod_entry)
		seq_printf(s, "%u\n", d->instance);
	mutex_unlock(&m->mutex);

	return 0;
}

static ssize_t bind_write(struct file *f, const char __user *ubuf,
			  size_t size, loff_t *pos)
{
	struct dummy_mod *m = file_inode(f)->i_private;
	u32 instance;
	ssize_t ret;

	ret = parse_device(ubuf, size, &instance);
	if (ret < 0)
		return ret;

	ret = dummy_add_device(m, instance);
	if (ret < 0)
		return ret;

	return size;
}
DEFINE_SHOW_STORE_ATTRIBUTE(bind);

static int unbind_show(struct seq_file *s, void *unused)
{
	return -EPERM;
}

static ssize_t unbind_write(struct file *f, const char __user *ubuf,
			    size_t size, loff_t *pos)
{
	struct dummy_mod *m = file_inode(f)->i_private;
	unsigned int instance;
	ssize_t ret;

	ret = parse_device(ubuf, size, &instance);
	if (ret < 0)
		return ret;

	ret = dummy_del_device(m, instance);
	if (ret < 0)
		return ret;

	return size;
}
DEFINE_SHOW_STORE_ATTRIBUTE(unbind);

static int __init dummy_init(void)
{
	struct dentry *dir;

	dir = debugfs_create_dir(KBUILD_MODNAME, NULL);
	debugfs_create_file("bind", 0600, dir, &dm, &bind_fops);
	debugfs_create_file("unbind", 0200, dir, &dm, &unbind_fops);

	dm.debugfs_root = dir;
	INIT_LIST_HEAD(&dm.device_list);
	mutex_init(&dm.mutex);

	return 0;
}

static void dummy_exit(void)
{
	struct dummy_device *d, *tmp;

	debugfs_remove_recursive(dm.debugfs_root);

	mutex_lock(&dm.mutex);
	list_for_each_entry_safe(d, tmp, &dm.device_list, mod_entry) {
		device_exit(d);
		kref_put(&d->refcount, device_release);
	}
	mutex_unlock(&dm.mutex);
}

module_init(dummy_init);
module_exit(dummy_exit);

MODULE_AUTHOR("Lucas De Marchi <lucas.demarchi@intel.com>");
MODULE_LICENSE("GPL");
