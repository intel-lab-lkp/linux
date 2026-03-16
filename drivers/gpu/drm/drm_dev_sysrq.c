// SPDX-License-Identifier: GPL-2.0 or MIT

#include <linux/sysrq.h>

#include <drm/drm_device.h>
#include <drm/drm_drv.h>
#include <drm/drm_print.h>

#include "drm_internal.h"

#ifdef CONFIG_MAGIC_SYSRQ
static LIST_HEAD(drm_dev_sysrq_dev_list);
static DEFINE_MUTEX(drm_dev_sysrq_dev_lock);

/* emergency restore, don't bother with error reporting */
static void drm_dev_sysrq_restore_work_fn(struct work_struct *ignored)
{
	struct drm_device *dev;

	guard(mutex)(&drm_dev_sysrq_dev_lock);

	list_for_each_entry(dev, &drm_dev_sysrq_dev_list, dev_sysrq_list) {
		dev->driver->sysrq_kill(dev);
	}
}

static DECLARE_WORK(drm_dev_sysrq_restore_work, drm_dev_sysrq_restore_work_fn);

static void drm_dev_sysrq_restore_handler(u8 ignored)
{
	schedule_work(&drm_dev_sysrq_restore_work);
}

static const struct sysrq_key_op drm_dev_sysrq_kill_op = {
	.handler = drm_dev_sysrq_restore_handler,
	.help_msg = "kill-gpu-job(G)",
	.action_msg = "Kill current job on the GPU",
};

void drm_dev_sysrq_register(struct drm_device *dev)
{
	const struct drm_driver *driver = dev->driver;

	if (!driver->sysrq_kill)
		return;

	guard(mutex)(&drm_dev_sysrq_dev_lock);

	if (list_empty(&drm_dev_sysrq_dev_list))
		register_sysrq_key('G', &drm_dev_sysrq_kill_op);

	list_add(&dev->dev_sysrq_list, &drm_dev_sysrq_dev_list);
}

void drm_dev_sysrq_unregister(struct drm_device *dev)
{
	guard(mutex)(&drm_dev_sysrq_dev_lock);

	/* remove device from global restore list */
	if (!drm_WARN_ON(dev, list_empty(&dev->dev_sysrq_list)))
		list_del(&dev->dev_sysrq_list);

	/* no devices left; unregister key */
	if (list_empty(&drm_dev_sysrq_dev_list))
		unregister_sysrq_key('G', &drm_dev_sysrq_kill_op);
}
#endif
