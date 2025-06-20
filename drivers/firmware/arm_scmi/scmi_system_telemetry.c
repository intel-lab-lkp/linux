// SPDX-License-Identifier: GPL-2.0
/*
 * SCMI - System Telemetry Driver
 *
 * Copyright (C) 2025 ARM Ltd.
 */

#include <linux/atomic.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/kstrtox.h>
#include <linux/module.h>
#include <linux/scmi_protocol.h>
#include <linux/sysfs.h>
#include <linux/slab.h>

#include <uapi/linux/scmi.h>

#define MAX_BULK_LINE_CHAR_LENGTH	64

struct scmi_tlm_setup;

static atomic_t scmi_tlm_instance_count = ATOMIC_INIT(0);

struct scmi_tlm_grp_dev {
	struct device dev;
	struct scmi_tlm_setup *tsp;
	const struct scmi_telemetry_group *grp;
};

#define to_tlm_grp_dev(d)					\
	(container_of((d), struct scmi_tlm_grp_dev, dev))

struct scmi_tlm_de_dev {
	struct device dev;
	struct scmi_tlm_setup *tsp;
	const struct scmi_telemetry_de *de;
};

#define to_tlm_de_dev(d)					\
	(container_of((d), struct scmi_tlm_de_dev, dev))

struct scmi_tlm_ioctls_db {
	struct scmi_tlm_info tlm_info;
	struct scmi_tlm_intervals *tlm_intervals;
	struct scmi_tlm_intervals **tlm_grp_intervals;
	struct scmi_tlm_des_list *tlm_des_list;
};

struct scmi_tlm_instance {
	struct device dev;
	struct cdev cdev;
	struct device des_dev;
	struct device groups_dev;
	struct scmi_tlm_de_dev **des;
	struct scmi_tlm_setup *tsp;
	const struct scmi_telemetry_info *info;
	struct scmi_tlm_ioctls_db io_db;
};

#define dev_to_tlm_instance(d)	\
	(container_of((d), struct scmi_tlm_instance, dev))

#define des_dev_to_tlm_instance(e)	\
	(container_of((e), struct scmi_tlm_instance, des_dev))

#define groups_dev_to_tlm_instance(e)	\
	(container_of((e), struct scmi_tlm_instance, groups_dev))

/**
 * struct scmi_tlm_setup  - Telemetry setup descriptor
 * @sdev: A reference to the related SCMI device
 * @ops: A reference to the protocol ops
 * @ph: A reference to the protocol handle to be used with the ops
 * @priv: A reference to optional driver-specific data
 */
struct scmi_tlm_setup {
	struct scmi_device *sdev;
	const struct scmi_telemetry_proto_ops *ops;
	struct scmi_protocol_handle *ph;
	const void *priv;
};

static int scmi_tlm_major;

static void scmi_telemetry_release(struct device *dev)
{
}

static ssize_t __all_enable_store(struct device *dev,
				  struct device_attribute *attr,
				  const char *buf, size_t len,
				  bool is_enable_entry)
{
	struct scmi_tlm_instance *ti = dev_to_tlm_instance(dev);
	struct scmi_tlm_setup *tsp = ti->tsp;
	bool enable;
	int ret;

	if (kstrtobool(buf, &enable))
		return -EINVAL;

	if (is_enable_entry && !enable) {
		ret = tsp->ops->all_disable(tsp->ph, false);
		if (ret)
			return ret;
	} else {
		for (int i = 0; i < ti->info->num_de; i++) {
			const struct scmi_telemetry_de *de = ti->info->des[i];

			ret = tsp->ops->state_set(tsp->ph, false, de->id,
						   is_enable_entry ? &enable : NULL,
						   !is_enable_entry ? &enable : NULL);
			if (ret)
				return ret;
		}
	}

	return len;
}

static ssize_t all_des_enable_store(struct device *dev,
				    struct device_attribute *attr,
				    const char *buf, size_t len)
{
	return __all_enable_store(dev, attr, buf, len, true);
}

static ssize_t all_des_tstamp_enable_store(struct device *dev,
					   struct device_attribute *attr,
					   const char *buf, size_t len)
{
	return __all_enable_store(dev, attr, buf, len, false);
}

static inline ssize_t __current_update_show(char *buf,
					    unsigned int active_update_interval)
{
	return sysfs_emit(buf, "%u\n",
			  SCMI_GET_UPDATE_INTERVAL_SECS(active_update_interval));
}

static inline ssize_t __current_update_store(struct scmi_tlm_setup *tsp,
					     const char *buf, size_t len,
					     unsigned int grp_id)
{
	bool grp_ignore = grp_id == SCMI_TLM_GRP_INVALID ? true : false;
	unsigned int update_interval_ms = 0;
	int ret;

	ret = kstrtouint(buf, 0, &update_interval_ms);
	if (ret)
		return ret;

	ret = tsp->ops->collection_configure(tsp->ph, grp_id, grp_ignore, NULL,
					     &update_interval_ms, NULL);
	if (ret)
		return ret;

	return len;
}

static ssize_t current_update_interval_ms_show(struct device *dev,
					       struct device_attribute *attr,
					       char *buf)
{
	struct scmi_tlm_instance *ti = dev_to_tlm_instance(dev);

	return __current_update_show(buf,
				     ti->info->intervals.active_update_interval);
}

static ssize_t current_update_interval_ms_store(struct device *dev,
						struct device_attribute *attr,
						const char *buf, size_t len)
{
	struct scmi_tlm_instance *ti = dev_to_tlm_instance(dev);
	struct scmi_tlm_setup *tsp = ti->tsp;

	return __current_update_store(tsp, buf, len, SCMI_TLM_GRP_INVALID);
}

static ssize_t tlm_enable_show(struct device *dev,
			       struct device_attribute *attr,
			       char *buf)
{
	struct scmi_tlm_instance *ti = dev_to_tlm_instance(dev);

	return sysfs_emit(buf, "%c\n", ti->info->enabled ? 'Y' : 'N');
}

