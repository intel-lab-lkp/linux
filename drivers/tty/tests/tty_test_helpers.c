// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit test helpers for TTY drivers
 *
 * This file is included directly into tty_io.c when CONFIG_TTY_KUNIT_TESTS=y.
 * This allows the helper functions to access internal TTY functions like
 * tty_open() and tty_release() while providing exported symbols for use
 * by test modules.
 *
 * All functions are exported via EXPORT_SYMBOL_IF_KUNIT() so they are
 * only available when KUNIT is enabled, preventing pollution of the
 * production symbol table.
 *
 * Copyright (c) 2025 Abhinav Saxena <xandfury@gmail.com>
 */

#include <kunit/test.h>
#include <kunit/visibility.h>
#include <linux/fs.h>
#include <linux/kdev_t.h>
#include <linux/uio.h>
#include <linux/tty.h>
#include <linux/tty_driver.h>
#include <linux/tty_flip.h>
#include <linux/tty_ldisc.h>
#include <linux/termios.h>

#include "tests/tty_test_helpers.h"


static struct cdev tty_cdev;

/**
 * _tty_test_cleanup_release - KUnit cleanup action for TTY release
 * @data: Pointer to tty_test_fixture
 *
 * Internal cleanup function registered with kunit_add_action() to ensure
 * TTY is properly released even if test fails or exits early.
 * This prevents resource leaks and system instability.
 */
static void _tty_test_cleanup_release(void *data)
{
	struct tty_test_fixture *fx = data;
	int ret;

	if (!fx || !fx->opened || !fx->file || !fx->inode)
		return;

	ret = tty_release(fx->inode, fx->file);
	if (ret)
		pr_warn("TTY test cleanup failed: %d\n", ret);
	fx->opened = false;
}

/**
 * tty_test_create_fixture - Create a test fixture for TTY driver testing
 */
struct tty_test_fixture *tty_test_create_fixture(struct kunit *test,
						 struct tty_driver *driver,
						 unsigned int index)
{
	struct tty_test_fixture *fx;

	KUNIT_ASSERT_NOT_NULL(test, driver);

	fx = kunit_kzalloc(test, sizeof(*fx), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, fx);

	fx->test = test;
	fx->driver = driver;
	fx->dev = MKDEV(driver->major, driver->minor_start + index);

	/* Create synthetic VFS structures for real TTY operations */
	fx->file = kunit_kzalloc(test, sizeof(*fx->file), GFP_KERNEL);
	fx->inode = kunit_kzalloc(test, sizeof(*fx->inode), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, fx->file);
	KUNIT_ASSERT_NOT_NULL(test, fx->inode);

	/* Initialize as character device with appropriate permissions */
	init_special_inode(fx->inode, S_IFCHR | 0600, fx->dev);
	fx->inode->i_rdev = fx->dev;
	fx->inode->i_cdev = &tty_cdev;
	KUNIT_ASSERT_NOT_NULL(test, fx->inode->i_cdev);

	fx->file->f_flags = O_RDWR;
	fx->file->f_mode = FMODE_READ | FMODE_WRITE;
	fx->file->f_inode = fx->inode;

	/* Register cleanup before any operations that might fail */
	kunit_add_action(test, _tty_test_cleanup_release, fx);

	fx->opened = false;
	return fx;
}
EXPORT_SYMBOL_IF_KUNIT(tty_test_create_fixture);

/**
 * tty_test_open - Open TTY through standard kernel path
 */
int tty_test_open(struct tty_test_fixture *fx)
{
	int ret;

	KUNIT_ASSERT_NOT_NULL(fx->test, fx);
	KUNIT_ASSERT_NOT_NULL(fx->test, fx->file);
	KUNIT_ASSERT_NOT_NULL(fx->test, fx->inode);

	ret = tty_open(fx->inode, fx->file);
	if (ret)
		return ret;

	fx->tty = file_tty(fx->file);
	KUNIT_ASSERT_NOT_NULL(fx->test, fx->tty);

	/* Verify the TTY is properly set up */
	KUNIT_EXPECT_TRUE(fx->test, !list_empty(&fx->tty->tty_files));
	/* Ldisc must now be fully installed */
	KUNIT_ASSERT_NOT_NULL(fx->test, fx->tty->ldisc);
	KUNIT_EXPECT_TRUE(fx->test, fx->tty->ldisc->ops);
	KUNIT_ASSERT_NOT_NULL(fx->test, fx->tty->disc_data);
	KUNIT_EXPECT_NOT_NULL(fx->test, fx->tty->port);

	fx->port = fx->tty->port;
	ret = fx->tty->ldisc->ops->open(fx->tty);
	if (ret) {
		tty_release(fx->inode, fx->file);
		return ret;
	}

	/* Enable non-blocking mode for predictable test behavior */
	fx->file->f_flags |= O_NONBLOCK;
	fx->opened = true;
	return 0;
}
EXPORT_SYMBOL_IF_KUNIT(tty_test_open);

