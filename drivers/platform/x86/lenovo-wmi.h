/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Lenovo Legion WMI interface driver. The Lenovo Legion WMI interface is
 * broken up into multiple GUID interfaces that require cross-references
 * between GUID's for some functionality. The "Custom Method" interface is a
 * legacy interface for managing and displaying CPU & GPU power and hwmon
 * settings and readings. The "Other Mode" interface is a modern interface
 * that replaces or extends the "Custom Method" interface methods. The
 * "Gamezone" interface adds advanced features such as fan profiles and
 * overclocking. The "Lighting" interface adds control of various status
 * lights related to different hardware components. "Other Mode" uses
 * the data structs LENOVO_CAPABILITY_DATA_00, LENOVO_CAPABILITY_DATA_01
 * and LENOVO_CAPABILITY_DATA_02 structs for capability information.
 *
 * Copyright(C) 2024 Derek J. Clark <derekjohn.clark@gmail.com>
 *
 */
#include <linux/notifier.h>
#include <linux/platform_profile.h>

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#ifndef _LENOVO_WMI_H_
#define _LENOVO_WMI_H_

#include <linux/bits.h>
#include <linux/types.h>
#include <linux/wmi.h>

struct wmi_method_args {
	u32 arg0;
	u32 arg1;
};

/* gamezone structs */
enum thermal_mode {
	SMARTFAN_MODE_QUIET = 0x01,
	SMARTFAN_MODE_BALANCED = 0x02,
	SMARTFAN_MODE_PERFORMANCE = 0x03,
	SMARTFAN_MODE_EXTREME = 0xE0, /* Ver 6+ */
	SMARTFAN_MODE_CUSTOM = 0xFF,
};

enum lenovo_wmi_action {
	THERMAL_MODE_EVENT = 1,
};

/* capdata01 structs */
struct lenovo_wmi_cd01 {
	struct capdata01 **capdata;
	struct wmi_device *wdev;
	int instance_count;
};

struct capdata01 {
	u32 id;
	u32 supported;
	u32 default_value;
	u32 step;
	u32 min_value;
	u32 max_value;
};

/* other method structs */
struct lenovo_wmi_om {
	struct component_master_ops *ops;
	struct lenovo_wmi_cd01 *cd01;
	struct capdata01 **capdata;
	struct device *fw_attr_dev;
	struct kset *fw_attr_kset;
	struct notifier_block nb;
	struct wmi_device *wdev;
	enum thermal_mode mode;
	int instance_count;
};

/* wmidev_evaluate_method helper functions */
int lenovo_wmidev_evaluate_method_2(struct wmi_device *wdev, u8 instance,
				    u32 method_id, u32 arg0, u32 arg1,
				    u32 *retval);
int lenovo_wmidev_evaluate_method_1(struct wmi_device *wdev, u8 instance,
				    u32 method_id, u32 arg0, u32 *retval);

/* lenovo_wmi_cd01_driver match function */
int lenovo_wmi_cd01_match(struct device *dev, void *data);

/* lenovo_wmi_gz_driver notifier functions */
int lenovo_wmi_gz_notifier_call(struct notifier_block *nb, unsigned long action,
				enum platform_profile_option *profile);
int lenovo_wmi_gz_register_notifier(struct notifier_block *nb);
int lenovo_wmi_gz_unregister_notifier(struct notifier_block *nb);
int devm_lenovo_wmi_gz_register_notifier(struct device *dev,
					 struct notifier_block *nb);
#endif /* !_LENOVO_WMI_H_ */
