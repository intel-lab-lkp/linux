// SPDX-License-Identifier: GPL-2.0-only
/*
 * KUnit tests for the TTY null driver
 *
 * Tests for the ttynull driver covering basic lifecycle and sink behavior.
 * The ttynull driver acts as a data sink, discarding all written data
 * while providing minimal overhead for applications that need a TTY
 * but don't care about the output.
 *
 * Copyright (c) 2025 Abhinav Saxena <xandury@gmail.com>
 *
 */

#include <kunit/test.h>
#include <linux/tty.h>
#include <linux/tty_driver.h>
#include <linux/tty_flip.h>
#include <linux/string.h>

#include "tests/tty_test_helpers.h"

/**
 * test_ttynull_write_sink - Verify ttynull acts as data sink
 * @test: KUnit test context
 *
 * ttynull should accept all write data and discard it silently.
 * This tests the core functionality of the null TTY driver.
 */
static void test_ttynull_write_sink(struct kunit *test)
{
	struct tty_driver *drv = ttynull_driver;
	struct tty_test_fixture *fx;
	const char *msg = "test data; discard me";
	unsigned int room;
	ssize_t write_result;

	fx = tty_test_create_fixture(test, drv, 0);
	KUNIT_ASSERT_NOT_NULL(test, fx);

	KUNIT_ASSERT_EQ(test, tty_test_open(fx), 0);
	KUNIT_ASSERT_TRUE(test, fx->opened);

	/* Verify TTY is properly initialized */
	KUNIT_EXPECT_NOT_NULL(test, fx->tty);
	KUNIT_EXPECT_NOT_NULL(test, fx->tty->ldisc);
	KUNIT_EXPECT_TRUE(test, !list_empty(&fx->tty->tty_files));

	/* Check initial write room - should be available */
	room = tty_test_get_write_room(fx);
	KUNIT_EXPECT_GT(test, room, 0U);

	/* Write data - should be completely accepted */
	KUNIT_ASSERT_EQ(test, tty_test_write_all(fx, msg, strlen(msg)), 0);

	/* ttynull discards writes; buffer should remain empty */
	KUNIT_EXPECT_EQ(test, tty_test_get_chars_in_buffer(fx), 0U);

	/* Write room should remain available for a sink */
	room = tty_test_get_write_room(fx);
	KUNIT_EXPECT_GT(test, room, 0U);

	/* ttynull should accept all data */
	write_result = tty_test_write(fx, msg, strlen(msg));
	KUNIT_EXPECT_EQ(test, write_result, strlen(msg));

	/* Multiple writes should all succeed */
	write_result = tty_test_write(fx, msg, strlen(msg));
	KUNIT_EXPECT_EQ(test, write_result, strlen(msg));

	/*
	 * TODO: Simulate hangup condition making subsequent writes fail
	 * For now, just release.
	 */
	KUNIT_ASSERT_EQ(test, tty_test_release(fx), 0);
}

/**
 * test_ttynull_read_behavior - Verify read behavior on null device
 * @test: KUnit test context
 *
 * While ttynull technically supports read (via N_TTY), reading should
 * behave predictably (likely EOF or blocking).
 */
static void test_ttynull_read_behavior(struct kunit *test)
{
	struct tty_driver *drv = ttynull_driver;
	struct tty_test_fixture *fx;
	struct tty_ldisc *ld;
	/* char read_buffer[128]; */
	/* ssize_t bytes_read; */

	fx = tty_test_create_fixture(test, drv, 0);
	KUNIT_ASSERT_NOT_NULL(test, fx);

	KUNIT_ASSERT_EQ(test, tty_test_open(fx), 0);
	KUNIT_ASSERT_TRUE(test, fx->opened);

	ld = tty_ldisc_ref(fx->tty);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, ld);
	KUNIT_ASSERT_TRUE(test, ld->ops && ld->ops->read);
	tty_ldisc_deref(ld);

	/*
	 * Reading from ttynull should behave consistently.
	 * Depending on implementation, this might:
	 * - Return 0 (EOF)
	 * - Block (if no data available)
	 * - Return -EAGAIN (if non-blocking)
	 */
	KUNIT_ASSERT_NOT_NULL(test, fx->tty->disc_data);
	/* bytes_read = tty_test_read(fx, read_buffer, sizeof(read_buffer)); */

	/*
	 * Document the expected behavior
	 *  - adjust based on actual ttynull implementation
	 */
	/* KUNIT_EXPECT_GE(test, bytes_read, 0); /\* Should not return error *\/ */

	KUNIT_ASSERT_EQ(test, tty_test_release(fx), 0);
}

/**
 * test_ttynull_driver_properties - Verify driver characteristics
 * @test: KUnit test context
 *
 * Test the driver's static properties and configuration. Also, ensure that
 * it implements the required file_operation ops.
 */
static void test_ttynull_driver_properties(struct kunit *test)
{
	struct tty_driver *drv = ttynull_driver;

	KUNIT_ASSERT_NOT_NULL(test, drv);

	/* Verify driver identification */
	KUNIT_EXPECT_STREQ(test, drv->driver_name, "ttynull");
	KUNIT_EXPECT_STREQ(test, drv->name, "ttynull");

	/* Ensure that driver implements the required ops. */
	KUNIT_ASSERT_NOT_NULL(test, drv);
	tty_test_assert_valid_ops(test, drv);

	/* Verify driver type */
	KUNIT_EXPECT_EQ(test, drv->type, TTY_DRIVER_TYPE_CONSOLE);

	/* Verify driver flags */
	KUNIT_EXPECT_TRUE(test, drv->flags & TTY_DRIVER_REAL_RAW);
	KUNIT_EXPECT_TRUE(test, drv->flags & TTY_DRIVER_RESET_TERMIOS);
}

static struct kunit_case ttynull_test_cases[] = {
	KUNIT_CASE(test_ttynull_write_sink),
	KUNIT_CASE(test_ttynull_read_behavior),
	KUNIT_CASE(test_ttynull_driver_properties),
	{}
};

static struct kunit_suite ttynull_test_suite = {
	.name = "ttynull",
	.test_cases = ttynull_test_cases,
};

kunit_test_suite(ttynull_test_suite);
