// SPDX-License-Identifier: GPL-2.0-only
/*
 * AMD Dynamic Power and Thermal Configuration Interface (DPTCi) driver
 *
 * Exposes AMD APU power and thermal parameters via the firmware-attributes
 * sysfs ABI. Parameters are staged and atomically committed through the
 * AGESA ALIB Function 0x0C (Dynamic Power and Thermal Configuration
 * interface).
 *
 * Reference: AMD AGESA Publication #44065, Appendix E.5
 *            https://docs.amd.com/v/u/en-US/44065_Arch2008
 *
 * Copyright (C) 2026 Antheas Kapenekakis <lkml@antheas.dev>
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/acpi.h>
#include <linux/cleanup.h>
#include <linux/dmi.h>
#include <linux/init.h>
#include <linux/kobject.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/platform_profile.h>
#include <linux/processor.h>
#include <linux/sysfs.h>
#include <linux/unaligned.h>

#include "../firmware_attributes_class.h"

#define DRIVER_NAME		"amd-dptc"

#define ALIB_FUNC_DPTC		0x0C
#define ALIB_PATH		"\\_SB.ALIB"

#define ALIB_ID_TEMP_TARGET	0x03
#define ALIB_ID_STAPM_LIMIT	0x05
#define ALIB_ID_FAST_LIMIT	0x06
#define ALIB_ID_SLOW_LIMIT	0x07
#define ALIB_ID_SKIN_LIMIT	0x2E

enum dptc_param_idx {
	DPTC_PPT_PL1_SPL,	/* STAPM + skin limit (set together) */
	DPTC_PPT_PL2_SPPT,	/* slow PPT limit */
	DPTC_PPT_PL3_FPPT,	/* fast PPT limit */
	DPTC_CPU_TEMP,		/* thermal control target */
	DPTC_NUM_PARAMS,
};

struct dptc_param_limits {
	u32 expanded_min;
	u32 device_min;
	u32 def;
	u32 device_max;
	u32 expanded_max;
};

struct dptc_profile {
	u32 vals[DPTC_NUM_PARAMS];	/* 0 = don't set / unstage this param */
};

struct dptc_device_limits {
	struct dptc_param_limits params[DPTC_NUM_PARAMS];
	struct dptc_profile profiles[PLATFORM_PROFILE_LAST];
};

struct dptc_param_desc {
	const char *name;
	const char *display_name;
	u16 scale;	/* sysfs-to-ALIB multiplier (e.g. 1000 for W->mW) */
	u8 param_id;
	u8 param_id2;	/* secondary ALIB ID, 0 if none */
};

static const struct dptc_param_desc dptc_params[DPTC_NUM_PARAMS] = {
	[DPTC_PPT_PL1_SPL]  = { "ppt_pl1_spl", "Sustained power limit (W)",
				 1000, ALIB_ID_STAPM_LIMIT,
				 ALIB_ID_SKIN_LIMIT },
	[DPTC_PPT_PL2_SPPT] = { "ppt_pl2_sppt", "Slow PPT limit (W)",
				 1000, ALIB_ID_SLOW_LIMIT },
	[DPTC_PPT_PL3_FPPT] = { "ppt_pl3_fppt", "Fast PPT limit (W)",
				 1000, ALIB_ID_FAST_LIMIT },
	[DPTC_CPU_TEMP]     = { "cpu_temp", "Thermal control limit (C)",
				 1, ALIB_ID_TEMP_TARGET },
};

/* AI MAX Handheld class: GPD Win 5 */
static const struct dptc_device_limits limits_maxhh = {
	.params = {
		[DPTC_PPT_PL1_SPL]  = {  1,  4, 25,  80, 100 },
		[DPTC_PPT_PL2_SPPT] = {  1,  4, 27,  82, 100 },
		[DPTC_PPT_PL3_FPPT] = {  1,  4, 40,  85, 100 },
		[DPTC_CPU_TEMP]     = { 60, 70, 95,  95, 100 },
	},
	.profiles = {
		[PLATFORM_PROFILE_LOW_POWER]   = { .vals = { 15, 15, 25, 0 } },
		[PLATFORM_PROFILE_BALANCED]    = { .vals = { 25, 27, 40, 0 } },
		[PLATFORM_PROFILE_PERFORMANCE] = { .vals = { 60, 63, 85, 0 } },
	},
};

