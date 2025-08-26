// SPDX-License-Identifier: GPL-2.0
/*
 * Minimal mock TTY driver for KUnit tests. Based on ttynull and ttyprintk
 *
 * Behavior:
 *   - write() pretends to transmit all bytes immediately
 *   - write_room() is large
 *   - chars_in_buffer() is 0
 *
 * Tracks only: total_writes, total_bytes, last_write_len
 *
 * Copyright (c) 2025 Abhinav Saxena <xandury@gmail.com>
 */

#include <kunit/visibility.h>
#include <linux/tty.h>
#include <linux/tty_driver.h>
#include <linux/tty_flip.h>
#include <linux/module.h>
#include <linux/spinlock.h>

#include "tty_mock.h"

#define TTYMOCK_NAME "ttymock"
#define TTYMOCK_ROOM 4096

static struct tty_port mock_port; /* single port */

/* --- Stats (private) --- */
static struct {
	u64 total_writes;
	u64 total_bytes;
	u32 last_write_len;
	spinlock_t lock;
} mock_state;

/* --- tty_operations --- */

static int mock_open(struct tty_struct *tty, struct file *file)
{
	tty->driver_data = &mock_port;
	return tty_port_open(&mock_port, tty, file);
}

static void mock_close(struct tty_struct *tty, struct file *file)
{
	tty_port_close(&mock_port, tty, file);
	tty->driver_data = NULL;
}

static ssize_t mock_write(struct tty_struct *tty, const u8 *buf, size_t cnt)
{
	unsigned long flags;

	if (!buf)
		return -EINVAL;

	spin_lock_irqsave(&mock_state.lock, flags);
	mock_state.total_writes++;
	mock_state.total_bytes += cnt;
	mock_state.last_write_len = cnt;
	spin_unlock_irqrestore(&mock_state.lock, flags);

	return cnt; /* everything written immediately */
}

static unsigned int mock_write_room(struct tty_struct *tty)
{
	return TTYMOCK_ROOM;
}

static unsigned int mock_chars_in_buffer(struct tty_struct *tty)
{
	return 0;
}

static const struct tty_operations mock_ops = {
	.open = mock_open,
	.close = mock_close,
	.write = mock_write,
	.write_room = mock_write_room,
	.chars_in_buffer = mock_chars_in_buffer,
};

/* --- tty_port_operations --- */

static bool mock_carrier_raised(struct tty_port *port)
{
	return true;
}

static void mock_shutdown(struct tty_port *port) { }

static const struct tty_port_operations mock_port_ops = {
	.carrier_raised = mock_carrier_raised,
	.shutdown = mock_shutdown,
};

/* --- Public helpers --- */

int tty_mock_register(struct tty_driver **out_drv, struct device *parent)
{
	struct tty_driver *drv;
	struct device *dev;
	int ret;

	spin_lock_init(&mock_state.lock);

	drv = tty_alloc_driver(1, TTY_DRIVER_RESET_TERMIOS |
				  TTY_DRIVER_REAL_RAW |
				  TTY_DRIVER_UNNUMBERED_NODE |
				  TTY_DRIVER_DYNAMIC_DEV);
	if (IS_ERR(drv))
		return PTR_ERR(drv);

	drv->driver_name = TTYMOCK_NAME;
	drv->name = TTYMOCK_NAME;
	drv->type = TTY_DRIVER_TYPE_SERIAL;
	drv->subtype = SERIAL_TYPE_NORMAL;
	drv->init_termios = tty_std_termios;
	tty_set_operations(drv, &mock_ops);

	ret = tty_register_driver(drv);
	if (ret) {
		tty_driver_kref_put(drv);
		return ret;
	}

	tty_port_init(&mock_port);
	mock_port.ops = &mock_port_ops;

	dev = tty_port_register_device(&mock_port, drv, 0, parent);
	if (IS_ERR(dev)) {
		ret = PTR_ERR(dev);
		tty_unregister_driver(drv);
		tty_driver_kref_put(drv);
		tty_port_destroy(&mock_port);
		return ret;
	}

	if (out_drv)
		*out_drv = drv;
	return 0;
}
EXPORT_SYMBOL_IF_KUNIT(tty_mock_register);

void tty_mock_unregister(struct tty_driver *drv)
{
	if (!drv)
		return;

	tty_port_unregister_device(&mock_port, drv, 0);
	tty_unregister_driver(drv);
	tty_driver_kref_put(drv);
	tty_port_destroy(&mock_port);
}
EXPORT_SYMBOL_IF_KUNIT(tty_mock_unregister);

struct tty_mock_stats tty_mock_get_stats(void)
{
	unsigned long flags;
	struct tty_mock_stats state;

	spin_lock_irqsave(&mock_state.lock, flags);
	state.total_writes   = mock_state.total_writes;
	state.total_bytes    = mock_state.total_bytes;
	state.last_write_len = mock_state.last_write_len;
	spin_unlock_irqrestore(&mock_state.lock, flags);

	return state;
}
EXPORT_SYMBOL_IF_KUNIT(tty_mock_get_stats);

void tty_mock_reset_stats(void)
{
	unsigned long flags;

	spin_lock_irqsave(&mock_state.lock, flags);
	mock_state.total_writes = 0;
	mock_state.total_bytes = 0;
	mock_state.last_write_len = 0;
	spin_unlock_irqrestore(&mock_state.lock, flags);
}
EXPORT_SYMBOL_IF_KUNIT(tty_mock_reset_stats);

MODULE_LICENSE("GPL");
