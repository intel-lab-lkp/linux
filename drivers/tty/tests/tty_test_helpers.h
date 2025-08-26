/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * KUnit test helpers for TTY drivers - Header declarations
 *
 * Provides reusable infrastructure for testing TTY drivers through
 * real kernel entry points without requiring userspace interaction
 * or hardware dependencies.
 *
 * The implementation (tty_test_helpers.c) is included directly into
 * tty_io.c to allow access to internal TTY functions while providing
 * exported symbols for test modules.
 *
 * Copyright (c) 2025 Abhinav Saxena <xandfury@gmail.com>
 *
 */

#ifndef _TTY_TEST_HELPERS_H
#define _TTY_TEST_HELPERS_H

#include <kunit/test.h>
#include <linux/tty.h>
#include <linux/tty_driver.h>
#include <linux/fs.h>

/**
 * struct tty_test_fixture - Test fixture for TTY driver testing
 * @test: KUnit test context for assertions and memory management
 * @driver: TTY driver being tested
 * @tty: TTY structure (valid after successful open)
 * @port: TTY port structure (valid after successful open)
 * @file: Synthetic file structure for VFS operations
 * @inode: Synthetic inode structure for device operations
 * @dev: Device number (major:minor) for this TTY
 * @opened: True if TTY has been opened successfully
 *
 * This fixture provides all necessary structures for testing TTY drivers
 * through the standard kernel interfaces. Memory is managed by KUnit and
 * automatic cleanup ensures proper resource release.
 */
struct tty_test_fixture {
	struct kunit *test;
	struct tty_driver *driver;
	struct tty_struct *tty;
	struct tty_port *port;
	struct file *file;
	struct inode *inode;
	dev_t dev;
	bool opened;
};

/* Core fixture management */

/**
 * tty_test_create_fixture - Create a test fixture for TTY driver testing
 * @test: KUnit test context
 * @driver: TTY driver to test (must be registered)
 * @index: Minor number index for this TTY instance
 *
 * Creates a complete test fixture with synthetic VFS structures that
 * enable testing through real tty_open()/tty_release() paths.
 * All memory is managed by KUnit with automatic cleanup.
 *
 * Return: Allocated fixture or NULL on failure (test will abort)
 */
struct tty_test_fixture *tty_test_create_fixture(struct kunit *test,
						 struct tty_driver *driver,
						 unsigned int index);

/* TTY lifecycle operations */

/**
 * tty_test_open - Open TTY through standard kernel path
 * @fx: Test fixture created with tty_test_create_fixture()
 *
 * Opens the TTY using tty_open(), the same entry point used by userspace.
 * This exercises the complete open sequence including driver install,
 * line discipline attachment, and port initialization.
 *
 * After successful open:
 * - fx->tty points to the allocated TTY structure
 * - fx->port points to the associated TTY port
 * - File is set to non-blocking mode for test convenience
 *
 * Return: 0 on success, negative error code on failure
 */
int tty_test_open(struct tty_test_fixture *fx);

/**
 * tty_test_release - Close TTY through standard kernel path
 * @fx: Test fixture with opened TTY
 *
 * Closes the TTY using tty_release(), exercising the complete close
 * Safe to call multiple times or on unopened fixtures.
 *
 * Return: 0 on success, negative error code on failure
 */
int tty_test_release(struct tty_test_fixture *fx);

/* Data transfer operations */

/**
 * tty_test_write - Write data to TTY
 * @fx: Test fixture with opened TTY
 * @buf: Data buffer to write
 * @count: Number of bytes to write
 *
 * Writes data using tty_write(), the same path used by userspace write().
 * This exercises line discipline processing, flow control, and driver
 * write operations. May return partial writes based on buffer availability.
 *
 * Return: Number of bytes written, or negative error code
 */
ssize_t tty_test_write(struct tty_test_fixture *fx, const void *buf,
		       size_t count);

/**
 * tty_test_write_all - Write all data or fail
 * @fx: Test fixture with opened TTY
 * @buf: Data buffer to write completely
 * @len: Number of bytes that must be written
 *
 * Ensures all data is written by retrying partial writes.
 * Useful for testing scenarios where complete data delivery is required.
 * Will assert-fail the test if any individual write returns 0 bytes.
 *
 * Return: 0 on complete success, negative error code on failure
 */
