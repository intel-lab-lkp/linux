// SPDX-License-Identifier: GPL-2.0

#include <kunit/platform_device.h>
#include <kunit/resource.h>
#include <kunit/test.h>

#include <linux/completion.h>
#include <linux/device.h>
#include <linux/device/driver.h>
#include <linux/fwnode.h>
#include <linux/jiffies.h>
#include <linux/kernfs.h>
#include <linux/kthread.h>
#include <linux/platform_device.h>
#include <linux/sched/task.h>
#include <linux/slab.h>
#include <linux/sysfs.h>

#define TEST_CLASS_NAME		"device-link-test"
#define TEST_DRIVER_NAME	"device-link-test-consumer"

enum test_class_action {
	TEST_CLASS_NONE,
	TEST_CLASS_SUPPLIER,
	TEST_CLASS_SYNC_CONSUMER,
	TEST_CLASS_OBSERVE,
	TEST_CLASS_WAIT,
	TEST_CLASS_UNBIND_RACE,
};

struct device_link_class_context {
	enum test_class_action action;
	struct device *endpoint;
	struct device_link *link;
	struct completion waiter_started;
	struct completion waiter_done;
	struct task_struct *waiter;
	struct completion unbind_started;
	struct completion unbind_done;
	struct task_struct *unbinder;
	bool waiter_completed_during_add;
	bool unbind_completed_during_add;
	bool waiting_for_supplier_seen;
};

struct test_class_device {
	struct device dev;
	struct fwnode_handle fwnode;
	struct fwnode_handle missing_supplier;
	bool added;
};

static struct class *test_class;
static struct device_link_class_context *test_context;
static atomic_t remove_count;
static atomic_t sync_count;

static int test_driver_probe(struct platform_device *pdev)
{
	return 0;
}

static void test_driver_remove(struct platform_device *pdev)
{
	atomic_inc(&remove_count);
}

static void test_driver_sync_state(struct device *dev)
{
	atomic_inc(&sync_count);
}

static struct platform_driver test_driver = {
	.probe = test_driver_probe,
	.remove = test_driver_remove,
	.driver = {
		.name = TEST_DRIVER_NAME,
		.sync_state = test_driver_sync_state,
	},
};

static bool test_has_waiting_for_supplier(struct device *dev)
{
	struct kernfs_node *kn;

	kn = sysfs_get_dirent(dev->kobj.sd, "waiting_for_supplier");
	if (kn)
		kernfs_put(kn);

	return !!kn;
}

static int test_wait_for_probe(void *data)
{
	struct device_link_class_context *context = data;

	complete(&context->waiter_started);
	wait_for_device_probe();
	complete(&context->waiter_done);

	return 0;
}

static int test_unbind_supplier(void *data)
{
	struct device_link_class_context *context = data;

	complete(&context->unbind_started);
	device_release_driver(context->endpoint);
	complete(&context->unbind_done);

	return 0;
}

static int test_class_add(struct device *dev)
{
	struct device_link_class_context *context = test_context;

	if (!context)
		return 0;

	switch (context->action) {
	case TEST_CLASS_SUPPLIER:
		context->link = device_link_add(context->endpoint, dev,
						DL_FLAG_INFERRED |
						DL_FLAG_AUTOPROBE_CONSUMER);
		return context->link ? 0 : -ENOMEM;
	case TEST_CLASS_SYNC_CONSUMER:
		context->link = device_link_add(dev, context->endpoint,
						DL_FLAG_INFERRED |
						DL_FLAG_SYNC_STATE_ONLY);
		return context->link ? 0 : -ENOMEM;
	case TEST_CLASS_OBSERVE:
		context->waiting_for_supplier_seen =
			test_has_waiting_for_supplier(dev);
		return 0;
	case TEST_CLASS_WAIT:
		context->waiter = kthread_create(test_wait_for_probe, context,
						 "device-link-wait");
		if (IS_ERR(context->waiter))
			return PTR_ERR(context->waiter);
		get_task_struct(context->waiter);
		wake_up_process(context->waiter);
		wait_for_completion(&context->waiter_started);
		context->waiter_completed_during_add =
			wait_for_completion_timeout(&context->waiter_done,
						    msecs_to_jiffies(50));
		return 0;
	case TEST_CLASS_UNBIND_RACE:
		context->link = device_link_add(dev, context->endpoint, 0);
		if (!context->link)
			return -ENOMEM;
		context->unbinder = kthread_create(test_unbind_supplier, context,
						   "device-link-unbind");
		if (IS_ERR(context->unbinder))
			return PTR_ERR(context->unbinder);
		get_task_struct(context->unbinder);
		wake_up_process(context->unbinder);
		wait_for_completion(&context->unbind_started);
		context->unbind_completed_during_add =
			wait_for_completion_timeout(&context->unbind_done,
						    msecs_to_jiffies(50));
		return 0;
	case TEST_CLASS_NONE:
		return 0;
	}

	return -EINVAL;
}