static ssize_t tlm_enable_store(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t len)
{
	struct scmi_tlm_instance *ti = dev_to_tlm_instance(dev);
	struct scmi_tlm_setup *tsp = ti->tsp;
	enum scmi_telemetry_collection mode = SCMI_TLM_ONDEMAND;
	bool enabled;
	int ret;

	ret = kstrtobool(buf, &enabled);
	if (ret)
		return ret;

	ret = tsp->ops->collection_configure(tsp->ph, SCMI_TLM_GRP_INVALID, true,
					     &enabled, NULL, &mode);
	if (ret)
		return ret;

	return len;
}

static int scmi_tlm_buffer_fill(struct device *dev, char *buf, size_t size,
				int *len, int num,
				struct scmi_telemetry_de_sample *samples)
{
	int idx, bytes = 0;

	/* Loop till there space for the next line */
	for (idx = 0; idx < num && size - bytes >= MAX_BULK_LINE_CHAR_LENGTH; idx++) {
		bytes += snprintf(buf + bytes, size - bytes,
				  "0x%04X %llu %016llX\n", samples[idx].id,
				  samples[idx].tstamp, samples[idx].val);
	}

	if (idx < num) {
		dev_err(dev, "Bulk buffer truncated !\n");
		return -ENOSPC;
	}

	if (len)
		*len = bytes;

	return 0;
}

static inline ssize_t __des_bulk_read_show(struct scmi_tlm_instance *ti,
					   unsigned int grp_id, char *buf,
					   int size)
{
	struct scmi_telemetry_de_sample *samples;
	struct scmi_tlm_setup *tsp = ti->tsp;
	int ret, num;

	num = ti->info->num_de;
	samples = kcalloc(num, sizeof(*samples), GFP_KERNEL);
	if (!samples)
		return -ENOMEM;

	ret = tsp->ops->des_bulk_read(tsp->ph, grp_id, &num, samples);
	if (ret) {
		kfree(samples);
		return ret;
	}

	ret = scmi_tlm_buffer_fill(&ti->dev, buf, size, NULL, num, samples);
	kfree(samples);
	if (ret)
		return ret;

	return sysfs_emit(buf, "%s", buf);
}

static ssize_t des_bulk_read_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	struct scmi_tlm_instance *ti = dev_to_tlm_instance(dev);

	return __des_bulk_read_show(ti, SCMI_TLM_GRP_INVALID, buf, PAGE_SIZE);
}

static inline ssize_t __des_single_sample_read_show(struct scmi_tlm_instance *ti,
						    unsigned int grp_id,
						    char *buf, int len)
{
	struct scmi_telemetry_de_sample *samples;
	struct scmi_tlm_setup *tsp = ti->tsp;
	int ret, num, bytes = 0;

	num = ti->info->num_de;
	samples = kcalloc(num, sizeof(*samples), GFP_KERNEL);
	if (!samples)
		return -ENOMEM;

	ret = tsp->ops->des_sample_get(tsp->ph, grp_id, &num, samples);
	if (ret) {
		kfree(samples);
		return ret;
	}

	for (int i = 0; i < num; i++) {
		bytes += snprintf(buf + bytes, len - bytes,
				  "0x%04X %llu %016llX\n", samples[i].id,
				  samples[i].tstamp, samples[i].val);

		if (bytes >= len) {
			dev_err(&ti->dev, "==>> BULK BUFFER OVERFLOW !\n");
			kfree(samples);
			return -ENOSPC;
		}
	}

	kfree(samples);

	return sysfs_emit(buf, "%s", buf);
}

static ssize_t des_single_sample_read_show(struct device *dev,
					   struct device_attribute *attr, char *buf)
{
	struct scmi_tlm_instance *ti = dev_to_tlm_instance(dev);

	return __des_single_sample_read_show(ti, SCMI_TLM_GRP_INVALID,
					     buf, PAGE_SIZE);
}

static ssize_t de_implementation_version_show(struct device *dev,
					      struct device_attribute *attr,
					      char *buf)
{
	struct scmi_tlm_instance *ti = dev_to_tlm_instance(dev);

	return sysfs_emit(buf, "%pUL\n", ti->info->de_impl_version);
}

static inline ssize_t __intervals_discrete_show(char *buf, const bool discrete)
{
	return sysfs_emit(buf, "%c\n", discrete ? 'Y' : 'N');
}

static ssize_t intervals_discrete_show(struct device *dev,
				       struct device_attribute *attr,
				       char *buf)
{
	struct scmi_tlm_instance *ti = dev_to_tlm_instance(dev);

	return __intervals_discrete_show(buf, ti->info->intervals.discrete);
}

#define BUF_SZ	1024
static inline ssize_t
__available_update_show(char *buf,
			const struct scmi_telemetry_update_interval *intervals)
{
	int len = 0, num_intervals = intervals->num;
	char available[BUF_SZ];

	for (int i = 0; i < num_intervals; i++) {
		len += scnprintf(available + len, BUF_SZ - len, "%u ",
				 intervals->update_intervals[i]);
	}

	available[len - 1] = '\0';

	return sysfs_emit(buf, "%s\n", available);
}

static ssize_t available_update_intervals_ms_show(struct device *dev,
						  struct device_attribute *attr,
						  char *buf)
{
	struct scmi_tlm_instance *ti = dev_to_tlm_instance(dev);

	return __available_update_show(buf, &ti->info->intervals);
}

static ssize_t version_show(struct device *dev, struct device_attribute *attr,
			    char *buf)
{
	struct scmi_tlm_instance *ti = dev_to_tlm_instance(dev);

	return sysfs_emit(buf, "0x%08x\n", ti->info->version);
}

static DEVICE_ATTR_WO(all_des_enable);
static DEVICE_ATTR_WO(all_des_tstamp_enable);
static DEVICE_ATTR_RW(current_update_interval_ms);
static DEVICE_ATTR_RW(tlm_enable);
static DEVICE_ATTR_RO(des_bulk_read);
static DEVICE_ATTR_RO(des_single_sample_read);
static DEVICE_ATTR_RO(de_implementation_version);
static DEVICE_ATTR_RO(intervals_discrete);
static DEVICE_ATTR_RO(available_update_intervals_ms);
static DEVICE_ATTR_RO(version);

static ssize_t reset_store(struct device *dev, struct device_attribute *attr,
			   const char *buf, size_t len)
{
	struct scmi_tlm_instance *ti = dev_to_tlm_instance(dev);
	int ret;

	ret = ti->tsp->ops->reset(ti->tsp->ph);
	if (ret)
		return ret;

	return len;
}

