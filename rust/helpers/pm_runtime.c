// SPDX-License-Identifier: GPL-2.0

#include <linux/pm_runtime.h>

__rust_helper
void rust_helper_pm_runtime_get_noresume(struct device *dev)
{
	pm_runtime_get_noresume(dev);
}

__rust_helper
void rust_helper_pm_runtime_put_noidle(struct device *dev)
{
	pm_runtime_put_noidle(dev);
}

__rust_helper
void rust_helper_pm_runtime_mark_last_busy(struct device *dev)
{
	pm_runtime_mark_last_busy(dev);
}

__rust_helper
bool rust_helper_pm_runtime_active(struct device *dev)
{
	return pm_runtime_active(dev);
}

__rust_helper
bool rust_helper_pm_runtime_suspended(struct device *dev)
{
	return pm_runtime_suspended(dev);
}

__rust_helper
void rust_helper_pm_suspend_ignore_children(struct device *dev, bool enable)
{
	pm_suspend_ignore_children(dev, enable);
}

__rust_helper
int rust_helper_pm_runtime_set_active(struct device *dev)
{
	return pm_runtime_set_active(dev);
}

__rust_helper
int rust_helper_pm_runtime_set_suspended(struct device *dev)
{
	return pm_runtime_set_suspended(dev);
}

__rust_helper
bool rust_helper_pm_runtime_enabled(struct device *dev)
{
	return pm_runtime_enabled(dev);
}

__rust_helper
bool rust_helper_pm_runtime_is_irq_safe(struct device *dev)
{
	return pm_runtime_is_irq_safe(dev);
}

#ifndef CONFIG_PM
__rust_helper
int rust_helper___pm_runtime_resume(struct device *dev, int rpmflags)
{
	return __pm_runtime_resume(dev, rpmflags);
}

__rust_helper
int rust_helper___pm_runtime_idle(struct device *dev, int rpmflags)
{
	return __pm_runtime_idle(dev, rpmflags);
}

__rust_helper
int rust_helper___pm_runtime_suspend(struct device *dev, int rpmflags)
{
	return __pm_runtime_suspend(dev, rpmflags);
}

__rust_helper
int rust_helper_pm_runtime_get_if_active(struct device *dev)
{
	return pm_runtime_get_if_active(dev);
}

__rust_helper void rust_helper_pm_runtime_enable(struct device *dev) {}
__rust_helper void rust_helper___pm_runtime_disable(struct device *dev, bool c) {}
__rust_helper void rust_helper_pm_runtime_barrier(struct device *dev) {}
__rust_helper void rust_helper_pm_runtime_no_callbacks(struct device *dev) {}
__rust_helper void rust_helper_pm_runtime_irq_safe(struct device *dev) {}
__rust_helper
void rust_helper___pm_runtime_use_autosuspend(struct device *dev, bool use)
{}
__rust_helper
void rust_helper_pm_runtime_set_autosuspend_delay(struct device *dev, int delay)
{}
#endif /* CONFIG_PM */
