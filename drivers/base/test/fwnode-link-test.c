// SPDX-License-Identifier: GPL-2.0

#include <kunit/platform_device.h>
#include <kunit/test.h>

#include <linux/device.h>
#include <linux/fwnode.h>
#include <linux/platform_device.h>

#define FWNODE_LINK_TEST_DRIVER_NAME	"fwnode-link-test"

struct fwnode_link_test_context {
	struct fwnode_handle consumer;
	struct fwnode_handle container;
	struct fwnode_handle supplier_a;
	struct fwnode_handle supplier_b;
};

static int fwnode_link_test_probe(struct platform_device *pdev)
{
	return 0;
}

static struct platform_driver fwnode_link_test_driver = {
	.probe = fwnode_link_test_probe,
	.driver = {
		.name = FWNODE_LINK_TEST_DRIVER_NAME,
	},
};

static void fwnode_link_test_cleanup(void *data)
{
	struct fwnode_link_test_context *context = data;

	fwnode_links_purge(&context->consumer);
	fwnode_links_purge(&context->container);
	fwnode_links_purge(&context->supplier_a);
	fwnode_links_purge(&context->supplier_b);
}

static struct fwnode_link_test_context *
fwnode_link_test_init(struct kunit *test)
{
	struct fwnode_link_test_context *context;
	int ret;

	context = kunit_kzalloc(test, sizeof(*context), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, context);

	fwnode_init(&context->consumer, NULL);
	fwnode_init(&context->container, NULL);
	fwnode_init(&context->supplier_a, NULL);
	fwnode_init(&context->supplier_b, NULL);
	ret = kunit_add_action_or_reset(test, fwnode_link_test_cleanup,
					context);
	KUNIT_ASSERT_EQ(test, ret, 0);

	return context;
}

static struct platform_device *
fwnode_link_test_register_pdev(struct kunit *test,
			       struct fwnode_handle *fwnode)
{
	struct platform_device *pdev;
	int ret;

	pdev = kunit_platform_device_alloc(test, FWNODE_LINK_TEST_DRIVER_NAME,
					   PLATFORM_DEVID_AUTO);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, pdev);

	device_set_node(&pdev->dev, fwnode);
	ret = kunit_platform_device_add(test, pdev);
	if (ret) {
		KUNIT_FAIL(test, "failed to register platform device: %d", ret);
		return NULL;
	}

	return pdev;
}

static unsigned int fwnode_supplier_count(struct fwnode_handle *fwnode)
{
	struct fwnode_link *link;
	unsigned int count = 0;

	list_for_each_entry(link, &fwnode->suppliers, c_hook)
		count++;

	return count;
}

static struct fwnode_link *
fwnode_find_supplier(struct fwnode_handle *consumer,
		     struct fwnode_handle *supplier)
{
	struct fwnode_link *link;

	list_for_each_entry(link, &consumer->suppliers, c_hook) {
		if (link->supplier == supplier)
			return link;
	}

	return NULL;
}

static void fwnode_link_copy_suppliers_test(struct kunit *test)
{
	struct fwnode_link_test_context *context;
	struct fwnode_link *link;
	int ret;

	context = fwnode_link_test_init(test);
	KUNIT_ASSERT_NOT_NULL(test, context);

	ret = fwnode_link_add(&context->container, &context->supplier_a,
			      FWLINK_FLAG_CYCLE);
	KUNIT_ASSERT_EQ(test, ret, 0);
	ret = fwnode_link_add(&context->container, &context->supplier_b,
			      FWLINK_FLAG_IGNORE);
	KUNIT_ASSERT_EQ(test, ret, 0);

	ret = fw_devlink_copy_suppliers(&context->consumer,
					&context->container);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, fwnode_supplier_count(&context->container), 2U);
	KUNIT_EXPECT_EQ(test, fwnode_supplier_count(&context->consumer), 1U);

	link = fwnode_find_supplier(&context->consumer, &context->supplier_a);
	KUNIT_ASSERT_NOT_NULL(test, link);
	KUNIT_EXPECT_EQ(test, link->flags, (u8)0);
	link = fwnode_find_supplier(&context->consumer, &context->supplier_b);
	KUNIT_EXPECT_NULL(test, link);

	ret = fw_devlink_copy_suppliers(&context->consumer,
					&context->container);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, fwnode_supplier_count(&context->consumer), 1U);
}

static void fwnode_link_copy_before_device_add_test(struct kunit *test)
{
	struct fwnode_link_test_context *context;
	struct platform_device *consumer;
	struct platform_device *supplier;
	struct device_link *link;
	int ret;

	context = fwnode_link_test_init(test);
	KUNIT_ASSERT_NOT_NULL(test, context);

	ret = fwnode_link_add(&context->container, &context->supplier_a, 0);
	KUNIT_ASSERT_EQ(test, ret, 0);
	supplier = fwnode_link_test_register_pdev(test, &context->supplier_a);
	KUNIT_ASSERT_NOT_NULL(test, supplier);

	ret = fw_devlink_copy_suppliers(&context->consumer,
					&context->container);
	KUNIT_ASSERT_EQ(test, ret, 0);
	consumer = fwnode_link_test_register_pdev(test, &context->consumer);
	KUNIT_ASSERT_NOT_NULL(test, consumer);

	KUNIT_EXPECT_EQ(test, fwnode_supplier_count(&context->container), 1U);
	KUNIT_EXPECT_EQ(test, fwnode_supplier_count(&context->consumer), 0U);

	if (list_empty(&consumer->dev.links.suppliers)) {
		KUNIT_FAIL(test, "consumer device link was not created");
		return;
	}
	link = list_first_entry(&consumer->dev.links.suppliers,
				struct device_link, c_node);
	KUNIT_EXPECT_PTR_EQ(test, link->supplier, &supplier->dev);
	KUNIT_EXPECT_EQ(test, READ_ONCE(link->status), DL_STATE_ACTIVE);

	ret = fw_devlink_copy_suppliers(&context->consumer,
					&context->container);
	KUNIT_EXPECT_EQ(test, ret, -EBUSY);
}

static struct kunit_case fwnode_link_test_cases[] = {
	KUNIT_CASE(fwnode_link_copy_suppliers_test),
	KUNIT_CASE(fwnode_link_copy_before_device_add_test),
	{}
};

static int fwnode_link_test_suite_init(struct kunit_suite *suite)
{
	return platform_driver_register(&fwnode_link_test_driver);
}

static void fwnode_link_test_suite_exit(struct kunit_suite *suite)
{
	platform_driver_unregister(&fwnode_link_test_driver);
	device_link_wait_removal();
}

static struct kunit_suite fwnode_link_test_suite = {
	.name = "fwnode-link",
	.suite_init = fwnode_link_test_suite_init,
	.suite_exit = fwnode_link_test_suite_exit,
	.test_cases = fwnode_link_test_cases,
};

kunit_test_suite(fwnode_link_test_suite);

MODULE_DESCRIPTION("KUnit tests for firmware-node links");
MODULE_LICENSE("GPL");
