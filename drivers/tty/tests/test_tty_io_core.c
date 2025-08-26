// SPDX-License-Identifier: GPL-2.0
/*
 * Minimal KUnit tests for TTY core using the mock driver.
 *
 * Scope:
 *  - open/close via tty_port_* paths
 *  - write() returns cnt, write_room() is large, chars_in_buffer() == 0
 *  - stats (writes, bytes, last len) observable via tty_mock_get_stats()
 *  - file_ops read functionality with various termios configurations
 *  - set_termios operations for practical use cases
 *
 * Keep this small and obvious—no driver-side buffering or timers.
 *
 * Copyright (c) 2025 Abhinav Saxena <xandury@gmail.com>
 */

#include <kunit/test.h>
#include <linux/tty.h>
#include <linux/tty_driver.h>
#include <linux/tty_flip.h>
#include <linux/string.h>

#include "tty_test_helpers.h"
#include "tty_mock.h"          /* mock driver API: register/unregister/stats */

/* Single-port driver used across tests (minor 0). */
static struct tty_driver *mock_driver;

/* ---------- Per-test init: sanity only ---------- */
static int tty_core_init(struct kunit *test)
{
	KUNIT_ASSERT_NOT_NULL(test, mock_driver);

	/* Reset mock statistics before each test */
	tty_mock_reset_stats();

	if (mock_driver && mock_driver->ports)
		KUNIT_EXPECT_NOT_NULL(test, mock_driver->ports[0]);
	return 0;
}

/* ---------- Tests ---------- */

/*
 * Test: Verify mock driver has all required operations wired correctly.
 * Expected: All mandatory ops present, ports array initialised, no NULL pointers.
 */
static void test_driver_ops_present(struct kunit *test)
{
	KUNIT_ASSERT_NOT_NULL(test, mock_driver);

	/* Basic ops presence and wiring checks via helper. */
	tty_test_assert_valid_ops(test, mock_driver);
	KUNIT_EXPECT_NOT_NULL(test, mock_driver->ops->write);
	KUNIT_EXPECT_NOT_NULL(test, mock_driver->ops->write_room);
	KUNIT_EXPECT_NOT_NULL(test, mock_driver->ops->chars_in_buffer);

	if (mock_driver->ports)
		KUNIT_EXPECT_NOT_NULL(test, mock_driver->ports[0]);
}

/*
 * Test: Basic TTY lifecycle (open/write/close).
 * Expected: write() returns byte count, stats increment correctly,
 * ldisc/port setup.
 */
static void test_basic_open_write_close(struct kunit *test)
{
	unsigned int idx = 0;
	struct tty_test_fixture *fx;
	const char *msg = "Hello, tty!\n";
	struct tty_mock_stats before, after;
	ssize_t ret;

	fx = tty_test_create_fixture(test, mock_driver, idx);
	KUNIT_ASSERT_NOT_NULL(test, fx);
	KUNIT_ASSERT_EQ(test, tty_test_open(fx), 0);
	KUNIT_ASSERT_TRUE(test, fx->opened);

	KUNIT_EXPECT_TRUE(test,
			  fx->tty && fx->tty->ldisc && fx->tty->ldisc->ops);
	KUNIT_EXPECT_TRUE(test, !list_empty(&fx->tty->tty_files));
	KUNIT_EXPECT_NOT_NULL(test, fx->tty->port);

	before = tty_mock_get_stats();

	ret = tty_test_write(fx, msg, strlen(msg));
	KUNIT_EXPECT_EQ(test, ret, (ssize_t)strlen(msg));

	after = tty_mock_get_stats();

	/* Test interface contract, not implementation details */
	KUNIT_EXPECT_GT(test, after.total_writes, before.total_writes);
	KUNIT_EXPECT_GE(test, after.total_bytes,
			before.total_bytes + strlen(msg));
	KUNIT_EXPECT_GT(test, after.last_write_len, 0u);

	KUNIT_EXPECT_EQ(test, tty_test_release(fx), 0);
}

/*
 * Test: write_room() and chars_in_buffer() consistency during operations.
 * Expected: write_room() > 0, chars_in_buffer() == 0, room unchanged after
 * writes.
 */
static void test_write_room_and_chars_in_buffer_invariants(struct kunit *test)
{
	unsigned int idx = 0;
	struct tty_test_fixture *fx;
	char buf[64];
	unsigned int room_before, room_after;
	ssize_t ret;

	memset(buf, 'A', sizeof(buf));

	fx = tty_test_create_fixture(test, mock_driver, idx);
	KUNIT_ASSERT_NOT_NULL(test, fx);
	KUNIT_ASSERT_EQ(test, tty_test_open(fx), 0);

	room_before = tty_write_room(fx->tty);
	KUNIT_EXPECT_GT(test, room_before, 0u);
	KUNIT_EXPECT_EQ(test, fx->tty->ops->chars_in_buffer(fx->tty), 0u);

	ret = tty_test_write(fx, buf, sizeof(buf));
	KUNIT_EXPECT_EQ(test, ret, (ssize_t)sizeof(buf));

	room_after = tty_write_room(fx->tty);
	KUNIT_EXPECT_EQ(test, fx->tty->ops->chars_in_buffer(fx->tty), 0u);
	KUNIT_EXPECT_GE(test, room_after, room_before);

	KUNIT_EXPECT_EQ(test, tty_test_release(fx), 0);
}

