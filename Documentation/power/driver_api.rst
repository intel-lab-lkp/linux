.. SPDX-License-Identifier: GPL-2.0+

==================
Driver Suspend API
==================


1. How Can A driver abort system suspend?
-----------------------------------------

Any driver can abort system-wide  by invoking pm_system_wakeup()
during the suspend flow.

ie. from the drivers suspend callbacks:
 .suspend()
 .suspend_noirq()
 .suspend_late()

Alternatively, if CONFIG_PM_SLEEP_LEGACY_CALLBACK_ABORT=y is present in .config,
then any non-zero return value from any device drivers callback:
 .suspend()
 .suspend_noirq()
 .suspend_late()
will abort the system-wide suspend flow.
Note that CONFIG_PM_SLEEP_LEGACY_CALLBACK_ABORT=n, by default.
