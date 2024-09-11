// SPDX-License-Identifier: GPL-2.0-only
/*
 * probe_control.c - Probe control driver
 *
 * Copyright (c) 2024 Sony Group Corporation
 */

#include <linux/init.h>
#include <linux/device.h>
#include <linux/slab.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/sysfs.h>
#include <linux/kobject.h>
#include <linux/debugfs.h>
#include <linux/module.h>
#include <linux/string.h>
#include <linux/seq_file.h>
#include <linux/device/driver.h>
#include <linux/platform_device.h>
#include <linux/mod_devicetable.h>

#include "base.h"

/*
 * Overview
 *
 * Probe control driver framework allows deferring the probes of a group of
 * devices to an arbitrary time, giving the user control to trigger the probes
 * after boot. This is useful for deferring probes from builtin drivers that
 * are not required during boot and probe when user wants after boot.
 *
 * This is achieved by adding a dummy device aka probe control device node
 * as provider to a group of devices(consumer nodes) in platform's device tree.
 * Consumers are the devices we want to probe after boot.
 * fw_devlink ensures consumers are not probed until provider is probed
 * successfully. The provider probe during boot returns -ENXIO and is not
 * re-probed again.
 *
 * The driver provides debug interface /sys/kernel/debug/probe_control_status
 * for checking probe control status of registered probe control devices.
 *  # cat /sys/kernel/debug/probe_control_status
 *  prb_ctrl_dev_0: [not triggered]
 *   Consumers: 1ffc000.pcie
 *
 * Interface /sys/kernel/probe_control/trigger is provided for triggering
 * probes of the probe control devices. User can write to this interface to
 * trigger specific or all device probes managed by this driver.
 * Once the probe is triggered by user, provider probe control device is added
 * to deferred_probe_pending_list and driver_deferred_probe_trigger() is
 * triggered. This time provider probe will return successfully and consumer
 * devices will then be probed.
 * To trigger specific provider probe:
 *   # echo prb_ctrl_dev_0 > /sys/kernel/probe_control/trigger
 *
 * To trigger all registered provider probes
 *   # echo all > /sys/kernel/probe_control/trigger
 *
 * For details on configuring probe control devices in platform device tree,
 * refer:
 *  Documentation/devicetree/bindings/probe-control/linux,probe-controller.yaml
 *
 */

#define MAX_PROBE_CTRL_DEVS 50

struct probe_ctrl_dev_data {
	struct device *dev;
	bool probe;
	struct list_head list;
};

static LIST_HEAD(probe_ctrl_dev_list);
static DEFINE_MUTEX(probe_ctrl_dev_list_mutex);
static atomic_t probes_pending = ATOMIC_INIT(0);
static struct kobject *probe_ctrl_kobj;