static struct device_attribute dev_attr_reset = {
	.attr	= { .name = "reset", .mode = 0200 },
	.store	= reset_store,
};

static struct attribute *scmi_telemetry_attrs[] = {
	&dev_attr_all_des_enable.attr,
	&dev_attr_all_des_tstamp_enable.attr,
	&dev_attr_current_update_interval_ms.attr,
	&dev_attr_tlm_enable.attr,
	&dev_attr_des_bulk_read.attr,
	&dev_attr_des_single_sample_read.attr,
	&dev_attr_de_implementation_version.attr,
	&dev_attr_intervals_discrete.attr,
	&dev_attr_available_update_intervals_ms.attr,
	&dev_attr_version.attr,
	NULL,
};
ATTRIBUTE_GROUPS(scmi_telemetry);

static struct class scmi_telemetry_class = {
	.name = "scmi_telemetry",
	.dev_release = scmi_telemetry_release,
};

static ssize_t value_show(struct device *dev, struct device_attribute *attr,
			  char *buf)
{
	struct scmi_tlm_de_dev *tde = to_tlm_de_dev(dev);
	struct scmi_tlm_setup *tsp = tde->tsp;
	struct scmi_telemetry_de_sample sample;
	int ret;

	sample.id = tde->de->id;
	ret = tsp->ops->de_data_read(tsp->ph, &sample);
	if (ret)
		return ret;

	return sysfs_emit(buf, "%llu: %016llX\n", sample.tstamp, sample.val);
}
static DEVICE_ATTR_RO(value);

#define DEFINE_DE_ATTR_INT_RO(_attr, _fmt)				\
static ssize_t _attr##_show(struct device *dev,				\
			    struct device_attribute *attr,		\
			    char *buf)					\
{									\
	struct scmi_tlm_de_dev *tde = to_tlm_de_dev(dev);		\
									\
	return sysfs_emit(buf, _fmt "\n", tde->de->_attr);		\
}									\
static DEVICE_ATTR_RO(_attr)

#define DEFINE_DE_ATTR_BOOL_RO(_attr)					\
static ssize_t _attr##_show(struct device *dev,				\
			    struct device_attribute *attr,		\
			    char *buf)					\
{									\
	struct scmi_tlm_de_dev *tde = to_tlm_de_dev(dev);		\
									\
	return sysfs_emit(buf, "%c\n", tde->de->_attr ? 'Y' : 'N');	\
}									\
static DEVICE_ATTR_RO(_attr)

DEFINE_DE_ATTR_INT_RO(type, "%u");
DEFINE_DE_ATTR_INT_RO(unit, "%u");
DEFINE_DE_ATTR_INT_RO(unit_exp, "%d");
DEFINE_DE_ATTR_INT_RO(instance_id, "%u");
DEFINE_DE_ATTR_INT_RO(compo_type, "%u");
DEFINE_DE_ATTR_INT_RO(compo_instance_id, "%u");
DEFINE_DE_ATTR_BOOL_RO(persistent);
DEFINE_DE_ATTR_INT_RO(name, "%s");
DEFINE_DE_ATTR_INT_RO(tstamp_exp, "%d");

#define DEFINE_DE_ATTR_STATE_RW(_name, _is_enable)			\
static ssize_t _name##_store(struct device *dev,			\
			     struct device_attribute *attr,		\
			     const char *buf, size_t len)		\
{									\
	struct scmi_tlm_de_dev *tde = to_tlm_de_dev(dev);		\
	struct scmi_tlm_setup *tsp = tde->tsp;				\
	typeof(_is_enable) _is_ena = _is_enable;			\
	bool enabled;							\
	int ret;							\
									\
	ret = kstrtobool(buf, &enabled);				\
	if (ret)							\
		return ret;						\
									\
	ret = tsp->ops->state_set(tsp->ph, false, tde->de->id,		\
				  _is_ena ? &enabled : NULL,		\
				  !_is_ena ? &enabled : NULL);		\
	if (ret)							\
		return ret;						\
									\
	return len;							\
}									\
									\
static ssize_t _name##_show(struct device *dev,				\
			    struct device_attribute *attr, char *buf)	\
{									\
	struct scmi_tlm_de_dev *tde = to_tlm_de_dev(dev);		\
									\
	return sysfs_emit(buf, "%c\n", tde->de->_name ## d ? 'Y' : 'N');\
}									\
static DEVICE_ATTR_RW(_name)

DEFINE_DE_ATTR_STATE_RW(enable, true);
DEFINE_DE_ATTR_STATE_RW(tstamp_enable, false);

static struct attribute *scmi_des_attrs[] = {
	&dev_attr_value.attr,
	&dev_attr_type.attr,
	&dev_attr_unit.attr,
	&dev_attr_unit_exp.attr,
	&dev_attr_instance_id.attr,
	&dev_attr_compo_type.attr,
	&dev_attr_compo_instance_id.attr,
	&dev_attr_persistent.attr,
	&dev_attr_enable.attr,
	NULL,
};
ATTRIBUTE_GROUPS(scmi_des);

static void scmi_tlm_dev_release(struct device *dev)
{
}

static int
scmi_telemetry_dev_register(struct device *dev, struct device *parent,
			    const char *name)
{
	int ret;

	dev->parent = parent;
	dev->release = scmi_tlm_dev_release;
	dev_set_name(dev, "%s", name);
	device_set_pm_not_required(dev);
	dev_set_uevent_suppress(dev, true);
	ret = device_register(dev);
	if (ret)
		put_device(dev);

	return ret;
}

static int
scmi_telemetry_cdev_register(struct device *dev, struct device *parent,
			     struct cdev *cdev, const struct file_operations *fops,
			     const char *name, unsigned int minor)
{
	int ret;

	dev->parent = parent;
	dev->release = scmi_tlm_dev_release;
	dev_set_name(dev, "%s", name);
	device_set_pm_not_required(dev);
	dev_set_uevent_suppress(dev, true);

	device_initialize(dev);

	dev->devt = MKDEV(scmi_tlm_major, minor);
	cdev_init(cdev, fops);

	ret = cdev_device_add(cdev, dev);
	if (ret)
		put_device(dev);

	return ret;
}

static int scmi_des_iter(struct device *dev, void *data)
{
	device_unregister(dev);

	return 0;
}

static void scmi_telemetry_dev_unregister(struct device *parent)
{
	device_for_each_child(parent, NULL, scmi_des_iter);
	device_unregister(parent);
}

static ssize_t grp_obj_store(struct device *dev,
			     struct device_attribute *attr,
			     const char *buf, size_t len)
{
	struct scmi_tlm_grp_dev *gde = to_tlm_grp_dev(dev);
	struct scmi_tlm_setup *tsp = gde->tsp;
	bool enabled, is_ena_entry;
	int ret;

	ret = kstrtobool(buf, &enabled);
	if (ret)
		return ret;

	is_ena_entry = !strncmp(attr->attr.name, "enable", 6);
	ret = tsp->ops->state_set(tsp->ph, true, gde->grp->id,
				  is_ena_entry ? &enabled : NULL,
				  !is_ena_entry ? &enabled : NULL);
	if (ret)
		return ret;

	return len;
}

static ssize_t grp_obj_show(struct device *dev,
			    struct device_attribute *attr, char *buf)
{
	struct scmi_tlm_grp_dev *gde = to_tlm_grp_dev(dev);
	bool enabled, is_ena_entry;

