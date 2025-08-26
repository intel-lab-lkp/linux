/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * TTY - Mock driver
 *
 * Copyright (c) 2025 Abhinav Saxena <xandury@gmail.com>
 *
 */

#ifndef _TTY_MOCK_H
#define _TTY_MOCK_H

#include <linux/device.h>
#include <linux/tty_driver.h>
#include <linux/types.h>

/* Register a single-port mock tty driver and create device #0. */
int tty_mock_register(struct tty_driver **out_drv, struct device *parent);
/* Tear down device, unregister driver and destroy port. */
void tty_mock_unregister(struct tty_driver *drv);

/* --- Stats available to KUnit tests --- */
struct tty_mock_stats {
	u64 total_writes;
	u64 total_bytes;
	u32 last_write_len;
};

/* Returns a snapshot of counters. */
struct tty_mock_stats tty_mock_get_stats(void);

/* Reset all statistics counters to zero. */
void tty_mock_reset_stats(void);

#endif /* _TTY_MOCK_H */