static int probe_ctrl_status_show(struct seq_file *s, void *v)
{
	struct probe_ctrl_dev_data *data;
	struct device_link *link;

	mutex_lock(&probe_ctrl_dev_list_mutex);
	list_for_each_entry(data, &probe_ctrl_dev_list, list) {
		seq_printf(s, "%s: [%s]\n", dev_name(data->dev),
			data->probe ? "triggered" : "not triggered");
		seq_puts(s, " Consumers:");
		if (list_empty(&data->dev->links.consumers)) {
			seq_puts(s, " None\n");
			continue;
		}
		list_for_each_entry(link, &data->dev->links.consumers, s_node) {
			seq_printf(s, " %s", dev_name(link->consumer));
		}
		seq_puts(s, "\n");
	}
	mutex_unlock(&probe_ctrl_dev_list_mutex);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(probe_ctrl_status);

static void enable_probe(struct probe_ctrl_dev_data *data)
{
	/*
	 * add the device to deferred_probe_pending_list
	 */
	driver_deferred_probe_add(data->dev);
	data->probe = true;
	atomic_dec(&probes_pending);
	dev_dbg(data->dev, "enabled probe\n");
}

static ssize_t probe_ctrl_trigger_store(struct kobject *kobj,
					struct kobj_attribute *attr,
					const char *buf, size_t count)
{
	struct probe_ctrl_dev_data *data;
	int probes_pending_l;
	int ret;

	probes_pending_l = atomic_read(&probes_pending);
	if (probes_pending_l == 0)
		return -EINVAL;

	mutex_lock(&probe_ctrl_dev_list_mutex);
	if (sysfs_streq(buf, "all")) {
		list_for_each_entry(data, &probe_ctrl_dev_list, list) {
			if (!data->probe)
				enable_probe(data);
		}
	} else {
		list_for_each_entry(data, &probe_ctrl_dev_list, list) {
			if (sysfs_streq(dev_name(data->dev), buf)) {
				if (!data->probe)
					enable_probe(data);
				break;
			}
		}
	}
	mutex_unlock(&probe_ctrl_dev_list_mutex);

	if (probes_pending_l == atomic_read(&probes_pending)) {
		ret = -EINVAL;
		goto out;
	}
	/*
	 * Re-probe deferred devices and
	 * wait for device probing to be completed
	 */
	driver_deferred_probe_trigger();
	wait_for_device_probe();
	ret = count;

out:
	return ret;
}

static struct kobj_attribute probe_ctrl_attribute =
	__ATTR(trigger, 0200, NULL, probe_ctrl_trigger_store);

static struct attribute *attrs[] = {
	&probe_ctrl_attribute.attr,
	NULL,
};

static struct attribute_group attr_group = {
	.attrs = attrs,
};

static int probe_control_dev_probe(struct platform_device *pdev)
{
	struct probe_ctrl_dev_data *data;
	int ret;

	if (!pdev->dev.of_node) {
		dev_err(&pdev->dev,
			"driver only supports devices from device tree\n");
		return -EINVAL;
	}

	mutex_lock(&probe_ctrl_dev_list_mutex);
	list_for_each_entry(data, &probe_ctrl_dev_list, list) {
		if (&pdev->dev == data->dev) {
			ret = data->probe ? 0 : -ENXIO;
			mutex_unlock(&probe_ctrl_dev_list_mutex);
			dev_dbg(data->dev, "probe return: %d\n", ret);
			return ret;
		}
	}
	mutex_unlock(&probe_ctrl_dev_list_mutex);

	if (atomic_read(&probes_pending) == MAX_PROBE_CTRL_DEVS) {
		dev_dbg(&pdev->dev,
			"Probe control device limit exceeded, probing now\n");
		return 0;
	}

	data = kzalloc(sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->dev = &pdev->dev;
	data->probe = false;

	mutex_lock(&probe_ctrl_dev_list_mutex);
	list_add_tail(&data->list, &probe_ctrl_dev_list);
	atomic_inc(&probes_pending);
	mutex_unlock(&probe_ctrl_dev_list_mutex);

	dev_dbg(data->dev, "Added dev to probe control list\n");

	return -ENXIO;
}

static const struct of_device_id probe_ctrl_of_match[] = {
	{ .compatible = "linux,probe-control" },
	{ },
};
MODULE_DEVICE_TABLE(of, probe_ctrl_of_match);

static struct platform_driver probe_control_driver = {
	.probe          = probe_control_dev_probe,
	.driver = {
		.name	= "probe-control",
		.of_match_table = probe_ctrl_of_match,
		.probe_type = PROBE_PREFER_ASYNCHRONOUS,
	},
};

static int __init probe_control_init(void)
{
	int ret;

	probe_ctrl_kobj = kobject_create_and_add("probe_control", kernel_kobj);
	if (!probe_ctrl_kobj)
		return -ENOMEM;

	ret = sysfs_create_group(probe_ctrl_kobj, &attr_group);
	if (ret)
		goto out_err;

	ret = platform_driver_register(&probe_control_driver);
	if (ret)
		goto out_err;

	debugfs_create_file("probe_control_status", 0444, NULL, NULL,
			    &probe_ctrl_status_fops);
	return ret;

out_err:
	kobject_put(probe_ctrl_kobj);
	return ret;
}

late_initcall(probe_control_init);

static void __exit probe_control_exit(void)
{
	struct probe_ctrl_dev_data *data, *tmp_data;

	kobject_put(probe_ctrl_kobj);
	debugfs_lookup_and_remove("probe_control_status", NULL);

	mutex_lock(&probe_ctrl_dev_list_mutex);
	list_for_each_entry_safe(data, tmp_data, &probe_ctrl_dev_list, list) {
		list_del(&data->list);
		kfree(data);
	}
	mutex_unlock(&probe_ctrl_dev_list_mutex);

	platform_driver_unregister(&probe_control_driver);
}

__exitcall(probe_control_exit);