	is_ena_entry = !strncmp(attr->attr.name, "enable", 6);
	enabled = is_ena_entry ? gde->grp->enabled : gde->grp->tstamp_enabled;

	return sysfs_emit(buf, "%c\n", enabled ? 'Y' : 'N');
}

static struct device_attribute dev_attr_grp_enable = {
	.attr	= { .name = "enable", .mode = 0600 },
	.show	= grp_obj_show,
	.store	= grp_obj_store,
};

static struct device_attribute dev_attr_grp_tstamp_enable = {
	.attr	= { .name = "tstamp_enable", .mode = 0600 },
	.show	= grp_obj_show,
	.store	= grp_obj_store,
};

static ssize_t composing_des_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	struct scmi_tlm_grp_dev *gde = to_tlm_grp_dev(dev);

	return sysfs_emit(buf, "%s\n", gde->grp->des_str);
}
static DEVICE_ATTR_RO(composing_des);

static ssize_t grp_current_update_show(struct device *dev,
				       struct device_attribute *attr, char *buf)
{
	struct scmi_tlm_grp_dev *gde = to_tlm_grp_dev(dev);

	return __current_update_show(buf,
				     gde->grp->intervals.active_update_interval);
}

static ssize_t grp_current_update_store(struct device *dev,
					struct device_attribute *attr,
					const char *buf, size_t len)
{
	struct scmi_tlm_grp_dev *gde = to_tlm_grp_dev(dev);
	struct scmi_tlm_setup *tsp = gde->tsp;

	return __current_update_store(tsp, buf, len, gde->grp->id);
}

static struct device_attribute dev_attr_grp_current_update = {
	.attr	= { .name = "current_update_interval_ms", .mode = 0600 },
	.show	= grp_current_update_show,
	.store	= grp_current_update_store,
};

static ssize_t grp_intervals_discrete_show(struct device *dev,
					   struct device_attribute *attr,
					   char *buf)
{
	struct scmi_tlm_grp_dev *gde = to_tlm_grp_dev(dev);

	return __intervals_discrete_show(buf, gde->grp->intervals.discrete);
}

static struct device_attribute dev_attr_grp_intervals_discrete = {
	.attr	= { .name = "intervals_discrete", .mode = 0400 },
	.show	= grp_intervals_discrete_show,
};

static ssize_t grp_available_intervals_show(struct device *dev,
					    struct device_attribute *attr,
					    char *buf)
{
	struct scmi_tlm_grp_dev *gde = to_tlm_grp_dev(dev);

	return __available_update_show(buf, &gde->grp->intervals);
}

static struct device_attribute dev_attr_grp_available_intervals = {
	.attr	= { .name = "available_update_intervals_ms", .mode = 0400 },
	.show	= grp_available_intervals_show,
};

static ssize_t grp_des_bulk_read_show(struct device *dev,
				      struct device_attribute *attr, char *buf)
{
	struct scmi_tlm_grp_dev *gde = to_tlm_grp_dev(dev);
	struct scmi_tlm_instance *ti =
		groups_dev_to_tlm_instance(gde->dev.parent);

	return __des_bulk_read_show(ti, gde->grp->id, buf, PAGE_SIZE);
}

static ssize_t grp_des_single_sample_read_show(struct device *dev,
					       struct device_attribute *attr,
					       char *buf)
{
	struct scmi_tlm_grp_dev *gde = to_tlm_grp_dev(dev);
	struct scmi_tlm_instance *ti =
		groups_dev_to_tlm_instance(gde->dev.parent);

	return __des_single_sample_read_show(ti, gde->grp->id, buf, PAGE_SIZE);
}

static struct device_attribute dev_attr_grp_des_bulk_read = {
	.attr	= { .name = "des_bulk_read", .mode = 0400 },
	.show	= grp_des_bulk_read_show,
};

static struct device_attribute dev_attr_grp_des_single_sample_read = {
	.attr	= { .name = "des_single_sample_read", .mode = 0400 },
	.show	= grp_des_single_sample_read_show,
};

static struct attribute *scmi_grp_attrs[] = {
	&dev_attr_grp_enable.attr,
	&dev_attr_grp_tstamp_enable.attr,
	&dev_attr_grp_des_bulk_read.attr,
	&dev_attr_grp_des_single_sample_read.attr,
	&dev_attr_composing_des.attr,
	NULL,
};
ATTRIBUTE_GROUPS(scmi_grp);

static int scmi_telemetry_groups_initialize(struct device *dev,
					    struct scmi_tlm_instance *ti)
{
	int ret;

	if (ti->info->num_groups == 0)
		return 0;

	ret = scmi_telemetry_dev_register(&ti->groups_dev, &ti->dev, "groups");
	if (ret)
		return ret;