static struct class_interface test_class_interface = {
	.add_dev = test_class_add,
};

static void test_unregister_device(void *data)
{
	device_unregister(data);
}

static void test_resume_sync_state(void *data)
{
	device_links_supplier_sync_state_resume();
}

static struct platform_device *
test_register_platform_device(struct kunit *test)
{
	struct platform_device_info info = {
		.name = TEST_DRIVER_NAME,
		.id = PLATFORM_DEVID_AUTO,
	};
	struct platform_device *pdev;

	pdev = kunit_platform_device_register_full(test, &info);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, pdev);
	KUNIT_ASSERT_EQ(test, pdev->dev.links.status, DL_DEV_DRIVER_BOUND);

	return pdev;
}

static void test_class_device_release(struct device *dev)
{
	struct test_class_device *tdev =
		container_of(dev, struct test_class_device, dev);

	fwnode_links_purge(&tdev->fwnode);
	fwnode_links_purge(&tdev->missing_supplier);
	kfree(tdev);
}

static void test_class_device_cleanup(void *data)
{
	struct test_class_device *tdev = data;

	if (tdev->added)
		device_unregister(&tdev->dev);
	else
		put_device(&tdev->dev);
}

static struct test_class_device *
test_class_device_alloc(struct kunit *test, const char *name)
{
	struct test_class_device *tdev;
	int ret;

	tdev = kzalloc_obj(*tdev);
	KUNIT_ASSERT_NOT_NULL(test, tdev);

	device_initialize(&tdev->dev);
	fwnode_init(&tdev->fwnode, NULL);
	fwnode_init(&tdev->missing_supplier, NULL);
	tdev->dev.class = test_class;
	tdev->dev.release = test_class_device_release;
	device_set_node(&tdev->dev, &tdev->fwnode);
	ret = dev_set_name(&tdev->dev, "%s", name);
	if (ret) {
		put_device(&tdev->dev);
		KUNIT_FAIL(test, "failed to name class device: %d", ret);
		return NULL;
	}

	ret = kunit_add_action_or_reset(test, test_class_device_cleanup, tdev);
	KUNIT_ASSERT_EQ(test, ret, 0);

	return tdev;
}

static void device_link_class_supplier_test(struct kunit *test)
{
	struct device_link_class_context *context = test->priv;
	struct platform_device *consumer;
	struct device *supplier;
	int ret;

	consumer = test_register_platform_device(test);
	context->action = TEST_CLASS_SUPPLIER;
	context->endpoint = &consumer->dev;

	supplier = device_create(test_class, NULL, 0, NULL,
				 TEST_CLASS_NAME "-supplier");
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, supplier);
	ret = kunit_add_action_or_reset(test, test_unregister_device, supplier);
	KUNIT_ASSERT_EQ(test, ret, 0);

	KUNIT_ASSERT_NOT_NULL(test, context->link);
	KUNIT_EXPECT_EQ(test, supplier->links.status, DL_DEV_DRIVER_BOUND);
	KUNIT_EXPECT_EQ(test, READ_ONCE(context->link->status), DL_STATE_ACTIVE);
	KUNIT_EXPECT_TRUE(test, device_link_test(context->link,
						 DL_FLAG_AUTOPROBE_CONSUMER));
	KUNIT_EXPECT_FALSE(test, device_link_test(context->link,
						  DL_FLAG_SYNC_STATE_ONLY));

	kunit_release_action(test, test_unregister_device, supplier);
	device_link_wait_removal();

	KUNIT_EXPECT_EQ(test, atomic_read(&remove_count), 1);
	KUNIT_EXPECT_EQ(test, consumer->dev.links.status, DL_DEV_NO_DRIVER);
}

