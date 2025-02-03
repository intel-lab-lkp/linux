// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright 2025 Thomas Weißschuh <linux@weissschuh.net>
 *
 * Mock-up PTP Hardware Clock device
 *
 * Create a PTP device which offers PTP time manipulation operations
 * using a timecounter/cyclecounter on top of CLOCK_MONOTONIC_RAW.
 */

#include <linux/err.h>
#include <linux/module.h>
#include <linux/ptp_mock.h>

static struct mock_phc *phc;

static int __init ptp_mock_dev_init(void)
{
	phc = mock_phc_create(NULL);
	return PTR_ERR_OR_ZERO(phc);
}
module_init(ptp_mock_dev_init);

static void __exit ptp_mock_dev_exit(void)
{
	mock_phc_destroy(phc);
}
module_exit(ptp_mock_dev_exit);

MODULE_DESCRIPTION("Mock-up PTP Hardware Clock device");
MODULE_AUTHOR("Thomas Weißschuh <linux@weissschuh.net>");
MODULE_LICENSE("GPL");