	for (int i = 0; i < ti->info->num_groups; i++) {
		const struct scmi_telemetry_group *grp = &ti->info->des_groups[i];
		struct scmi_tlm_grp_dev *gdev;
		char name[16];

		gdev = devm_kzalloc(dev, sizeof(*gdev), GFP_KERNEL);
		if (!gdev) {
			ret = -ENOMEM;
			goto err;
		}

		gdev->tsp = ti->tsp;
		gdev->grp = grp;
		gdev->dev.groups = scmi_grp_groups;

		snprintf(name, 8, "%d", grp->id);
		ret = scmi_telemetry_dev_register(&gdev->dev,
						  &ti->groups_dev, name);
		if (ret)
			goto err;

		if (ti->info->per_group_config_support) {
			sysfs_add_file_to_group(&gdev->dev.kobj,
						&dev_attr_grp_current_update.attr,
						NULL);
			sysfs_add_file_to_group(&gdev->dev.kobj,
						&dev_attr_grp_intervals_discrete.attr,
						NULL);
			sysfs_add_file_to_group(&gdev->dev.kobj,
						&dev_attr_grp_available_intervals.attr,
						NULL);
		}
	}

	dev_info(dev, "Found %d Telemetry GROUPS resources.\n",
		 ti->info->num_groups);

	return 0;

err:
	scmi_telemetry_dev_unregister(&ti->groups_dev);

	return ret;
}

static int scmi_telemetry_des_initialize(struct device *dev,
					 struct scmi_tlm_instance *ti)
{
	int ret;

	ret = scmi_telemetry_dev_register(&ti->des_dev, &ti->dev, "des");
	if (ret)
		return ret;

	for (int i = 0; i < ti->info->num_de; i++) {
		const struct scmi_telemetry_de *de = ti->info->des[i];
		struct scmi_tlm_de_dev *tdev;
		char name[16];

		tdev = devm_kzalloc(dev, sizeof(*tdev), GFP_KERNEL);
		if (!tdev) {
			ret = -ENOMEM;
			goto err;
		}

		tdev->tsp = ti->tsp;
		tdev->de = de;
		tdev->dev.groups = scmi_des_groups;

		/*XXX What about of ID/name digits-length used ? */
		snprintf(name, 8, "0x%04X", de->id);
		ret = scmi_telemetry_dev_register(&tdev->dev,
						  &ti->des_dev, name);
		if (ret)
			goto err;

		if (de->name)
			sysfs_add_file_to_group(&tdev->dev.kobj,
						&dev_attr_name.attr, NULL);
		if (de->tstamp_support) {
			sysfs_add_file_to_group(&tdev->dev.kobj,
						&dev_attr_tstamp_exp.attr,
						NULL);
			sysfs_add_file_to_group(&tdev->dev.kobj,
						&dev_attr_tstamp_enable.attr,
						NULL);
		}
	}

	dev_info(dev, "Found %d Telemetry DE resources.\n",
		 ti->info->num_de);

	return 0;

err:
	scmi_telemetry_dev_unregister(&ti->des_dev);

	return ret;
}

struct scmi_tlm_priv {
	char *buf;
	size_t buf_sz;
	int buf_len;
	struct scmi_tlm_instance *ti;
};

static int scmi_tlm_open(struct inode *ino, struct file *filp)
{
	struct scmi_tlm_instance *ti;
	struct scmi_tlm_priv *tp;

	tp = kzalloc(sizeof(*tp), GFP_KERNEL);
	if (!tp)
		return -ENOMEM;

	ti = container_of(ino->i_cdev, struct scmi_tlm_instance, cdev);
	tp->ti = ti;

	filp->private_data = tp;

	return 0;
}

static int scmi_tlm_bulk_buffer_allocate_and_fill(struct scmi_tlm_priv *tp)
{
	struct scmi_tlm_instance *ti = tp->ti;
	struct scmi_tlm_setup *tsp = ti->tsp;
	struct scmi_telemetry_de_sample *samples;
	int ret, num_samples;

	tp->buf_sz = ti->info->num_de * MAX_BULK_LINE_CHAR_LENGTH;
	tp->buf = kzalloc(tp->buf_sz, GFP_KERNEL);
	if (!tp->buf)
		return -ENOMEM;

	num_samples = ti->info->num_de;
	samples = kcalloc(num_samples, sizeof(*samples), GFP_KERNEL);
	if (!samples) {
		kfree(tp->buf);
		return -ENOMEM;
	}

	ret = tsp->ops->des_bulk_read(tsp->ph, SCMI_TLM_GRP_INVALID,
				      &num_samples, samples);
	if (ret) {
		kfree(samples);
		kfree(tp->buf);
		return ret;
	}

	ret = scmi_tlm_buffer_fill(&ti->dev, tp->buf, tp->buf_sz, &tp->buf_len,
				   num_samples, samples);
	kfree(samples);

	return ret;
}

static ssize_t scmi_tlm_read(struct file *filp, char __user *buf, size_t count,
			     loff_t *ppos)
{
	struct scmi_tlm_priv *tp = filp->private_data;
	int ret;

	if (!tp->buf) {
		ret = scmi_tlm_bulk_buffer_allocate_and_fill(tp);
		if (ret)
			return ret;
	}

	return simple_read_from_buffer(buf, count, ppos, tp->buf, tp->buf_len);
}

static __poll_t scmi_tlm_poll(struct file *, struct poll_table_struct *)
{
	return 0;
}

static long
scmi_tlm_info_get_ioctl(struct scmi_tlm_instance *ti, unsigned long arg)
{
	void * __user uptr = (void * __user)arg;

	if (copy_to_user(uptr, &ti->io_db.tlm_info,
			 sizeof(ti->io_db.tlm_info)))
		return -EFAULT;

	return 0;
}

static long
scmi_tlm_intervals_ioctl(struct scmi_tlm_instance *ti, unsigned long arg,
			 bool group)
{
	void * __user uptr = (void * __user)arg;
	struct scmi_tlm_intervals ivs, *tlm_ivs;

	if (copy_from_user(&ivs, uptr, sizeof(ivs)))
		return -EFAULT;

	if (!group) {
		tlm_ivs = ti->io_db.tlm_intervals;
	} else {
		if (ivs.grp_id >= ti->info->num_groups)
			return -EINVAL;

		tlm_ivs = ti->io_db.tlm_grp_intervals[ivs.grp_id];
	}

	if (ivs.num != tlm_ivs->num)
		return -EINVAL;

	if (copy_to_user(uptr, tlm_ivs,
			 sizeof(*tlm_ivs) + sizeof(u32) * ivs.num))
		return -EFAULT;

	return 0;
}