/*
 * Test: RX data injection via flip buffers if line discipline supports it.
 * Expected: tty_test_simulate_rx() returns injected byte count, or test
 * skipped.
 */
static void test_flip_rx_if_supported(struct kunit *test)
{
	unsigned int idx = 0;
	struct tty_test_fixture *fx;
	int rx_result;

	/* Raw byte buffer, not NUL-terminated */
	static const unsigned char rx_data[] = "rx-line\n";

	fx = tty_test_create_fixture(test, mock_driver, idx);
	KUNIT_ASSERT_NOT_NULL(test, fx);
	KUNIT_ASSERT_EQ(test, tty_test_open(fx), 0);

	KUNIT_EXPECT_TRUE(test,
			  fx->tty && fx->tty->ldisc && fx->tty->ldisc->ops);
	KUNIT_EXPECT_TRUE(test, !list_empty(&fx->tty->tty_files));
	KUNIT_EXPECT_NOT_NULL(test, fx->tty->port);

	if (tty_fx_supports_rx(fx)) {
		rx_result = tty_test_simulate_rx(fx, rx_data, sizeof(rx_data));
		KUNIT_EXPECT_EQ(test, rx_result, (int)sizeof(rx_data));
	} else {
		kunit_skip(test,
			   "ldisc does not support RX; skipping injection");
	}

	KUNIT_EXPECT_EQ(test, tty_test_release(fx), 0);
}

/*
 * Test: set_termios() for hardware settings (baud rate, character size, parity).
 * Expected: c_cflag settings persist correctly, hardware parameters validated.
 */
static void test_set_termios_baud_rate_and_character_size(struct kunit *test)
{
	unsigned int idx = 0;
	struct tty_test_fixture *fx;
	struct ktermios termios_before, termios_after;

	fx = tty_test_create_fixture(test, mock_driver, idx);
	KUNIT_ASSERT_NOT_NULL(test, fx);
	KUNIT_ASSERT_EQ(test, tty_test_open(fx), 0);

	/* Get initial termios */
	termios_before = fx->tty->termios;

	/* Test baud rate changes */
	termios_after = termios_before;
	termios_after.c_cflag &= ~CBAUD;
	termios_after.c_cflag |= B9600;
	KUNIT_ASSERT_EQ(test, tty_test_set_termios(fx, &termios_after), 0);
	KUNIT_EXPECT_EQ(test, fx->tty->termios.c_cflag & CBAUD, B9600);

	/* Test higher baud rate */
	termios_after.c_cflag &= ~CBAUD;
	termios_after.c_cflag |= B115200;
	KUNIT_ASSERT_EQ(test, tty_test_set_termios(fx, &termios_after), 0);
	KUNIT_EXPECT_EQ(test, fx->tty->termios.c_cflag & CBAUD, B115200);

	/* Test character size changes */
	termios_after.c_cflag &= ~CSIZE;
	termios_after.c_cflag |= CS7; /* 7-bit characters */
	KUNIT_ASSERT_EQ(test, tty_test_set_termios(fx, &termios_after), 0);
	KUNIT_EXPECT_EQ(test, fx->tty->termios.c_cflag & CSIZE, CS7);

	termios_after.c_cflag &= ~CSIZE;
	termios_after.c_cflag |= CS8; /* 8-bit characters */
	KUNIT_ASSERT_EQ(test, tty_test_set_termios(fx, &termios_after), 0);
	KUNIT_EXPECT_EQ(test, fx->tty->termios.c_cflag & CSIZE, CS8);

	/* Test parity settings */
	termios_after.c_cflag |= PARENB; /* Enable parity */
	termios_after.c_cflag &= ~PARODD; /* Even parity */
	KUNIT_ASSERT_EQ(test, tty_test_set_termios(fx, &termios_after), 0);
	KUNIT_EXPECT_TRUE(test, fx->tty->termios.c_cflag & PARENB);
	KUNIT_EXPECT_FALSE(test, fx->tty->termios.c_cflag & PARODD);

	KUNIT_EXPECT_EQ(test, tty_test_release(fx), 0);
}

/* ---------- Suite registration ---------- */

static int tty_core_suite_init(struct kunit_suite *suite)
{
	return tty_mock_register(&mock_driver, NULL);
}

static void tty_core_suite_exit(struct kunit_suite *suite)
{
	tty_mock_unregister(mock_driver);
	mock_driver = NULL;
}

static struct kunit_case tty_core_cases[] = {
	KUNIT_CASE(test_driver_ops_present),
	KUNIT_CASE(test_basic_open_write_close),
	KUNIT_CASE(test_write_room_and_chars_in_buffer_invariants),
	KUNIT_CASE(test_flip_rx_if_supported),
	KUNIT_CASE(test_set_termios_baud_rate_and_character_size),
	{}
};

static struct kunit_suite tty_core_suite = {
	.name = "tty_io_core",
	.init = tty_core_init,
	.suite_init = tty_core_suite_init,
	.suite_exit = tty_core_suite_exit,
	.test_cases = tty_core_cases,
};

kunit_test_suite(tty_core_suite);