/* Substring matches against boot_cpu_data.x86_model_id; order matters. */
static const char * const dptc_soc_table[] = {
	/* AI MAX */
	"AMD RYZEN AI MAX+ 395",
	"AMD RYZEN AI MAX+ 385",
	"AMD RYZEN AI MAX 380",
	NULL,
};

static const struct dmi_system_id dptc_dmi_table[] = {
	/* GPD */
	{
		.ident = "GPD Win 5",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "GPD"),
			DMI_MATCH(DMI_PRODUCT_NAME, "G1618-05"),
		},
		.driver_data = (void *)&limits_maxhh,
	},
	{ }
};
MODULE_DEVICE_TABLE(dmi, dptc_dmi_table);

enum dptc_save_mode { SAVE_SINGLE, SAVE_BULK };

struct dptc_priv;

struct dptc_attr_sysfs {
	struct dptc_priv *priv;
	struct kobj_attribute current_value;
	struct kobj_attribute default_value;
	struct kobj_attribute min_value;
	struct kobj_attribute max_value;
	struct kobj_attribute scalar_increment;
	struct kobj_attribute display_name;
	struct kobj_attribute type;
	struct attribute *attrs[8];
	struct attribute_group group;
	int idx;
};

struct dptc_priv {
	struct device *fw_attr_dev;
	struct kset *fw_attr_kset;

	const struct dptc_device_limits *dev_limits;

	bool expanded;

	enum platform_profile_option profile;
	struct device *ppdev;

	enum dptc_save_mode save_mode;

	u32 staged[DPTC_NUM_PARAMS];

	/* Protects staged, expanded, save_mode, and profile */
	struct mutex lock;

	struct dptc_attr_sysfs params[DPTC_NUM_PARAMS];
	struct dptc_attr_sysfs expanded_attr;
	struct kobj_attribute save_settings_attr;
};

static struct platform_device *dptc_pdev;

static u32 dptc_get_min(struct dptc_priv *dptc, int idx)
{
	return dptc->expanded ? dptc->dev_limits->params[idx].expanded_min
			      : dptc->dev_limits->params[idx].device_min;
}

static u32 dptc_get_max(struct dptc_priv *dptc, int idx)
{
	return dptc->expanded ? dptc->dev_limits->params[idx].expanded_max
			      : dptc->dev_limits->params[idx].device_max;
}

static u32 dptc_get_default(struct dptc_priv *dptc, int idx)
{
	return dptc->dev_limits->params[idx].def;
}

