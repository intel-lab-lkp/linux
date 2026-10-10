// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit test for the input core.
 *
 * Copyright (c) 2023 Red Hat Inc
 */

#include <linux/delay.h>
#include <linux/input.h>

#include <kunit/test.h>

#define POLL_INTERVAL 100

static int input_test_init(struct kunit *test)
{
	struct input_dev *input_dev;
	int ret;

	input_dev = input_allocate_device();
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, input_dev);

	input_dev->name = "Test input device";
	input_dev->id.bustype = BUS_VIRTUAL;
	input_dev->id.vendor = 1;
	input_dev->id.product = 1;
	input_dev->id.version = 1;
	input_set_capability(input_dev, EV_KEY, BTN_LEFT);
	input_set_capability(input_dev, EV_KEY, BTN_RIGHT);

	ret = input_register_device(input_dev);
	if (ret) {
		input_free_device(input_dev);
		KUNIT_FAIL_AND_ABORT(test, "Register device failed: %d", ret);
	}

	test->priv = input_dev;

	return 0;
}

static void input_test_exit(struct kunit *test)
{
	struct input_dev *input_dev = test->priv;

	if (input_dev)
		input_unregister_device(input_dev);
}

static void input_test_poll(struct input_dev *input) { }

static void input_test_polling(struct kunit *test)
{
	struct input_dev *input_dev = test->priv;

	/* Must fail because a poll handler has not been set-up yet */
	KUNIT_ASSERT_EQ(test, input_get_poll_interval(input_dev), -EINVAL);

	KUNIT_ASSERT_EQ(test, input_setup_polling(input_dev, input_test_poll), 0);

	input_set_poll_interval(input_dev, POLL_INTERVAL);

	/* Must succeed because poll handler was set-up and poll interval set */
	KUNIT_ASSERT_EQ(test, input_get_poll_interval(input_dev), POLL_INTERVAL);
}

static void input_test_timestamp(struct kunit *test)
{
	const ktime_t invalid_timestamp = ktime_set(0, 0);
	struct input_dev *input_dev = test->priv;
	ktime_t *timestamp, time;

	timestamp = input_get_timestamp(input_dev);
	time = timestamp[INPUT_CLK_MONO];

	/* The returned timestamp must always be valid */
	KUNIT_ASSERT_EQ(test, ktime_compare(time, invalid_timestamp), 1);

	time = ktime_get();
	input_set_timestamp(input_dev, time);

	timestamp = input_get_timestamp(input_dev);
	/* The timestamp must be the same than set before */
	KUNIT_ASSERT_EQ(test, ktime_compare(timestamp[INPUT_CLK_MONO], time), 0);
}

static void input_test_match_device_id(struct kunit *test)
{
	struct input_dev *input_dev = test->priv;
	struct input_device_id id = { 0 };

	/*
	 * Must match when the input device bus, vendor, product, version
	 * and events capable of handling are the same and fail to match
	 * otherwise.
	 */
	id.flags = INPUT_DEVICE_ID_MATCH_BUS;
	id.bustype = BUS_VIRTUAL;
	KUNIT_ASSERT_TRUE(test, input_match_device_id(input_dev, &id));

	id.bustype = BUS_I2C;
	KUNIT_ASSERT_FALSE(test, input_match_device_id(input_dev, &id));

	id.flags = INPUT_DEVICE_ID_MATCH_VENDOR;
	id.vendor = 1;
	KUNIT_ASSERT_TRUE(test, input_match_device_id(input_dev, &id));

	id.vendor = 2;
	KUNIT_ASSERT_FALSE(test, input_match_device_id(input_dev, &id));

	id.flags = INPUT_DEVICE_ID_MATCH_PRODUCT;
	id.product = 1;
	KUNIT_ASSERT_TRUE(test, input_match_device_id(input_dev, &id));

	id.product = 2;
	KUNIT_ASSERT_FALSE(test, input_match_device_id(input_dev, &id));

	id.flags = INPUT_DEVICE_ID_MATCH_VERSION;
	id.version = 1;
	KUNIT_ASSERT_TRUE(test, input_match_device_id(input_dev, &id));

	id.version = 2;
	KUNIT_ASSERT_FALSE(test, input_match_device_id(input_dev, &id));

	id.flags = INPUT_DEVICE_ID_MATCH_EVBIT;
	__set_bit(EV_KEY, id.evbit);
	KUNIT_ASSERT_TRUE(test, input_match_device_id(input_dev, &id));

	__set_bit(EV_ABS, id.evbit);
	KUNIT_ASSERT_FALSE(test, input_match_device_id(input_dev, &id));
}

