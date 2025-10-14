// SPDX-License-Identifier: GPL-2.0
/*
 * drivers/base/power/generic_ops.c - Generic PM callbacks for subsystems
 *
 * Copyright (c) 2010 Rafael J. Wysocki <rjw@sisk.pl>, Novell Inc.
 */
#include <linux/pm.h>
#include <linux/pm_runtime.h>
#include <linux/export.h>

#define DEFINE_PM_GENERIC_FUNC(func_name, op_name) \
int pm_generic_##func_name(struct device *dev) \
{ \
	const struct dev_pm_ops *pm = dev->driver ? dev->driver->pm : NULL; \
	return pm && pm->op_name ? pm->op_name(dev) : 0; \
} \
EXPORT_SYMBOL_GPL(pm_generic_##func_name)

#ifdef CONFIG_PM
/**
 * pm_generic_runtime_suspend - Generic runtime suspend callback for subsystems.
 * @dev: Device to suspend.
 *
 * If PM operations are defined for the @dev's driver and they include
 * ->runtime_suspend(), execute it and return its error code.  Otherwise,
 * return 0.
 */
DEFINE_PM_GENERIC_FUNC(runtime_suspend, runtime_suspend);

/**
 * pm_generic_runtime_resume - Generic runtime resume callback for subsystems.
 * @dev: Device to resume.
 *
 * If PM operations are defined for the @dev's driver and they include
 * ->runtime_resume(), execute it and return its error code.  Otherwise,
 * return 0.
 */
DEFINE_PM_GENERIC_FUNC(runtime_resume, runtime_resume);
#endif /* CONFIG_PM */

#ifdef CONFIG_PM_SLEEP
/**
 * pm_generic_prepare - Generic routine preparing a device for power transition.
 * @dev: Device to prepare.
 *
 * Prepare a device for a system-wide power transition.
 */
int pm_generic_prepare(struct device *dev)
{
	struct device_driver *drv = dev->driver;
	int ret = 0;

	if (drv && drv->pm && drv->pm->prepare)
		ret = drv->pm->prepare(dev);

	return ret;
}

/**
 * pm_generic_suspend_noirq - Generic suspend_noirq callback for subsystems.
 * @dev: Device to suspend.
 */
DEFINE_PM_GENERIC_FUNC(suspend_noirq, suspend_noirq);

/**
 * pm_generic_suspend_late - Generic suspend_late callback for subsystems.
 * @dev: Device to suspend.
 */
DEFINE_PM_GENERIC_FUNC(suspend_late, suspend_late);

/**
 * pm_generic_suspend - Generic suspend callback for subsystems.
 * @dev: Device to suspend.
 */
DEFINE_PM_GENERIC_FUNC(suspend, suspend);

/**
 * pm_generic_freeze_noirq - Generic freeze_noirq callback for subsystems.
 * @dev: Device to freeze.
 */
DEFINE_PM_GENERIC_FUNC(freeze_noirq, freeze_noirq);

/**
 * pm_generic_freeze - Generic freeze callback for subsystems.
 * @dev: Device to freeze.
 */
DEFINE_PM_GENERIC_FUNC(freeze, freeze);

/**
 * pm_generic_poweroff_noirq - Generic poweroff_noirq callback for subsystems.
 * @dev: Device to handle.
 */
DEFINE_PM_GENERIC_FUNC(poweroff_noirq, poweroff_noirq);

/**
 * pm_generic_poweroff_late - Generic poweroff_late callback for subsystems.
 * @dev: Device to handle.
 */
DEFINE_PM_GENERIC_FUNC(poweroff_late, poweroff_late);

/**
 * pm_generic_poweroff - Generic poweroff callback for subsystems.
 * @dev: Device to handle.
 */
DEFINE_PM_GENERIC_FUNC(poweroff, poweroff);

/**
 * pm_generic_thaw_noirq - Generic thaw_noirq callback for subsystems.
 * @dev: Device to thaw.
 */
DEFINE_PM_GENERIC_FUNC(thaw_noirq, thaw_noirq);

/**
 * pm_generic_thaw - Generic thaw callback for subsystems.
 * @dev: Device to thaw.
 */
DEFINE_PM_GENERIC_FUNC(thaw, thaw);

/**
 * pm_generic_resume_noirq - Generic resume_noirq callback for subsystems.
 * @dev: Device to resume.
 */
DEFINE_PM_GENERIC_FUNC(resume_noirq, resume_noirq);

/**
 * pm_generic_resume_early - Generic resume_early callback for subsystems.
 * @dev: Device to resume.
 */
DEFINE_PM_GENERIC_FUNC(resume_early, resume_early);

/**
 * pm_generic_resume - Generic resume callback for subsystems.
 * @dev: Device to resume.
 */
DEFINE_PM_GENERIC_FUNC(resume, resume);

/**
 * pm_generic_restore_noirq - Generic restore_noirq callback for subsystems.
 * @dev: Device to restore.
 */
DEFINE_PM_GENERIC_FUNC(restore_noirq, restore_noirq);

/**
 * pm_generic_restore_early - Generic restore_early callback for subsystems.
 * @dev: Device to resume.
 */
DEFINE_PM_GENERIC_FUNC(restore_early, restore_early);

/**
 * pm_generic_restore - Generic restore callback for subsystems.
 * @dev: Device to restore.
 */
DEFINE_PM_GENERIC_FUNC(restore, restore);

/**
 * pm_generic_complete - Generic routine completing a device power transition.
 * @dev: Device to handle.
 *
 * Complete a device power transition during a system-wide power transition.
 */
void pm_generic_complete(struct device *dev)
{
	struct device_driver *drv = dev->driver;

	if (drv && drv->pm && drv->pm->complete)
		drv->pm->complete(dev);
}
#endif /* CONFIG_PM_SLEEP */