static int dptc_alib_call(const u8 *ids, const u32 *vals, int count)
{
	union acpi_object in_params[2];
	struct acpi_object_list input;
	acpi_status status;
	u32 buf_size;
	int i, off;
	u8 *buf;

	if (count == 0)
		return -ENOENT;

	/* Buffer layout: WORD total_size + count * (BYTE id + DWORD value) */
	buf_size = 2 + count * 5;
	buf = kzalloc(buf_size, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	put_unaligned_le16(buf_size, buf);

	for (i = 0; i < count; i++) {
		off  = 2 + i * 5;
		buf[off] = ids[i];
		put_unaligned_le32(vals[i], &buf[off + 1]);
	}

	in_params[0].type           = ACPI_TYPE_INTEGER;
	in_params[0].integer.value  = ALIB_FUNC_DPTC;
	in_params[1].type           = ACPI_TYPE_BUFFER;
	in_params[1].buffer.length  = buf_size;
	in_params[1].buffer.pointer = buf;

	input.count   = 2;
	input.pointer = in_params;

	status = acpi_evaluate_object(NULL, ALIB_PATH, &input, NULL);
	kfree(buf);

	if (ACPI_FAILURE(status)) {
		pr_err("ALIB call failed: %s\n",
		       acpi_format_exception(status));
		return -EIO;
	}

	pr_debug("sent %d ALIB parameter(s)\n", count);
	return 0;
}

static int dptc_alib_fill_param(u8 *ids, u32 *vals, int offset,
				enum dptc_param_idx param, u32 val)
{
	u32 hw_val = val * dptc_params[param].scale;

	ids[offset]    = dptc_params[param].param_id;
	vals[offset++] = hw_val;

	if (dptc_params[param].param_id2) {
		ids[offset]    = dptc_params[param].param_id2;
		vals[offset++] = hw_val;
	}

	return offset;
}

static int dptc_alib_send_one(int idx, u32 val)
{
	u32 vals[2];
	u8 ids[2];

	return dptc_alib_call(ids, vals,
			      dptc_alib_fill_param(ids, vals, 0, idx, val));
}

static int dptc_alib_save(struct dptc_priv *dptc)
{
	u32 vals[DPTC_NUM_PARAMS * 2];
	u8 ids[DPTC_NUM_PARAMS * 2];
	int count = 0;
	int i;

	for (i = 0; i < DPTC_NUM_PARAMS; i++) {
		if (!dptc->staged[i])
			continue;
		count = dptc_alib_fill_param(ids, vals, count, i,
					     dptc->staged[i]);
	}

	return dptc_alib_call(ids, vals, count);
}

/* Sysfs callbacks */

static ssize_t dptc_current_value_show(struct kobject *kobj,
				       struct kobj_attribute *attr, char *buf)
{
	struct dptc_attr_sysfs *ps =
		container_of(attr, struct dptc_attr_sysfs, current_value);
	struct dptc_priv *dptc = ps->priv;

	guard(mutex)(&dptc->lock);

	if (dptc->profile != PLATFORM_PROFILE_CUSTOM) {
		u32 val = dptc->dev_limits->profiles[dptc->profile].vals[ps->idx];

		if (!val)
			return sysfs_emit(buf, "\n");
		return sysfs_emit(buf, "%u\n", val);
	}

	if (!dptc->staged[ps->idx])
		return sysfs_emit(buf, "\n");
	return sysfs_emit(buf, "%u\n", dptc->staged[ps->idx]);
}

static ssize_t dptc_current_value_store(struct kobject *kobj,
					struct kobj_attribute *attr,
					const char *buf, size_t count)
{
	struct dptc_attr_sysfs *ps =
		container_of(attr, struct dptc_attr_sysfs, current_value);
	struct dptc_priv *dptc = ps->priv;
	u32 val, min, max;
	int ret;

	guard(mutex)(&dptc->lock);

	if (dptc->profile != PLATFORM_PROFILE_CUSTOM)
		return -EBUSY;

	if (count == 1 && buf[0] == '\n') {
		dptc->staged[ps->idx] = 0;
		return count;
	}

	ret = kstrtou32(buf, 10, &val);
	if (ret)
		return ret;

	min = dptc_get_min(dptc, ps->idx);
	max = dptc_get_max(dptc, ps->idx);
	if (val < min || (max && val > max))
		return -EINVAL;
	dptc->staged[ps->idx]    = val;
	if (dptc->save_mode == SAVE_SINGLE)
		ret = dptc_alib_send_one(ps->idx, val);

	return ret ? ret : count;
}

static ssize_t dptc_default_value_show(struct kobject *kobj,
				       struct kobj_attribute *attr, char *buf)
{
	struct dptc_attr_sysfs *ps =
		container_of(attr, struct dptc_attr_sysfs, default_value);

	return sysfs_emit(buf, "%u\n", dptc_get_default(ps->priv, ps->idx));
}

static ssize_t dptc_min_value_show(struct kobject *kobj,
				   struct kobj_attribute *attr, char *buf)
{
	struct dptc_attr_sysfs *ps =
		container_of(attr, struct dptc_attr_sysfs, min_value);
	struct dptc_priv *dptc = ps->priv;

	guard(mutex)(&dptc->lock);

	return sysfs_emit(buf, "%u\n", dptc_get_min(dptc, ps->idx));
}

static ssize_t dptc_max_value_show(struct kobject *kobj,
				   struct kobj_attribute *attr, char *buf)
{
	struct dptc_attr_sysfs *ps =
		container_of(attr, struct dptc_attr_sysfs, max_value);
	struct dptc_priv *dptc = ps->priv;

	guard(mutex)(&dptc->lock);

	return sysfs_emit(buf, "%u\n", dptc_get_max(dptc, ps->idx));
}

static ssize_t dptc_scalar_increment_show(struct kobject *kobj,
					  struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "1\n");
}