static long
scmi_tlm_de_config_set_ioctl(struct scmi_tlm_instance *ti, unsigned long arg,
			     bool all)
{
	void * __user uptr = (void * __user)arg;
	struct scmi_tlm_setup *tsp = ti->tsp;
	const struct scmi_telemetry_de *de;
	struct scmi_tlm_de_config tcfg = {};
	int ret;

	if (copy_from_user(&tcfg, uptr, sizeof(tcfg)))
		return -EFAULT;

	if (!all)
		return tsp->ops->state_set(tsp->ph, false, tcfg.id,
					   (bool *)&tcfg.enable,
					   (bool *)&tcfg.t_enable);

	for (int i = 0; i < ti->info->num_de; i++) {
		de = ti->info->des[i];

		ret = tsp->ops->state_set(tsp->ph, false, de->id,
					  (bool *)&tcfg.enable,
					  (bool *)&tcfg.t_enable);
		if (ret)
			return ret;
	}

	return 0;
}

static long
scmi_tlm_de_config_get_ioctl(struct scmi_tlm_instance *ti, unsigned long arg)
{
	void * __user uptr = (void * __user)arg;
	struct scmi_tlm_setup *tsp = ti->tsp;
	struct scmi_tlm_de_config tcfg = {};
	int ret;

	if (copy_from_user(&tcfg, uptr, sizeof(tcfg)))
		return -EFAULT;

	ret = tsp->ops->state_get(tsp->ph, tcfg.id,
				  (bool *)&tcfg.enable, (bool *)&tcfg.t_enable);
	if (ret)
		return ret;

	if (copy_to_user(uptr, &tcfg, sizeof(tcfg)))
		return -EFAULT;

	return 0;
}

static long
scmi_tlm_config_get_ioctl(struct scmi_tlm_instance *ti, unsigned long arg)
{
	void * __user uptr = (void * __user)arg;
	struct scmi_tlm_config cfg;

	cfg.enable = !!ti->info->enabled;
	cfg.current_update_interval =
		ti->info->intervals.active_update_interval;

	if (copy_to_user(uptr, &cfg, sizeof(cfg)))
		return -EFAULT;

	return 0;
}

static long
scmi_tlm_config_set_ioctl(struct scmi_tlm_instance *ti, unsigned long arg)
{
	void * __user uptr = (void * __user)arg;
	struct scmi_tlm_setup *tsp = ti->tsp;
	struct scmi_tlm_config cfg = {};

	if (copy_from_user(&cfg, uptr, sizeof(cfg)))
		return -EFAULT;

	return tsp->ops->collection_configure(tsp->ph, SCMI_TLM_GRP_INVALID,
					      true, (bool *)&cfg.enable,
					      &cfg.current_update_interval,
					      NULL);
}

static long
scmi_tlm_des_list_get_ioctl(struct scmi_tlm_instance *ti, unsigned long arg)
{
	void * __user uptr = (void * __user)arg;
	struct scmi_tlm_des_list dsl;

	if (copy_from_user(&dsl, uptr, sizeof(dsl)))
		return -EFAULT;

	if (dsl.num_des < ti->io_db.tlm_des_list->num_des)
		return -EFAULT;

	if (copy_to_user(uptr, ti->io_db.tlm_des_list,
			 sizeof(*ti->io_db.tlm_des_list) +
			 ti->io_db.tlm_des_list->num_des * sizeof(ti->io_db.tlm_des_list->des[0])))
		return -EFAULT;

	return 0;
}

static long
scmi_tlm_de_value_get_ioctl(struct scmi_tlm_instance *ti, unsigned long arg)
{
	void * __user uptr = (void * __user)arg;
	struct scmi_tlm_setup *tsp = ti->tsp;
	struct scmi_tlm_de_sample sample;
	int ret;

	if (copy_from_user(&sample, uptr, sizeof(sample)))
		return -EFAULT;

	ret = tsp->ops->de_data_read(tsp->ph,
				     (struct scmi_telemetry_de_sample *)&sample);
	if (ret)
		return ret;

	if (copy_to_user(uptr, &sample, sizeof(sample)))
		return -EFAULT;

	return 0;
}

static long scmi_tlm_des_read_ioctl(struct scmi_tlm_instance *ti,
				    unsigned long arg, bool single)
{
	void * __user uptr = (void * __user)arg;
	struct scmi_tlm_setup *tsp = ti->tsp;
	struct scmi_tlm_bulk_read bulk, *bulk_ptr;
	int ret;

	if (copy_from_user(&bulk, uptr, sizeof(bulk)))
		return -EFAULT;

	bulk_ptr = kzalloc(sizeof(*bulk_ptr) +
			     bulk.num_samples * sizeof(bulk_ptr->samples[0]),
			     GFP_KERNEL);
	if (!bulk_ptr)
		return -ENOMEM;

	bulk_ptr->grp_id = bulk.grp_id;
	bulk_ptr->num_samples = bulk.num_samples;
	if (!single)
		ret = tsp->ops->des_bulk_read(tsp->ph, bulk_ptr->grp_id,
					      &bulk_ptr->num_samples,
			  (struct scmi_telemetry_de_sample *)bulk_ptr->samples);
	else
		ret = tsp->ops->des_sample_get(tsp->ph, bulk_ptr->grp_id,
					       &bulk_ptr->num_samples,
			  (struct scmi_telemetry_de_sample *)bulk_ptr->samples);
	if (ret)
		goto out;

	if (copy_to_user(uptr, bulk_ptr, sizeof(*bulk_ptr) +
			 bulk_ptr->num_samples * sizeof(bulk_ptr->samples[0])))
		ret = -EFAULT;

out:
	kfree(bulk_ptr);

	return ret;
}

