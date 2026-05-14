// SPDX-License-Identifier: GPL-2.0

#include <linux/pm_runtime.h>

void rust_helper_pm_runtime_get_noresume(struct device *dev)
{
	pm_runtime_get_noresume(dev);
}

void rust_helper_pm_runtime_put_noidle(struct device *dev)
{
	pm_runtime_put_noidle(dev);
}

void rust_helper_pm_runtime_mark_last_busy(struct device *dev)
{
	pm_runtime_mark_last_busy(dev) ;
}

bool rust_helper_pm_runtime_active(struct device *dev)
{
	return pm_runtime_active(dev);
}

void rust_helper_pm_suspend_ignore_children(struct device *dev, bool enable)
{
	pm_suspend_ignore_children(dev, enable);
}

int rust_helper_pm_runtime_set_active(struct device *dev)
{
	return pm_runtime_set_active(dev);
}

int rust_helper_pm_runtime_set_suspended(struct device *dev)
{
	return pm_runtime_set_suspended(dev);
}