static ssize_t dptc_display_name_show(struct kobject *kobj,
				      struct kobj_attribute *attr, char *buf)
{
	struct dptc_attr_sysfs *ps =
		container_of(attr, struct dptc_attr_sysfs, display_name);
	return sysfs_emit(buf, "%s\n", dptc_params[ps->idx].display_name);
}

static ssize_t dptc_type_show(struct kobject *kobj,
			      struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "integer\n");
}

static ssize_t dptc_save_settings_show(struct kobject *kobj,
				       struct kobj_attribute *attr, char *buf)
{
	struct dptc_priv *dptc =
		container_of(attr, struct dptc_priv, save_settings_attr);

	guard(mutex)(&dptc->lock);

	if (dptc->save_mode == SAVE_SINGLE)
		return sysfs_emit(buf, "single\n");
	return sysfs_emit(buf, "bulk\n");
}

static ssize_t dptc_save_settings_store(struct kobject *kobj,
					struct kobj_attribute *attr,
					const char *buf, size_t count)
{
	struct dptc_priv *dptc =
		container_of(attr, struct dptc_priv, save_settings_attr);
	int ret = 0;

	guard(mutex)(&dptc->lock);

	if (sysfs_streq(buf, "save"))
		ret = dptc_alib_save(dptc);
	else if (sysfs_streq(buf, "single"))
		dptc->save_mode = SAVE_SINGLE;
	else if (sysfs_streq(buf, "bulk"))
		dptc->save_mode = SAVE_BULK;
	else
		return -EINVAL;

	return ret ? ret : count;
}

static ssize_t dptc_expanded_current_value_show(struct kobject *kobj,
						struct kobj_attribute *attr,
						char *buf)
{
	struct dptc_attr_sysfs *ps =
		container_of(attr, struct dptc_attr_sysfs, current_value);
	struct dptc_priv *dptc = ps->priv;

	guard(mutex)(&dptc->lock);

	return sysfs_emit(buf, "%d\n", dptc->expanded);
}

static ssize_t dptc_expanded_current_value_store(struct kobject *kobj,
						 struct kobj_attribute *attr,
						 const char *buf, size_t count)
{
	struct dptc_attr_sysfs *ps =
		container_of(attr, struct dptc_attr_sysfs, current_value);
	struct dptc_priv *dptc = ps->priv;
	bool val;
	int ret;

	ret = kstrtobool(buf, &val);
	if (ret)
		return ret;

	guard(mutex)(&dptc->lock);

	if (dptc->profile != PLATFORM_PROFILE_CUSTOM)
		return -EBUSY;

	dptc->expanded = val;
	/* Clear staged values: limits changed, old values may be out of range */
	memset(dptc->staged, 0, sizeof(dptc->staged));

	return count;
}

static ssize_t dptc_expanded_default_value_show(struct kobject *kobj,
						struct kobj_attribute *attr,
						char *buf)
{
	return sysfs_emit(buf, "0\n");
}

static ssize_t dptc_expanded_min_value_show(struct kobject *kobj,
					    struct kobj_attribute *attr,
					    char *buf)
{
	return sysfs_emit(buf, "0\n");
}

static ssize_t dptc_expanded_max_value_show(struct kobject *kobj,
					    struct kobj_attribute *attr,
					    char *buf)
{
	return sysfs_emit(buf, "1\n");
}

static ssize_t dptc_expanded_scalar_increment_show(struct kobject *kobj,
						   struct kobj_attribute *attr,
						   char *buf)
{
	return sysfs_emit(buf, "1\n");
}

static ssize_t dptc_expanded_display_name_show(struct kobject *kobj,
					       struct kobj_attribute *attr,
					       char *buf)
{
	return sysfs_emit(buf, "Expanded Limits\n");
}

