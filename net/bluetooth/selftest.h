/* SPDX-License-Identifier: GPL-2.0 */
/*
   BlueZ - Bluetooth protocol stack for Linux
   Copyright (C) 2014 Intel Corporation
*/

#if IS_ENABLED(CONFIG_BT_SELFTEST) && IS_MODULE(CONFIG_BT)

/* When CONFIG_BT_SELFTEST=y and the CONFIG_BT=m, then the self testing
 * is run at module loading time.
 */
int bt_selftest(void);

#else

/* When CONFIG_BT_SELFTEST=y and CONFIG_BT=y, then the self testing
 * is run via late_initcall() to make sure that subsys_initcall() of
 * the Bluetooth subsystem and device_initcall() of the Crypto subsystem
 * do not clash.
 *
 * When CONFIG_BT_SELFTEST=n, then this turns into an empty call that
 * has no impact.
 */
static inline int bt_selftest(void)
{
	return 0;
}

#endif
