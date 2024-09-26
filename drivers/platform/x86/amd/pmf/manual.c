// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * AMD Platform Management Framework Driver
 *
 * Copyright (c) 2024, Advanced Micro Devices, Inc.
 * All Rights Reserved.
 *
 * Author: Mario Limonciello <mario.limonciello@amd.com>
 */

#include "pmf.h"

#define pmf_manual_attribute(_name, _set_command, _get_command)		\
static ssize_t _name##_store(struct device *d,				\
			     struct device_attribute *attr,		\
			     const char *buf, size_t count)		\
{									\
	struct amd_pmf_dev *dev = dev_get_drvdata(d);			\
	uint val;							\
									\
	if (dev->current_profile != PLATFORM_PROFILE_CUSTOM) {		\
		dev_warn_once(dev->dev,					\
			      "Manual control is disabled, please set "	\
			      "platform profile to custom.\n");		\
		return -EINVAL;						\
	}								\
									\
	if (kstrtouint(buf, 10, &val) < 0)				\
		return -EINVAL;						\
									\
	amd_pmf_send_cmd(dev, _set_command, false, val, NULL);		\
									\
	return count;							\
}									\
static ssize_t _name##_show(struct device *d,				\
			   struct device_attribute *attr,		\
			   char *buf)					\
{									\
	struct amd_pmf_dev *dev = dev_get_drvdata(d);			\
	uint val;							\
									\
	amd_pmf_send_cmd(dev, _get_command, true, ARG_NONE, &val);	\
									\
	return sysfs_emit(buf, "%u\n", val);				\
}

pmf_manual_attribute(spl, SET_SPL, GET_SPL);
static DEVICE_ATTR_RW(spl);
pmf_manual_attribute(fppt, SET_FPPT, GET_FPPT);
static DEVICE_ATTR_RW(fppt);
pmf_manual_attribute(sppt, SET_SPPT, GET_SPPT);
static DEVICE_ATTR_RW(sppt);
pmf_manual_attribute(sppt_apu_only, SET_SPPT_APU_ONLY, GET_SPPT_APU_ONLY);
static DEVICE_ATTR_RW(sppt_apu_only);
pmf_manual_attribute(stt_min, SET_STT_MIN_LIMIT, GET_STT_MIN_LIMIT);
static DEVICE_ATTR_RW(stt_min);
pmf_manual_attribute(stt_limit_apu, SET_STT_LIMIT_APU, GET_STT_LIMIT_APU);
static DEVICE_ATTR_RW(stt_limit_apu);
pmf_manual_attribute(stt_skin_temp, SET_STT_LIMIT_HS2, GET_STT_LIMIT_HS2);
static DEVICE_ATTR_RW(stt_skin_temp);

static umode_t manual_attr_is_visible(struct kobject *kobj, struct attribute *attr, int idx)
{
	return pmf_manual_control ? 0660 : 0;
}

static struct attribute *manual_attrs[] = {
	&dev_attr_spl.attr,
	&dev_attr_fppt.attr,
	&dev_attr_sppt.attr,
	&dev_attr_sppt_apu_only.attr,
	&dev_attr_stt_min.attr,
	&dev_attr_stt_limit_apu.attr,
	&dev_attr_stt_skin_temp.attr,
	NULL,
};

const struct attribute_group manual_attribute_group = {
	.attrs = manual_attrs,
	.is_visible = manual_attr_is_visible,
};

void amd_pmf_init_manual_control(struct amd_pmf_dev *dev)
{
	add_taint(TAINT_CPU_OUT_OF_SPEC, LOCKDEP_STILL_OK);
	pr_crit("Manual PMF control is enabled, please disable it before "
		"reporting any bugs unrelated to PMF.\n");
}