static ssize_t dptc_expanded_type_show(struct kobject *kobj,
				       struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "integer\n");
}

/* Sysfs setup */

static void dptc_setup_param_sysfs(struct dptc_priv *dptc,
				   struct dptc_attr_sysfs *ps, int idx)
{
	ps->priv = dptc;
	ps->idx = idx;

	sysfs_attr_init(&ps->current_value.attr);
	ps->current_value.attr.name  = "current_value";
	ps->current_value.attr.mode  = 0644;
	ps->current_value.show       = dptc_current_value_show;
	ps->current_value.store      = dptc_current_value_store;

	sysfs_attr_init(&ps->default_value.attr);
	ps->default_value.attr.name  = "default_value";
	ps->default_value.attr.mode  = 0444;
	ps->default_value.show       = dptc_default_value_show;

	sysfs_attr_init(&ps->min_value.attr);
	ps->min_value.attr.name      = "min_value";
	ps->min_value.attr.mode      = 0444;
	ps->min_value.show           = dptc_min_value_show;

	sysfs_attr_init(&ps->max_value.attr);
	ps->max_value.attr.name      = "max_value";
	ps->max_value.attr.mode      = 0444;
	ps->max_value.show           = dptc_max_value_show;

	sysfs_attr_init(&ps->scalar_increment.attr);
	ps->scalar_increment.attr.name = "scalar_increment";
	ps->scalar_increment.attr.mode = 0444;
	ps->scalar_increment.show      = dptc_scalar_increment_show;

	sysfs_attr_init(&ps->display_name.attr);
	ps->display_name.attr.name   = "display_name";
	ps->display_name.attr.mode   = 0444;
	ps->display_name.show        = dptc_display_name_show;

	sysfs_attr_init(&ps->type.attr);
	ps->type.attr.name           = "type";
	ps->type.attr.mode           = 0444;
	ps->type.show                = dptc_type_show;

	ps->attrs[0] = &ps->current_value.attr;
	ps->attrs[1] = &ps->default_value.attr;
	ps->attrs[2] = &ps->min_value.attr;
	ps->attrs[3] = &ps->max_value.attr;
	ps->attrs[4] = &ps->scalar_increment.attr;
	ps->attrs[5] = &ps->display_name.attr;
	ps->attrs[6] = &ps->type.attr;
	ps->attrs[7] = NULL;

	ps->group.name  = dptc_params[idx].name;
	ps->group.attrs = ps->attrs;
}

static void dptc_setup_expanded_sysfs(struct dptc_priv *dptc,
				      struct dptc_attr_sysfs *ps)
{
	ps->priv = dptc;
	sysfs_attr_init(&ps->current_value.attr);
	ps->current_value.attr.name  = "current_value";
	ps->current_value.attr.mode  = 0644;
	ps->current_value.show       = dptc_expanded_current_value_show;
	ps->current_value.store      = dptc_expanded_current_value_store;

	sysfs_attr_init(&ps->default_value.attr);
	ps->default_value.attr.name  = "default_value";
	ps->default_value.attr.mode  = 0444;
	ps->default_value.show       = dptc_expanded_default_value_show;

	sysfs_attr_init(&ps->min_value.attr);
	ps->min_value.attr.name      = "min_value";
	ps->min_value.attr.mode      = 0444;
	ps->min_value.show           = dptc_expanded_min_value_show;

	sysfs_attr_init(&ps->max_value.attr);
	ps->max_value.attr.name      = "max_value";
	ps->max_value.attr.mode      = 0444;
	ps->max_value.show           = dptc_expanded_max_value_show;

	sysfs_attr_init(&ps->scalar_increment.attr);
	ps->scalar_increment.attr.name = "scalar_increment";
	ps->scalar_increment.attr.mode = 0444;
	ps->scalar_increment.show      = dptc_expanded_scalar_increment_show;

	sysfs_attr_init(&ps->display_name.attr);
	ps->display_name.attr.name   = "display_name";
	ps->display_name.attr.mode   = 0444;
	ps->display_name.show        = dptc_expanded_display_name_show;