static long scmi_tlm_unlocked_ioctl(struct file *filp, unsigned int cmd,
				    unsigned long arg)
{
	struct scmi_tlm_priv *tp = filp->private_data;
	struct scmi_tlm_instance *ti = tp->ti;

	switch (cmd) {
	case SCMI_TLM_GET_INFO:
		return scmi_tlm_info_get_ioctl(ti, arg);
	case SCMI_TLM_GET_CFG:
		return scmi_tlm_config_get_ioctl(ti, arg);
	case SCMI_TLM_SET_CFG:
		return scmi_tlm_config_set_ioctl(ti, arg);
	case SCMI_TLM_GET_INTRVS:
		return scmi_tlm_intervals_ioctl(ti, arg, false);
	case SCMI_TLM_GET_DE_CFG:
		return scmi_tlm_de_config_get_ioctl(ti, arg);
	case SCMI_TLM_SET_DE_CFG:
		return scmi_tlm_de_config_set_ioctl(ti, arg, false);
	case SCMI_TLM_GET_DE_INFO:
		return -EOPNOTSUPP;
	case SCMI_TLM_GET_DE_LIST:
		return scmi_tlm_des_list_get_ioctl(ti, arg);
	case SCMI_TLM_GET_DE_VALUE:
		return scmi_tlm_de_value_get_ioctl(ti, arg);
	case SCMI_TLM_GET_GRP_CFG:
		return -EOPNOTSUPP;
	case SCMI_TLM_SET_GRP_CFG:
		return -EOPNOTSUPP;
	case SCMI_TLM_GET_GRP_INTRVS:
		return scmi_tlm_intervals_ioctl(ti, arg, true);
	case SCMI_TLM_GET_GRP_INFO:
		return -EOPNOTSUPP;
	case SCMI_TLM_GET_GRP_LIST:
		return -EOPNOTSUPP;
	case SCMI_TLM_SINGLE_SAMPLE:
		return scmi_tlm_des_read_ioctl(ti, arg, true);
	case SCMI_TLM_BULK_READ:
		return scmi_tlm_des_read_ioctl(ti, arg, false);
	case SCMI_TLM_SET_ALL_CFG:
		return scmi_tlm_de_config_set_ioctl(ti, arg, true);
	default:
		return -ENOTTY;
	}
}

static long scmi_tlm_compat_ioctl(struct file *, unsigned int, unsigned long)
{
	return 0;
}

static int scmi_tlm_mmap(struct file *, struct vm_area_struct *)
{
	return 0;
}

static int scmi_tlm_release(struct inode *ino, struct file *filp)
{
	struct scmi_tlm_priv *tp = filp->private_data;

	kfree(tp->buf);
	kfree(tp);

	return 0;
}

static const struct file_operations scmi_tlm_fops = {
	.owner = THIS_MODULE,
	.open = scmi_tlm_open,
	.read = scmi_tlm_read,
	.poll = scmi_tlm_poll,
	.unlocked_ioctl = scmi_tlm_unlocked_ioctl,
	.compat_ioctl = scmi_tlm_compat_ioctl,
	.mmap = scmi_tlm_mmap,
	.release = scmi_tlm_release,
};

static int scmi_tlm_setup_ioctl_data(struct device *dev,
				     struct scmi_tlm_instance *ti)
{
	ti->io_db.tlm_info.version = ti->info->version;
	for (int i = 0; i < SCMI_TLM_DE_IMPL_VERS; i++)
		ti->io_db.tlm_info.de_impl_version[i] = ti->info->de_impl_version[i];
	ti->io_db.tlm_info.num_des = ti->info->num_de;
	ti->io_db.tlm_info.num_groups = ti->info->num_groups;
	ti->io_db.tlm_info.num_intervals = ti->info->intervals.num;
	if (ti->info->reset_support)
		ti->io_db.tlm_info.flags = SCMI_TLM_CAN_RESET;

	ti->io_db.tlm_intervals = devm_kzalloc(dev, sizeof(*ti->io_db.tlm_intervals) +
					       ti->info->intervals.num * sizeof(__u32),
					       GFP_KERNEL);
	if (!ti->io_db.tlm_intervals)
		return -ENOMEM;

	ti->io_db.tlm_intervals->grp_id = 0;
	ti->io_db.tlm_intervals->discrete = ti->info->intervals.discrete;
	ti->io_db.tlm_intervals->num = ti->info->intervals.num;
	for (int i = 0; i < ti->info->intervals.num; i++)
		ti->io_db.tlm_intervals->available[i] =
			ti->info->intervals.update_intervals[i];

	ti->io_db.tlm_grp_intervals = devm_kcalloc(dev, ti->info->num_groups,
						   sizeof(ti->io_db.tlm_grp_intervals),
						   GFP_KERNEL);
	if (!ti->io_db.tlm_grp_intervals)
		return -ENOMEM;

	for (int i = 0; i < ti->info->num_groups; i++) {
		struct scmi_tlm_intervals *ivs;
		struct scmi_telemetry_group *grp = &ti->info->des_groups[i];

		ivs = devm_kzalloc(dev, sizeof(*ivs) +
				   grp->intervals.num * sizeof(__u32),
				   GFP_KERNEL);
		if (!ivs)
			return -ENOMEM;

		ivs->grp_id = i;
		ivs->discrete = grp->intervals.discrete;
		ivs->num = grp->intervals.num;
		for (int j = 0; j < ivs->num; j++)
			ivs->available[i] = grp->intervals.update_intervals[i];

		ti->io_db.tlm_grp_intervals[i] = ivs;
	}

	ti->io_db.tlm_des_list = devm_kzalloc(dev, sizeof(*ti->io_db.tlm_des_list) +
					ti->info->num_de * sizeof(ti->io_db.tlm_des_list->des[0]),
					GFP_KERNEL);
	if (!ti->io_db.tlm_des_list)
		return -ENOMEM;

