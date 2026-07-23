// SPDX-License-Identifier: GPL-2.0

#include <linux/watchdog.h>

__rust_helper bool rust_helper_watchdog_active(const struct watchdog_device *wdd)
{
	return watchdog_active(wdd);
}

__rust_helper bool rust_helper_watchdog_hw_running(const struct watchdog_device *wdd)
{
	return watchdog_hw_running(wdd);
}

__rust_helper void rust_helper_watchdog_set_nowayout(struct watchdog_device *wdd, bool nowayout)
{
	watchdog_set_nowayout(wdd, nowayout);
}

__rust_helper void rust_helper_watchdog_stop_on_reboot(struct watchdog_device *wdd)
{
	watchdog_stop_on_reboot(wdd);
}

__rust_helper void rust_helper_watchdog_stop_on_unregister(struct watchdog_device *wdd)
{
	watchdog_stop_on_unregister(wdd);
}

__rust_helper void rust_helper_watchdog_set_drvdata(struct watchdog_device *wdd, void *data)
{
	watchdog_set_drvdata(wdd, data);
}

__rust_helper void *rust_helper_watchdog_get_drvdata(struct watchdog_device *wdd)
{
	return watchdog_get_drvdata(wdd);
}