static void input_test_grab(struct kunit *test)
{
	struct input_dev *input_dev = test->priv;
	struct input_handle test_handle;
	struct input_handler handler;
	struct input_handle handle;
	struct input_device_id id;
	int res;

	handler.name = "handler";
	handler.id_table = &id;

	handle.dev = input_get_device(input_dev);
	handle.name = dev_name(&input_dev->dev);
	handle.handler = &handler;
	res = input_grab_device(&handle);
	KUNIT_ASSERT_TRUE(test, res == 0);

	test_handle.dev = input_get_device(input_dev);
	test_handle.name = dev_name(&input_dev->dev);
	test_handle.handler = &handler;
	res = input_grab_device(&test_handle);
	KUNIT_ASSERT_EQ(test, res, -EBUSY);

	input_release_device(&handle);
	input_put_device(input_dev);
	res = input_grab_device(&test_handle);
	KUNIT_ASSERT_TRUE(test, res == 0);
	input_put_device(input_dev);
}

struct input_seed_test {
	struct input_dev *dev;
	struct input_handler handler;
	struct input_handle handle;
	unsigned int switch_events;
	unsigned int abs_events;
	ktime_t led_timestamp;
	bool connected;
};

static void input_seed_event(struct input_handle *handle, unsigned int type,
			     unsigned int code, int value)
{
	struct input_seed_test *seed = handle->private;

	if (type == EV_SW && code == SW_DOCK)
		seed->switch_events++;
	if (type == EV_ABS && code == ABS_X)
		seed->abs_events++;
	if (type == EV_LED && code == LED_NUML)
		seed->led_timestamp = input_get_timestamp(handle->dev)[INPUT_CLK_MONO];
}

static bool input_seed_match(struct input_handler *handler,
			     struct input_dev *dev)
{
	struct input_seed_test *seed = container_of(handler, struct input_seed_test,
						  handler);

	return seed->dev == dev;
}

static int input_seed_connect(struct input_handler *handler,
			      struct input_dev *dev,
			      const struct input_device_id *id)
{
	struct input_seed_test *seed = container_of(handler, struct input_seed_test,
						  handler);
	int error;

	seed->handle.dev = dev;
	seed->handle.handler = handler;
	seed->handle.name = "input-seed-test";
	seed->handle.private = seed;
	error = input_register_handle(&seed->handle);
	if (error)
		return error;

	error = input_open_device(&seed->handle);
	if (error) {
		input_unregister_handle(&seed->handle);
		return error;
	}

	seed->connected = true;
	return 0;
}

static void input_seed_disconnect(struct input_handle *handle)
{
	input_close_device(handle);
	input_unregister_handle(handle);
}

static const struct input_device_id input_seed_ids[] = {
	{ .flags = INPUT_DEVICE_ID_MATCH_BUS, .bustype = BUS_VIRTUAL },
	{ }
};