int tty_test_write_all(struct tty_test_fixture *fx, const void *buf,
		       size_t len);

/**
 * tty_test_read - Read data from TTY (non-blocking)
 * @fx: Test fixture with opened TTY
 * @buf: Buffer to receive data
 * @count: Maximum bytes to read
 *
 * Reads data using tty_read() in non-blocking mode. This is useful for
 * verifying that injected RX data is properly delivered through the
 * line discipline to userspace. Returns immediately with -EAGAIN if
 * no data is available.
 *
 * Return: Number of bytes read, -EAGAIN if no data, or other negative error
 */
ssize_t tty_test_read(struct tty_test_fixture *fx, void *buf, size_t count);

/**
 * tty_test_read_all - Attempt to read all requested data
 * @fx: Test fixture with opened TTY
 * @buf: Buffer to receive data
 * @want: Number of bytes desired
 *
 * Makes a bounded number of read attempts to collect the requested amount
 * of data. Useful for reading back data that was injected via flip buffers,
 * accounting for potential delays in line discipline processing.
 *
 * Return: Number of bytes actually read (may be less than requested)
 */
ssize_t tty_test_read_all(struct tty_test_fixture *fx, void *buf, size_t want);

/* RX simulation and testing */

/**
 * tty_test_simulate_rx - Inject received data for testing
 * @fx: Test fixture with opened TTY
 * @data: Data bytes to inject
 * @len: Number of bytes to inject
 *
 * Simulates data reception by injecting bytes through the flip buffer
 * interface and pushing them to the line discipline. This allows testing
 * of RX data paths, flow control, and line discipline processing without
 * requiring actual hardware or external data sources.
 *
 * Return: Number of bytes successfully queued, or negative error code
 */
int tty_test_simulate_rx(struct tty_test_fixture *fx, const unsigned char *data,
			 size_t len);

/**
 * tty_fx_supports_rx - Check if fixture supports RX testing
 * @fx: Test fixture to check
 *
 * Determines if the TTY has a line discipline attached that can receive
 * data. This is used to conditionally run RX-related tests since not all
 * TTY configurations support data reception (e.g., write-only devices).
 *
 * Return: true if RX testing is supported, false otherwise
 */
bool tty_fx_supports_rx(const struct tty_test_fixture *fx);

/* Driver validation and utility functions */

/**
 * tty_test_assert_valid_ops - Validate driver has required operations
 * @test: KUnit test context
 * @driver: TTY driver to validate
 *
 * Performs basic sanity checks on TTY driver structure to ensure it has
 * the minimum required operations. This catches configuration errors that
 * would cause NULL pointer dereferences during testing.
 */
void tty_test_assert_valid_ops(struct kunit *test,
			       const struct tty_driver *driver);

/**
 * tty_test_get_chars_in_buffer - Get number of chars in output buffer
 * @fx: Test fixture with opened TTY
 *
 * Returns the number of characters currently in the driver's output buffer.
 * Useful for testing flow control and buffer management.
 *
 * Return: Number of characters in buffer, or 0 if not supported
 */
unsigned int tty_test_get_chars_in_buffer(struct tty_test_fixture *fx);

/**
 * tty_test_get_write_room - Get available write room
 * @fx: Test fixture with opened TTY
 *
 * Returns the number of bytes that can be written without blocking.
 * Useful for testing buffer management and flow control.
 *
 * Return: Number of bytes available for writing
 */
unsigned int tty_test_get_write_room(struct tty_test_fixture *fx);

/**
 * tty_test_set_termios - Set terminal attributes for testing
 * @fx: Test fixture with opened TTY
 * @termios: Terminal attributes to set
 *
 * Sets terminal attributes through the standard termios interface.
 * Useful for testing different terminal configurations.
 *
 * Return: 0 on success, negative error code on failure
 */
int tty_test_set_termios(struct tty_test_fixture *fx,
			 const struct ktermios *termios);

#endif /* _TTY_TEST_HELPERS_H */