static void device_link_class_consumer_sync_test(struct kunit *test)
{
	struct device_link_class_context *context = test->priv;
	struct platform_device *supplier;
	struct device *consumer;
	int ret;

	device_links_supplier_sync_state_pause();
	ret = kunit_add_action_or_reset(test, test_resume_sync_state, NULL);
	KUNIT_ASSERT_EQ(test, ret, 0);

	supplier = test_register_platform_device(test);
	context->action = TEST_CLASS_SYNC_CONSUMER;
	context->endpoint = &supplier->dev;

	consumer = device_create(test_class, NULL, 0, NULL,
				 TEST_CLASS_NAME "-consumer");
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, consumer);
	ret = kunit_add_action_or_reset(test, test_unregister_device, consumer);
	KUNIT_ASSERT_EQ(test, ret, 0);

	KUNIT_ASSERT_NOT_NULL(test, context->link);
	KUNIT_EXPECT_EQ(test, consumer->links.status, DL_DEV_DRIVER_BOUND);
	KUNIT_EXPECT_TRUE(test, list_empty(&consumer->links.suppliers));
	kunit_release_action(test, test_resume_sync_state, NULL);
	KUNIT_EXPECT_EQ(test, atomic_read(&sync_count), 1);
}

static void device_link_class_existing_consumer_link_test(struct kunit *test)
{
	struct device_link_class_context *context = test->priv;
	struct test_class_device *consumer;
	struct platform_device *supplier;
	struct device_link *link;
	int ret;

	supplier = test_register_platform_device(test);
	consumer = test_class_device_alloc(test,
					   TEST_CLASS_NAME "-existing-link");
	KUNIT_ASSERT_NOT_NULL(test, consumer);

	link = device_link_add(&consumer->dev, &supplier->dev,
			       DL_FLAG_AUTOREMOVE_CONSUMER);
	KUNIT_ASSERT_NOT_NULL(test, link);
	KUNIT_ASSERT_EQ(test, READ_ONCE(link->status), DL_STATE_AVAILABLE);
	ret = fwnode_link_add(&consumer->fwnode,
			      &consumer->missing_supplier, 0);
	KUNIT_ASSERT_EQ(test, ret, 0);

	context->action = TEST_CLASS_OBSERVE;
	ret = device_add(&consumer->dev);
	KUNIT_ASSERT_EQ(test, ret, 0);
	consumer->added = true;

	KUNIT_EXPECT_EQ(test, consumer->dev.links.status, DL_DEV_DRIVER_BOUND);
	KUNIT_EXPECT_EQ(test, READ_ONCE(link->status), DL_STATE_ACTIVE);
	KUNIT_EXPECT_TRUE(test, context->waiting_for_supplier_seen);
	KUNIT_EXPECT_FALSE(test, test_has_waiting_for_supplier(&consumer->dev));
}

static void device_link_class_registration_wait_test(struct kunit *test)
{
	struct device_link_class_context *context = test->priv;
	struct device *dev;
	unsigned long timeout;
	int ret;

	wait_for_device_probe();
	context->action = TEST_CLASS_WAIT;
	dev = device_create(test_class, NULL, 0, NULL,
			    TEST_CLASS_NAME "-wait");
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, dev);
	ret = kunit_add_action_or_reset(test, test_unregister_device, dev);
	KUNIT_ASSERT_EQ(test, ret, 0);

	KUNIT_EXPECT_FALSE(test, context->waiter_completed_during_add);
	timeout = wait_for_completion_timeout(&context->waiter_done,
					      msecs_to_jiffies(1000));
	KUNIT_EXPECT_GT(test, timeout, 0UL);
}