/**
 * tty_test_release - Close TTY through standard kernel path
 */
int tty_test_release(struct tty_test_fixture *fx)
{
	int ret;

	if (!fx || !fx->opened)
		return 0;

	/*
	 * This calls the internal tty_release() function directly.
	 * This works because this code is compiled as part of tty_io.c.
	 */
	ret = tty_release(fx->inode, fx->file);
	if (!ret) {
		fx->opened = false;
		fx->tty = NULL;
		fx->port = NULL;
	}
	return ret;
}
EXPORT_SYMBOL_IF_KUNIT(tty_test_release);

/**
 * tty_test_write - Write data to TTY
 */
ssize_t tty_test_write(struct tty_test_fixture *fx, const void *buf,
		       size_t count)
{
	struct kiocb iocb;
	struct iov_iter from;
	struct kvec kvec = { .iov_base = (void *)buf, .iov_len = count };

	KUNIT_ASSERT_NOT_NULL(fx->test, fx);
	KUNIT_ASSERT_NOT_NULL(fx->test, fx->file);
	KUNIT_ASSERT_TRUE(fx->test, fx->opened);

	init_sync_kiocb(&iocb, fx->file);
	iov_iter_kvec(&from, WRITE, &kvec, 1, count);

	/* tty_write() is exported, so this works */
	return tty_write(&iocb, &from);
}
EXPORT_SYMBOL_IF_KUNIT(tty_test_write);

/**
 * tty_test_write_all - Write all data or fail
 */
int tty_test_write_all(struct tty_test_fixture *fx, const void *buf, size_t len)
{
	size_t off = 0;
	int retries = 10;

	KUNIT_ASSERT_NOT_NULL(fx->test, fx);
	KUNIT_ASSERT_TRUE(fx->test, fx->opened);

	while (off < len && retries--) {
		ssize_t n =
			tty_test_write(fx, (const char *)buf + off, len - off);
		if (n < 0)
			return n;
		if (n == 0) {
			/* No progress - prevent infinite loop */
			if (--retries <= 0) {
				KUNIT_FAIL(fx->test,
					   "Write stalled after %zu bytes",
					   off);
				return -EIO;
			}
			continue;
		}
		off += n;
	}

	if (off < len) {
		KUNIT_FAIL(fx->test, "Incomplete write: %zu/%zu bytes", off,
			   len);
		return -EIO;
	}

	return 0;
}
EXPORT_SYMBOL_IF_KUNIT(tty_test_write_all);

/**
 * tty_test_read - Read data from TTY (non-blocking)
 */
ssize_t tty_test_read(struct tty_test_fixture *fx, void *buf, size_t count)
{
	struct kiocb iocb;
	struct iov_iter to;
	struct kvec kvec = { .iov_base = buf, .iov_len = count };

	KUNIT_ASSERT_NOT_NULL(fx->test, fx);
	KUNIT_ASSERT_NOT_NULL(fx->test, fx->file);
	KUNIT_ASSERT_TRUE(fx->test, fx->opened);

	init_sync_kiocb(&iocb, fx->file);
	iov_iter_kvec(&to, READ, &kvec, 1, count);

	return tty_read(&iocb, &to);
}
EXPORT_SYMBOL_IF_KUNIT(tty_test_read);

/**
 * tty_test_read_all - Attempt to read all requested data
 */
ssize_t tty_test_read_all(struct tty_test_fixture *fx, void *buf, size_t want)
{
	size_t off = 0;
	int tries = 8;

	KUNIT_ASSERT_NOT_NULL(fx->test, fx);
	KUNIT_ASSERT_TRUE(fx->test, fx->opened);

	while (off < want && tries--) {
		ssize_t n = tty_test_read(fx, (char *)buf + off, want - off);

		if (n == -EAGAIN)
			continue;
		if (n < 0)
			return n;
		if (n == 0)
			continue;
		off += n;
	}
	return off;
}
EXPORT_SYMBOL_IF_KUNIT(tty_test_read_all);