	sysfs_attr_init(&ps->type.attr);
	ps->type.attr.name           = "type";
	ps->type.attr.mode           = 0444;
	ps->type.show                = dptc_expanded_type_show;

	ps->attrs[0] = &ps->current_value.attr;
	ps->attrs[1] = &ps->default_value.attr;
	ps->attrs[2] = &ps->min_value.attr;
	ps->attrs[3] = &ps->max_value.attr;
	ps->attrs[4] = &ps->scalar_increment.attr;
	ps->attrs[5] = &ps->display_name.attr;
	ps->attrs[6] = &ps->type.attr;
	ps->attrs[7] = NULL;

	ps->group.name  = "expanded_limits";
	ps->group.attrs = ps->attrs;
}

static void dptc_fw_dev_unregister(void *data)
{
	device_unregister(data);
}

static void dptc_kset_unregister(void *data)
{
	kset_unregister(data);
}

/* Platform profile */

static int dptc_apply_profile(struct dptc_priv *dptc,
			      enum platform_profile_option profile)
{
	const struct dptc_profile *pp;
	int i;

	memset(dptc->staged, 0, sizeof(dptc->staged));

	if (profile == PLATFORM_PROFILE_CUSTOM)
		return 0;

	pp = &dptc->dev_limits->profiles[profile];
	for (i = 0; i < DPTC_NUM_PARAMS; i++) {
		if (!pp->vals[i])
			continue;
		dptc->staged[i] = pp->vals[i];
	}

	return dptc_alib_save(dptc);
}

static int dptc_pp_probe(void *drvdata, unsigned long *choices)
{
	struct dptc_priv *dptc = drvdata;
	int i, j;

	set_bit(PLATFORM_PROFILE_CUSTOM, choices);
	for (i = 0; i < PLATFORM_PROFILE_LAST; i++) {
		for (j = 0; j < DPTC_NUM_PARAMS; j++) {
			if (dptc->dev_limits->profiles[i].vals[j]) {
				set_bit(i, choices);
				break;
			}
		}
	}
	return 0;
}

static int dptc_pp_get(struct device *dev,
		       enum platform_profile_option *profile)
{
	struct dptc_priv *dptc = dev_get_drvdata(dev);

	guard(mutex)(&dptc->lock);

	*profile = dptc->profile;
	return 0;
}

static int dptc_pp_set(struct device *dev,
		       enum platform_profile_option profile)
{
	struct dptc_priv *dptc = dev_get_drvdata(dev);

	guard(mutex)(&dptc->lock);

	dptc->profile = profile;

	return dptc_apply_profile(dptc, profile);
}

static const struct platform_profile_ops dptc_pp_ops = {
	.probe       = dptc_pp_probe,
	.profile_get = dptc_pp_get,
	.profile_set = dptc_pp_set,
};