static void device_link_class_supplier_unbind_race_test(struct kunit *test)
{
	struct device_link_class_context *context = test->priv;
	struct platform_device *supplier;
	struct device *consumer;
	unsigned long timeout;
	int ret;

	supplier = test_register_platform_device(test);
	context->action = TEST_CLASS_UNBIND_RACE;
	context->endpoint = &supplier->dev;
	consumer = device_create(test_class, NULL, 0, NULL,
				 TEST_CLASS_NAME "-unbind-race");
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, consumer);
	ret = kunit_add_action_or_reset(test, test_unregister_device, consumer);
	KUNIT_ASSERT_EQ(test, ret, 0);

	KUNIT_ASSERT_NOT_NULL(test, context->link);
	KUNIT_EXPECT_FALSE(test, context->unbind_completed_during_add);
	timeout = wait_for_completion_timeout(&context->unbind_done,
					      msecs_to_jiffies(1000));
	KUNIT_EXPECT_GT(test, timeout, 0UL);
	KUNIT_EXPECT_EQ(test, atomic_read(&remove_count), 1);
	KUNIT_EXPECT_EQ(test, supplier->dev.links.status, DL_DEV_NO_DRIVER);
}

static int device_link_class_test_init(struct kunit *test)
{
	struct device_link_class_context *context;

	context = kunit_kzalloc(test, sizeof(*context), GFP_KERNEL);
	if (!context)
		return -ENOMEM;

	init_completion(&context->waiter_started);
	init_completion(&context->waiter_done);
	init_completion(&context->unbind_started);
	init_completion(&context->unbind_done);
	test->priv = context;
	test_context = context;
	atomic_set(&remove_count, 0);
	atomic_set(&sync_count, 0);

	return 0;
}

static void device_link_class_test_exit(struct kunit *test)
{
	struct device_link_class_context *context = test->priv;

	if (context->waiter && !IS_ERR(context->waiter)) {
		kthread_stop(context->waiter);
		put_task_struct(context->waiter);
	}
	if (context->unbinder && !IS_ERR(context->unbinder)) {
		kthread_stop(context->unbinder);
		put_task_struct(context->unbinder);
	}
	test_context = NULL;
}

static int device_link_class_suite_init(struct kunit_suite *suite)
{
	int ret;

	test_class = class_create(TEST_CLASS_NAME);
	if (IS_ERR(test_class))
		return PTR_ERR(test_class);

	ret = platform_driver_register(&test_driver);
	if (ret)
		goto destroy_class;

	test_class_interface.class = test_class;
	ret = class_interface_register(&test_class_interface);
	if (ret)
		goto unregister_driver;

	return 0;

unregister_driver:
	platform_driver_unregister(&test_driver);
destroy_class:
	class_destroy(test_class);

	return ret;
}

static void device_link_class_suite_exit(struct kunit_suite *suite)
{
	class_interface_unregister(&test_class_interface);
	platform_driver_unregister(&test_driver);
	class_destroy(test_class);
	device_link_wait_removal();
}

static struct kunit_case device_link_class_test_cases[] = {
	KUNIT_CASE(device_link_class_supplier_test),
	KUNIT_CASE(device_link_class_consumer_sync_test),
	KUNIT_CASE(device_link_class_existing_consumer_link_test),
	KUNIT_CASE(device_link_class_registration_wait_test),
	KUNIT_CASE(device_link_class_supplier_unbind_race_test),
	{}
};

static struct kunit_suite device_link_class_test_suite = {
	.name = "device-link-class",
	.suite_init = device_link_class_suite_init,
	.suite_exit = device_link_class_suite_exit,
	.init = device_link_class_test_init,
	.exit = device_link_class_test_exit,
	.test_cases = device_link_class_test_cases,
};

kunit_test_suite(device_link_class_test_suite);

MODULE_DESCRIPTION("KUnit tests for class device links");
MODULE_LICENSE("GPL");