/**
 * tty_test_simulate_rx - Inject received data for testing
 */
int tty_test_simulate_rx(struct tty_test_fixture *fx, const unsigned char *data,
			 size_t len)
{
	int ret;

	KUNIT_ASSERT_NOT_NULL(fx->test, fx);
	KUNIT_ASSERT_NOT_NULL(fx->test, fx->port);
	KUNIT_ASSERT_TRUE(fx->test, fx->opened);

	ret = tty_insert_flip_string(fx->port, data, len);
	if (ret > 0)
		tty_flip_buffer_push(fx->port);

	return ret;
}
EXPORT_SYMBOL_IF_KUNIT(tty_test_simulate_rx);

/**
 * tty_fx_supports_rx - Check if fixture supports RX testing
 */
bool tty_fx_supports_rx(const struct tty_test_fixture *fx)
{
	struct tty_ldisc *ld;
	const struct tty_ldisc_ops *ops;

	if (!fx || !fx->tty || !fx->opened)
		return false;

	ld = tty_ldisc_ref(fx->tty);
	if (!ld)
		return false;

	ops = READ_ONCE(ld->ops);
	if (ops && (ops->receive_buf || ops->receive_buf2)) {
		tty_ldisc_deref(ld);
		return true;
	}

	tty_ldisc_deref(ld);
	return false;
}
EXPORT_SYMBOL_IF_KUNIT(tty_fx_supports_rx);

/**
 * tty_test_assert_valid_ops - Validate driver has required operations
 */
void tty_test_assert_valid_ops(struct kunit *test,
			       const struct tty_driver *driver)
{
	KUNIT_ASSERT_NOT_NULL(test, driver);
	KUNIT_ASSERT_NOT_NULL(test, driver->ops);
	KUNIT_ASSERT_NOT_NULL(test, driver->ops->open);
	KUNIT_ASSERT_NOT_NULL(test, driver->ops->close);
	KUNIT_ASSERT_NOT_NULL(test, driver->ops->write);
	KUNIT_ASSERT_NOT_NULL(test, driver->ops->write_room);
	KUNIT_EXPECT_TRUE(test, driver->flags & TTY_DRIVER_INSTALLED);
}
EXPORT_SYMBOL_IF_KUNIT(tty_test_assert_valid_ops);

/**
 * tty_test_get_chars_in_buffer - Get number of chars in output buffer
 */
unsigned int tty_test_get_chars_in_buffer(struct tty_test_fixture *fx)
{
	KUNIT_ASSERT_NOT_NULL(fx->test, fx);
	KUNIT_ASSERT_TRUE(fx->test, fx->opened);
	KUNIT_ASSERT_NOT_NULL(fx->test, fx->tty);

	if (fx->tty->ops->chars_in_buffer)
		return fx->tty->ops->chars_in_buffer(fx->tty);

	return 0;
}
EXPORT_SYMBOL_IF_KUNIT(tty_test_get_chars_in_buffer);

/**
 * tty_test_get_write_room - Get available write room
 */
unsigned int tty_test_get_write_room(struct tty_test_fixture *fx)
{
	KUNIT_ASSERT_NOT_NULL(fx->test, fx);
	KUNIT_ASSERT_TRUE(fx->test, fx->opened);
	KUNIT_ASSERT_NOT_NULL(fx->test, fx->tty);

	if (fx->tty->ops->write_room)
		return fx->tty->ops->write_room(fx->tty);

	return 0;
}
EXPORT_SYMBOL_IF_KUNIT(tty_test_get_write_room);

/**
 * tty_test_set_termios - Set terminal attributes for testing
 */
int tty_test_set_termios(struct tty_test_fixture *fx,
			 const struct ktermios *termios)
{
	struct ktermios old_termios;

	KUNIT_ASSERT_NOT_NULL(fx->test, fx);
	KUNIT_ASSERT_TRUE(fx->test, fx->opened);
	KUNIT_ASSERT_NOT_NULL(fx->test, fx->tty);
	KUNIT_ASSERT_NOT_NULL(fx->test, termios);

	/* Save old termios for potential restoration */
	old_termios = fx->tty->termios;

	/* Update termios */
	fx->tty->termios = *termios;

	/* Call driver's set_termios if it exists */
	if (fx->tty->ops->set_termios)
		fx->tty->ops->set_termios(fx->tty, &old_termios);

	return 0;
}
EXPORT_SYMBOL_IF_KUNIT(tty_test_set_termios);