	ti->io_db.tlm_des_list->num_des = ti->info->num_de;
	for (int i = 0; i < ti->info->num_de; i++) {
		ti->io_db.tlm_des_list->des[i].id = ti->info->des[i]->id;
		ti->io_db.tlm_des_list->des[i].grp_id =
			ti->info->des[i]->grp ? ti->info->des[i]->grp->id : SCMI_TLM_GRP_INVALID;
		ti->io_db.tlm_des_list->des[i].data_sz = ti->info->des[i]->data_sz;
		ti->io_db.tlm_des_list->des[i].type = ti->info->des[i]->type;
		ti->io_db.tlm_des_list->des[i].unit = ti->info->des[i]->unit;
		ti->io_db.tlm_des_list->des[i].unit_exp = ti->info->des[i]->unit_exp;
		ti->io_db.tlm_des_list->des[i].tstamp_exp = ti->info->des[i]->tstamp_exp;
		ti->io_db.tlm_des_list->des[i].instance_id = ti->info->des[i]->instance_id;
		ti->io_db.tlm_des_list->des[i].compo_instance_id =
			ti->info->des[i]->compo_instance_id;
		ti->io_db.tlm_des_list->des[i].compo_type = ti->info->des[i]->compo_type;
		ti->io_db.tlm_des_list->des[i].persistent = ti->info->des[i]->persistent;
		if (ti->info->des[i]->name)
			strscpy(ti->io_db.tlm_des_list->des[i].name, ti->info->des[i]->name,
				SCMI_SHORT_NAME_MAX_SIZE);
	}

	return 0;
}

static int
scmi_tlm_root_instance_initialize(struct device *dev,
				  struct scmi_tlm_instance *ti, int instance_id)
{
	char name[16];
	int ret;

	ret = scmi_tlm_setup_ioctl_data(dev, ti);
	if (ret)
		return ret;

	ti->dev.class = &scmi_telemetry_class;
	ti->dev.groups = scmi_telemetry_groups;

	snprintf(name, 16, "scmi_tlm_%d", instance_id);
	ret = scmi_telemetry_cdev_register(&ti->dev, NULL, &ti->cdev,
					   &scmi_tlm_fops, name, instance_id);
	if (ret)
		return ret;

	if (ti->info->reset_support)
		ret = sysfs_add_file_to_group(&ti->dev.kobj,
					      &dev_attr_reset.attr, NULL);

	return ret;
}

static struct scmi_tlm_instance *scmi_tlm_init(struct scmi_tlm_setup *tsp,
					       int instance_id)
{
	const struct scmi_telemetry_proto_ops *tlm_ops = tsp->ops;
	struct device *dev = &tsp->sdev->dev;
	struct scmi_tlm_instance *ti;
	int ret;

	ti = devm_kzalloc(dev, sizeof(*ti), GFP_KERNEL);
	if (!ti)
		return ERR_PTR(-ENOMEM);

	ti->info = tlm_ops->info_get(tsp->ph);
	if (!ti->info) {
		dev_err(dev, "invalid Telemetry info !\n");
		return ERR_PTR(-EINVAL);
	}

	ti->tsp = tsp;

	ret = scmi_tlm_root_instance_initialize(dev, ti, instance_id);
	if (ret)
		return ERR_PTR(ret);

	ret = scmi_telemetry_des_initialize(dev, ti);
	if (ret) {
		device_unregister(&ti->dev);
		return ERR_PTR(ret);
	}

	ret = scmi_telemetry_groups_initialize(dev, ti);
	if (ret) {
		scmi_telemetry_dev_unregister(&ti->des_dev);
		device_unregister(&ti->dev);
		return ERR_PTR(ret);
	}

	return ti;
}

static int scmi_telemetry_probe(struct scmi_device *sdev)
{
	const struct scmi_handle *handle = sdev->handle;
	struct scmi_protocol_handle *ph;
	struct device *dev = &sdev->dev;
	struct scmi_tlm_instance *ti;
	struct scmi_tlm_setup *tsp;
	const void *ops;

	if (!handle)
		return -ENODEV;

	ops = handle->devm_protocol_get(sdev, sdev->protocol_id, &ph);
	if (IS_ERR(ops))
		return dev_err_probe(dev, PTR_ERR(ops),
				     "Cannot access protocol:0x%X\n",
				     sdev->protocol_id);

	tsp = devm_kzalloc(&sdev->dev, sizeof(*tsp), GFP_KERNEL);
	if (!tsp)
		return -ENOMEM;

	tsp->sdev = sdev;
	tsp->ops = ops;
	tsp->ph = ph;

	//TODO Better to get info->id from SCMI/core
	ti = scmi_tlm_init(tsp, atomic_fetch_inc(&scmi_tlm_instance_count));
	if (IS_ERR(ti))
		return PTR_ERR(ti);

	dev_set_drvdata(&sdev->dev, ti);

	return 0;
}

static void scmi_telemetry_remove(struct scmi_device *sdev)
{
	struct device *dev = &sdev->dev;
	struct scmi_tlm_instance *ti;
	bool enabled = false;
	int ret;

	ti = dev_get_drvdata(&sdev->dev);

	ret = ti->tsp->ops->collection_configure(ti->tsp->ph,
						 SCMI_TLM_GRP_INVALID, true,
						 &enabled, NULL, NULL);
	if (ret)
		dev_warn(dev, "Failed to stop Telemetry collection\n");

	scmi_telemetry_dev_unregister(&ti->groups_dev);
	scmi_telemetry_dev_unregister(&ti->des_dev);
	device_unregister(&ti->dev);
}

static const struct scmi_device_id scmi_id_table[] = {
	{ SCMI_PROTOCOL_TELEMETRY, "telemetry" },
	{ },
};
MODULE_DEVICE_TABLE(scmi, scmi_id_table);

static struct scmi_driver scmi_telemetry_driver = {
	.name = "scmi-telemetry-driver",
	.probe = scmi_telemetry_probe,
	.remove = scmi_telemetry_remove,
	.id_table = scmi_id_table,
};

static int __init scmi_telemetry_init(void)
{
	dev_t devt;
	int ret;

	ret = alloc_chrdev_region(&devt, 0, 1024, "scmi-tlm");
	if (ret)
		return ret;

	scmi_tlm_major = MAJOR(devt);

	ret = class_register(&scmi_telemetry_class);
	if (ret)
		return ret;

	ret = scmi_register(&scmi_telemetry_driver);
	if (ret)
		class_unregister(&scmi_telemetry_class);

	return ret;
}
module_init(scmi_telemetry_init);

static void __exit scmi_telemetry_exit(void)
{
	scmi_unregister(&scmi_telemetry_driver);

	class_unregister(&scmi_telemetry_class);
}
module_exit(scmi_telemetry_exit);

MODULE_AUTHOR("Cristian Marussi <cristian.marussi@arm.com>");
MODULE_DESCRIPTION("ARM SCMI Telemetry Driver");
MODULE_LICENSE("GPL v2");