static void input_test_seed_events(struct kunit *test, bool resize)
{
	const ktime_t seed_timestamp = ktime_set(123456, 789);
	struct input_seed_test *seed;
	int error;

	seed = kunit_kzalloc(test, sizeof(*seed), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, seed);
	seed->dev = input_allocate_device();
	KUNIT_ASSERT_NOT_NULL(test, seed->dev);

	seed->dev->name = "Input pre-registration events";
	seed->dev->id.bustype = BUS_VIRTUAL;
	input_set_capability(seed->dev, EV_SW, SW_DOCK);
	input_set_capability(seed->dev, EV_LED, LED_NUML);
	/* An ABS axis also raises the estimated event-buffer size. */
	if (resize) {
		input_set_abs_params(seed->dev, ABS_X, 0, 100, 0, 0);
		seed->dev->hint_events_per_packet = 128;
	}

	seed->handler.event = input_seed_event;
	seed->handler.match = input_seed_match;
	seed->handler.connect = input_seed_connect;
	seed->handler.disconnect = input_seed_disconnect;
	seed->handler.name = "input-seed-test";
	seed->handler.id_table = input_seed_ids;
	error = input_register_handler(&seed->handler);
	KUNIT_EXPECT_EQ(test, error, 0);
	if (error)
		goto free_device;

	/* Initialize state before registration, without a SYN_REPORT. */
	input_set_timestamp(seed->dev, seed_timestamp);
	input_report_switch(seed->dev, SW_DOCK, 1);
	if (resize)
		input_report_abs(seed->dev, ABS_X, 42);
	error = input_register_device(seed->dev);
	KUNIT_EXPECT_EQ(test, error, 0);
	if (error)
		goto unregister_handler;

	KUNIT_EXPECT_TRUE(test, seed->connected);
	if (!seed->connected)
		goto unregister_device;

	KUNIT_EXPECT_TRUE(test, test_bit(SW_DOCK, seed->dev->sw));
	if (resize)
		KUNIT_EXPECT_EQ(test, input_abs_get_val(seed->dev, ABS_X), 42);
	KUNIT_EXPECT_EQ(test, seed->switch_events, 0);
	KUNIT_EXPECT_EQ(test, seed->abs_events, 0);

	/* A later LED write must not replay the initialization events. */
	input_inject_event(&seed->handle, EV_LED, LED_NUML, 1);
	input_inject_event(&seed->handle, EV_SYN, SYN_REPORT, 0);
	KUNIT_EXPECT_EQ(test, seed->switch_events, 0);
	KUNIT_EXPECT_EQ(test, seed->abs_events, 0);
	KUNIT_EXPECT_NE(test, seed->led_timestamp, seed_timestamp);
	KUNIT_EXPECT_NE(test, seed->led_timestamp, ktime_set(0, 0));

	/* Genuine post-registration state changes must still be delivered. */
	seed->switch_events = 0;
	seed->abs_events = 0;
	input_report_switch(seed->dev, SW_DOCK, 0);
	if (resize)
		input_report_abs(seed->dev, ABS_X, 77);
	input_sync(seed->dev);
	KUNIT_EXPECT_EQ(test, seed->switch_events, 1);
	KUNIT_EXPECT_EQ(test, seed->abs_events, resize ? 1 : 0);
	KUNIT_EXPECT_FALSE(test, test_bit(SW_DOCK, seed->dev->sw));
	if (resize)
		KUNIT_EXPECT_EQ(test, input_abs_get_val(seed->dev, ABS_X), 77);
	input_inject_event(&seed->handle, EV_SYN, SYN_REPORT, 0);
	KUNIT_EXPECT_EQ(test, seed->switch_events, 1);
	KUNIT_EXPECT_EQ(test, seed->abs_events, resize ? 1 : 0);

unregister_device:
	input_unregister_device(seed->dev);
	input_unregister_handler(&seed->handler);
	return;

unregister_handler:
	input_unregister_handler(&seed->handler);
free_device:
	input_free_device(seed->dev);
}

static void input_test_seed(struct kunit *test)
{
	input_test_seed_events(test, false);
}

static void input_test_seed_resize(struct kunit *test)
{
	input_test_seed_events(test, true);
}

static struct kunit_case input_tests[] = {
	KUNIT_CASE(input_test_polling),
	KUNIT_CASE(input_test_timestamp),
	KUNIT_CASE(input_test_match_device_id),
	KUNIT_CASE(input_test_grab),
	KUNIT_CASE(input_test_seed),
	KUNIT_CASE(input_test_seed_resize),
	{ /* sentinel */ }
};

static struct kunit_suite input_test_suite = {
	.name = "input_core",
	.init = input_test_init,
	.exit = input_test_exit,
	.test_cases = input_tests,
};

kunit_test_suite(input_test_suite);

MODULE_AUTHOR("Javier Martinez Canillas <javierm@redhat.com>");
MODULE_DESCRIPTION("KUnit test for the input core");
MODULE_LICENSE("GPL");