static int dptc_resume(struct device *dev)
{
	struct dptc_priv *dptc = dev_get_drvdata(dev);
	int ret;

	guard(mutex)(&dptc->lock);

	/* In bulk mode, do not use pm ops for userspace flexibility. */
	if (dptc->profile != PLATFORM_PROFILE_CUSTOM)
		ret = dptc_apply_profile(dptc, dptc->profile);
	else if (dptc->save_mode == SAVE_SINGLE)
		ret = dptc_alib_save(dptc);
	else
		ret = 0;

	if (ret && ret != -ENOENT)
		dev_warn(dev, "failed to restore settings on resume: %d\n", ret);

	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(dptc_pm_ops, NULL, dptc_resume);

static int dptc_probe(struct platform_device *pdev)
{
	const struct dmi_system_id *dmi_match = dev_get_platdata(&pdev->dev);
	struct device *dev = &pdev->dev;
	struct dptc_priv *dptc;
	int i, ret;

	dptc = devm_kzalloc(dev, sizeof(*dptc), GFP_KERNEL);
	if (!dptc)
		return -ENOMEM;

	platform_set_drvdata(pdev, dptc);

	ret = devm_mutex_init(dev, &dptc->lock);
	if (ret)
		return ret;

	dptc->dev_limits = dmi_match->driver_data;
	dev_info(dev, "%s (%s)\n", dmi_match->ident,
		 boot_cpu_data.x86_model_id);

	dptc->fw_attr_dev = device_create(&firmware_attributes_class,
					  NULL, MKDEV(0, 0), NULL, DRIVER_NAME);
	if (IS_ERR(dptc->fw_attr_dev))
		return PTR_ERR(dptc->fw_attr_dev);

	ret = devm_add_action_or_reset(dev, dptc_fw_dev_unregister,
				       dptc->fw_attr_dev);
	if (ret)
		return ret;

	dptc->fw_attr_kset = kset_create_and_add("attributes", NULL,
						 &dptc->fw_attr_dev->kobj);
	if (!dptc->fw_attr_kset)
		return -ENOMEM;

	ret = devm_add_action_or_reset(dev, dptc_kset_unregister,
				       dptc->fw_attr_kset);
	if (ret)
		return ret;

	for (i = 0; i < DPTC_NUM_PARAMS; i++) {
		dptc_setup_param_sysfs(dptc, &dptc->params[i], i);
		ret = sysfs_create_group(&dptc->fw_attr_kset->kobj,
					 &dptc->params[i].group);
		if (ret)
			return ret;
	}

	dptc_setup_expanded_sysfs(dptc, &dptc->expanded_attr);
	ret = sysfs_create_group(&dptc->fw_attr_kset->kobj,
				 &dptc->expanded_attr.group);
	if (ret)
		return ret;

	sysfs_attr_init(&dptc->save_settings_attr.attr);
	dptc->save_settings_attr.attr.name  = "save_settings";
	dptc->save_settings_attr.attr.mode  = 0644;
	dptc->save_settings_attr.show       = dptc_save_settings_show;
	dptc->save_settings_attr.store      = dptc_save_settings_store;
	ret = sysfs_create_file(&dptc->fw_attr_kset->kobj,
				&dptc->save_settings_attr.attr);
	if (ret)
		return ret;

	dptc->profile = PLATFORM_PROFILE_CUSTOM;
	dptc->ppdev = devm_platform_profile_register(dev, "amd-dptc", dptc,
						     &dptc_pp_ops);
	if (IS_ERR(dptc->ppdev))
		return PTR_ERR(dptc->ppdev);

	return 0;
}

static struct platform_driver dptc_driver = {
	.driver = {
		.name = DRIVER_NAME,
		.pm   = pm_sleep_ptr(&dptc_pm_ops),
	},
	.probe  = dptc_probe,
};

static int __init dptc_init(void)
{
	const struct dmi_system_id *match;
	bool soc_found = false;
	int i, ret;

	match = dmi_first_match(dptc_dmi_table);
	if (!match)
		return -ENODEV;

	if (!acpi_has_method(NULL, ALIB_PATH)) {
		pr_warn("ALIB method not present\n");
		return -ENODEV;
	}

	for (i = 0; dptc_soc_table[i]; i++) {
		if (strstr(boot_cpu_data.x86_model_id,
			   dptc_soc_table[i])) {
			soc_found = true;
			break;
		}
	}
	if (!soc_found) {
		pr_warn("unrecognized SoC '%s'\n",
			boot_cpu_data.x86_model_id);
		return -ENODEV;
	}

	dptc_pdev = platform_device_register_data(NULL, DRIVER_NAME, -1,
						  match, sizeof(*match));
	if (IS_ERR(dptc_pdev))
		return PTR_ERR(dptc_pdev);

	ret = platform_driver_register(&dptc_driver);
	if (ret) {
		platform_device_unregister(dptc_pdev);
		return ret;
	}

	return 0;
}

static void __exit dptc_exit(void)
{
	platform_driver_unregister(&dptc_driver);
	platform_device_unregister(dptc_pdev);
}

module_init(dptc_init);
module_exit(dptc_exit);

MODULE_AUTHOR("Antheas Kapenekakis <lkml@antheas.dev>");
MODULE_DESCRIPTION("AMD DPTCi ACPI Driver");
MODULE_LICENSE("GPL");
