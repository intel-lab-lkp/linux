// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) Qualcomm Technologies, Inc. and/or its subsidiaries
 */

#include <linux/cleanup.h>
#include <linux/err.h>
#include <linux/fwnode.h>
#include <linux/gpio/consumer.h>
#include <linux/gpio/driver.h>
#include <linux/gpio/machine.h>
#include <linux/gpio/property.h>
#include <linux/notifier.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/types.h>

#include <kunit/fwnode.h>
#include <kunit/platform_device.h>
#include <kunit/test.h>

#define GPIO_TEST_PROVIDER		"gpio-test-provider"
#define GPIO_SWNODE_TEST_CONSUMER	"gpio-swnode-test-consumer"
#define GPIO_PROBE_ORDER_TEST_CONSUMER	"gpio-probe-order-test-consumer"
#define GPIO_PROBE_DEFER_TEST_CONSUMER	"gpio-probe-defer-test-consumer"
#define GPIO_UNBIND_TEST_CONSUMER	"gpio-unbind-test-consumer"

static int gpio_test_provider_get_direction(struct gpio_chip *gc, unsigned int offset)
{
	return GPIO_LINE_DIRECTION_OUT;
}

static int gpio_test_provider_set(struct gpio_chip *gc, unsigned int offset, int value)
{
	return 0;
}

static int gpio_test_provider_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct gpio_chip *gc;

	gc = devm_kzalloc(dev, sizeof(*gc), GFP_KERNEL);
	if (!gc)
		return -ENOMEM;

	gc->base = -1;
	gc->ngpio = 4;
	gc->label = "gpio-swnode-consumer-test-device";
	gc->parent = dev;
	gc->owner = THIS_MODULE;

	gc->get_direction = gpio_test_provider_get_direction;
	gc->set = gpio_test_provider_set;

	return devm_gpiochip_add_data(dev, gc, NULL);
}

static struct platform_driver gpio_test_provider_driver = {
	.probe = gpio_test_provider_probe,
	.driver = {
		.name = GPIO_TEST_PROVIDER,
	},
};

static const struct software_node gpio_test_provider_swnode = {
	.name = "gpio-test-provider-primary",
};

struct gpio_swnode_consumer_pdata {
	bool gpio_ok;
};

static const struct gpio_swnode_consumer_pdata gpio_swnode_pdata_template = {
	.gpio_ok = false,
};

static int gpio_swnode_consumer_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct gpio_swnode_consumer_pdata *pdata = dev_get_platdata(dev);
	struct gpio_desc *desc;

	desc = devm_gpiod_get(dev, "foo", GPIOD_OUT_HIGH);
	if (IS_ERR(desc))
		return PTR_ERR(desc);

	pdata->gpio_ok = true;

	return 0;
}

static struct platform_driver gpio_swnode_consumer_driver = {
	.probe = gpio_swnode_consumer_probe,
	.driver = {
		.name = GPIO_SWNODE_TEST_CONSUMER,
	},
};

static void gpio_swnode_lookup_by_primary(struct kunit *test)
{
	struct gpio_swnode_consumer_pdata *pdata;
	struct platform_device_info pdevinfo;
	struct property_entry properties[2];
	struct platform_device *pdev;
	bool bound = false;
	int ret;

	ret = kunit_platform_driver_register(test, &gpio_test_provider_driver);
	KUNIT_ASSERT_EQ(test, ret, 0);

	ret = kunit_platform_driver_register(test, &gpio_swnode_consumer_driver);
	KUNIT_ASSERT_EQ(test, ret, 0);

	pdevinfo = (struct platform_device_info){
		.name = GPIO_TEST_PROVIDER,
		.id = PLATFORM_DEVID_NONE,
		.swnode = &gpio_test_provider_swnode,
	};

	pdev = kunit_platform_device_register_full(test, &pdevinfo);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, pdev);

	properties[0] = PROPERTY_ENTRY_GPIO("foo-gpios",
					    &gpio_test_provider_swnode,
					    0, GPIO_ACTIVE_HIGH);
	properties[1] = (struct property_entry){ };

	pdevinfo = (struct platform_device_info){
		.name = GPIO_SWNODE_TEST_CONSUMER,
		.id = PLATFORM_DEVID_NONE,
		.data = &gpio_swnode_pdata_template,
		.size_data = sizeof(gpio_swnode_pdata_template),
		.properties = properties,
	};

	pdev = kunit_platform_device_register_full(test, &pdevinfo);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, pdev);

	wait_for_device_probe();
	scoped_guard(device, &pdev->dev)
		bound = device_is_bound(&pdev->dev);

	KUNIT_ASSERT_TRUE(test, bound);

	pdata = dev_get_platdata(&pdev->dev);
	KUNIT_ASSERT_TRUE(test, pdata->gpio_ok);
}

static void gpio_swnode_lookup_by_secondary(struct kunit *test)
{
	struct gpio_swnode_consumer_pdata *pdata;
	struct platform_device_info pdevinfo;
	struct property_entry properties[2];
	struct fwnode_handle *primary;
	struct platform_device *pdev;
	bool bound = false;
	int ret;

	/*
	 * Can't live on the stack as it will still get referenced in cleanup
	 * path after this function returns.
	 */
	primary = kunit_kzalloc(test, sizeof(*primary), GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, primary);

	ret = kunit_platform_driver_register(test, &gpio_test_provider_driver);
	KUNIT_ASSERT_EQ(test, ret, 0);

	ret = kunit_platform_driver_register(test, &gpio_swnode_consumer_driver);
	KUNIT_ASSERT_EQ(test, ret, 0);

	fwnode_init(primary, NULL);

	pdevinfo = (struct platform_device_info){
		.name = GPIO_TEST_PROVIDER,
		.id = PLATFORM_DEVID_NONE,
		.fwnode = primary,
		.swnode = &gpio_test_provider_swnode,
	};

	pdev = kunit_platform_device_register_full(test, &pdevinfo);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, pdev);

	properties[0] = PROPERTY_ENTRY_GPIO("foo-gpios",
					    &gpio_test_provider_swnode,
					    0, GPIO_ACTIVE_HIGH);
	properties[1] = (struct property_entry){ };

	pdevinfo = (struct platform_device_info){
		.name = GPIO_SWNODE_TEST_CONSUMER,
		.id = PLATFORM_DEVID_NONE,
		.data = &gpio_swnode_pdata_template,
		.size_data = sizeof(gpio_swnode_pdata_template),
		.properties = properties,
	};

	pdev = kunit_platform_device_register_full(test, &pdevinfo);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, pdev);

	wait_for_device_probe();
	scoped_guard(device, &pdev->dev)
		bound = device_is_bound(&pdev->dev);

	KUNIT_ASSERT_TRUE(test, bound);

	pdata = dev_get_platdata(&pdev->dev);
	KUNIT_ASSERT_TRUE(test, pdata->gpio_ok);
}

static struct kunit_case gpio_swnode_lookup_tests[] = {
	KUNIT_CASE(gpio_swnode_lookup_by_primary),
	KUNIT_CASE(gpio_swnode_lookup_by_secondary),
	{ }
};

static struct kunit_suite gpio_swnode_lookup_test_suite = {
	.name = "gpio-swnode-lookup",
	.test_cases = gpio_swnode_lookup_tests,
};

static void gpio_swnode_unregister_swnode(void *data)
{
	software_node_unregister(data);
}

struct gpio_probe_order_pdata {
	unsigned int probe_count;
	bool gpio_ok;
};

static const struct gpio_probe_order_pdata gpio_probe_order_pdata_template = {
	.probe_count = 0,
	.gpio_ok = false,
};

static int gpio_probe_order_consumer_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct gpio_probe_order_pdata *pdata = dev_get_platdata(dev);
	struct gpio_desc *desc;

	pdata->probe_count++;

	desc = devm_gpiod_get(dev, "foo", GPIOD_OUT_HIGH);
	if (IS_ERR(desc))
		return PTR_ERR(desc);

	pdata->gpio_ok = true;

	return 0;
}

static struct platform_driver gpio_probe_order_consumer_driver = {
	.probe = gpio_probe_order_consumer_probe,
	.driver = {
		.name = GPIO_PROBE_ORDER_TEST_CONSUMER,
	},
};

/*
 * Verify that fw_devlink orders the probe of a GPIO consumer after its
 * provider. The consumer references the provider through a software node and
 * is registered first. fw_devlink must defer it before its driver's probe()
 * is ever entered, so the consumer probes exactly once - only after the
 * provider is added and bound.
 */
static void gpio_swnode_probe_order(struct kunit *test)
{
	struct property_entry properties[2] = { };
	struct gpio_probe_order_pdata *pdata;
	struct platform_device_info pdevinfo;
	struct platform_device *prvd, *cons;
	bool bound = false;
	int ret;

	ret = kunit_platform_driver_register(test, &gpio_test_provider_driver);
	KUNIT_ASSERT_EQ(test, ret, 0);

	ret = kunit_platform_driver_register(test, &gpio_probe_order_consumer_driver);
	KUNIT_ASSERT_EQ(test, ret, 0);

	ret = software_node_register(&gpio_test_provider_swnode);
	KUNIT_ASSERT_EQ(test, ret, 0);

	ret = kunit_add_action_or_reset(test, gpio_swnode_unregister_swnode,
					(void *)&gpio_test_provider_swnode);
	KUNIT_ASSERT_EQ(test, ret, 0);

	properties[0] = PROPERTY_ENTRY_GPIO("foo-gpios",
					    &gpio_test_provider_swnode,
					    0, GPIO_ACTIVE_HIGH);

	pdevinfo = (struct platform_device_info){
		.name = GPIO_PROBE_ORDER_TEST_CONSUMER,
		.id = PLATFORM_DEVID_NONE,
		.data = &gpio_probe_order_pdata_template,
		.size_data = sizeof(gpio_probe_order_pdata_template),
		.properties = properties,
	};

	cons = kunit_platform_device_register_full(test, &pdevinfo);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, cons);

	wait_for_device_probe();
	scoped_guard(device, &cons->dev)
		bound = device_is_bound(&cons->dev);

	KUNIT_ASSERT_FALSE(test, bound);

	pdata = dev_get_platdata(&cons->dev);
	KUNIT_ASSERT_EQ(test, pdata->probe_count, 0);
	KUNIT_ASSERT_FALSE(test, pdata->gpio_ok);

	pdevinfo = (struct platform_device_info){
		.name = GPIO_TEST_PROVIDER,
		.id = PLATFORM_DEVID_NONE,
		.swnode = &gpio_test_provider_swnode,
	};

	prvd = kunit_platform_device_register_full(test, &pdevinfo);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, prvd);

	wait_for_device_probe();

	scoped_guard(device, &prvd->dev)
		bound = device_is_bound(&prvd->dev);
	KUNIT_ASSERT_TRUE(test, bound);

	scoped_guard(device, &cons->dev)
		bound = device_is_bound(&cons->dev);
	KUNIT_ASSERT_TRUE(test, bound);

	pdata = dev_get_platdata(&cons->dev);
	KUNIT_ASSERT_EQ(test, pdata->probe_count, 1);
	KUNIT_ASSERT_TRUE(test, pdata->gpio_ok);
}

struct gpio_probe_defer_pdata {
	unsigned int probe_count;
	int gpio_err;
};

static const struct gpio_probe_defer_pdata gpio_probe_defer_pdata_template = {
	.probe_count = 0,
	.gpio_err = 0,
};

static int gpio_probe_defer_consumer_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct gpio_probe_defer_pdata *pdata = dev_get_platdata(dev);
	struct gpio_desc *desc;

	pdata->probe_count++;

	desc = devm_gpiod_get(dev, "foo", GPIOD_OUT_HIGH);
	if (IS_ERR(desc)) {
		pdata->gpio_err = PTR_ERR(desc);
		return pdata->gpio_err;
	}

	pdata->gpio_err = 0;

	return 0;
}

static struct platform_driver gpio_probe_defer_consumer_driver = {
	.probe = gpio_probe_defer_consumer_probe,
	.driver = {
		.name = GPIO_PROBE_DEFER_TEST_CONSUMER,
	},
};

/*
 * Verify that a GPIO consumer referencing a provider whose software node is
 * not registered yet, defers its probe instead of failing.
 *
 * The provider software node is deliberately left unregistered when the
 * consumer is added. fw_devlink cannot resolve the reference, so it creates no
 * supplier link and does not order the consumer - the consumer's probe() runs
 * and reaches devm_gpiod_get(). The swnode GPIO lookup returns -ENOTCONN for a
 * reference to an unregistered node, which gpiolib maps to -EPROBE_DEFER. Once
 * the provider software node and device appear, the deferred consumer probes
 * again and binds.
 */
static void gpio_swnode_probe_defer_on_unregistered(struct kunit *test)
{
	struct property_entry properties[2] = { };
	struct gpio_probe_defer_pdata *pdata;
	struct platform_device_info pdevinfo;
	struct platform_device *prvd, *cons;
	struct fwnode_handle *fwnode;
	bool bound = false;
	int ret;

	ret = kunit_platform_driver_register(test, &gpio_test_provider_driver);
	KUNIT_ASSERT_EQ(test, ret, 0);

	ret = kunit_platform_driver_register(test, &gpio_probe_defer_consumer_driver);
	KUNIT_ASSERT_EQ(test, ret, 0);

	properties[0] = PROPERTY_ENTRY_GPIO("foo-gpios",
					    &gpio_test_provider_swnode,
					    0, GPIO_ACTIVE_HIGH);

	pdevinfo = (struct platform_device_info){
		.name = GPIO_PROBE_DEFER_TEST_CONSUMER,
		.id = PLATFORM_DEVID_NONE,
		.data = &gpio_probe_defer_pdata_template,
		.size_data = sizeof(gpio_probe_defer_pdata_template),
		.properties = properties,
	};

	cons = kunit_platform_device_register_full(test, &pdevinfo);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, cons);

	wait_for_device_probe();
	scoped_guard(device, &cons->dev)
		bound = device_is_bound(&cons->dev);

	KUNIT_ASSERT_FALSE(test, bound);

	pdata = dev_get_platdata(&cons->dev);
	KUNIT_ASSERT_GT(test, pdata->probe_count, 0);
	KUNIT_ASSERT_EQ(test, pdata->gpio_err, -EPROBE_DEFER);

	fwnode = kunit_software_node_register(test, &gpio_test_provider_swnode);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, fwnode);

	pdevinfo = (struct platform_device_info){
		.name = GPIO_TEST_PROVIDER,
		.id = PLATFORM_DEVID_NONE,
		.swnode = &gpio_test_provider_swnode,
	};

	prvd = kunit_platform_device_register_full(test, &pdevinfo);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, prvd);

	wait_for_device_probe();

	scoped_guard(device, &prvd->dev)
		bound = device_is_bound(&prvd->dev);
	KUNIT_ASSERT_TRUE(test, bound);

	scoped_guard(device, &cons->dev)
		bound = device_is_bound(&cons->dev);
	KUNIT_ASSERT_TRUE(test, bound);

	pdata = dev_get_platdata(&cons->dev);
	KUNIT_ASSERT_EQ(test, pdata->gpio_err, 0);
}

static int gpio_swnode_probe_order_test_init(struct kunit *test)
{
	/*
	 * A prior test may have left a managed device link teardown queued on
	 * the device_link_mq. Flush it so that software_node_register()
	 * doesn't spuriously see the node as registered and fail with -EEXIST.
	 */
	device_link_wait_removal();

	return 0;
}

static struct kunit_case gpio_swnode_probe_order_tests[] = {
	KUNIT_CASE(gpio_swnode_probe_order),
	KUNIT_CASE(gpio_swnode_probe_defer_on_unregistered),
	{ }
};

static struct kunit_suite gpio_swnode_probe_order_test_suite = {
	.name = "gpio-swnode-probe-order",
	.test_cases = gpio_swnode_probe_order_tests,
	.init = gpio_swnode_probe_order_test_init,
};

static BLOCKING_NOTIFIER_HEAD(gpio_unbind_notifier);

struct gpio_unbind_consumer_drvdata {
	struct device *dev;
	struct gpio_desc *desc;
	struct notifier_block nb;
	int set_retval;
};

static int gpio_unbind_notify(struct notifier_block *nb, unsigned long action,
			      void *data)
{
	struct gpio_unbind_consumer_drvdata *drvdata =
		container_of(nb, struct gpio_unbind_consumer_drvdata, nb);
	struct device *dev = data;

	if (dev != drvdata->dev)
		return NOTIFY_DONE;

	drvdata->set_retval = gpiod_set_value_cansleep(drvdata->desc, 0);

	return NOTIFY_OK;
}

static void gpio_unbind_unregister_notifier(void *data)
{
	struct notifier_block *nb = data;

	blocking_notifier_chain_unregister(&gpio_unbind_notifier, nb);
}

static int gpio_unbind_consumer_probe(struct platform_device *pdev)
{
	struct gpio_unbind_consumer_drvdata *data;
	struct device *dev = &pdev->dev;
	int ret;

	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->dev = dev;

	data->desc = devm_gpiod_get(dev, "foo", GPIOD_OUT_HIGH);
	if (IS_ERR(data->desc))
		return PTR_ERR(data->desc);

	data->nb.notifier_call = gpio_unbind_notify;
	ret = blocking_notifier_chain_register(&gpio_unbind_notifier, &data->nb);
	if (ret)
		return ret;

	ret = devm_add_action_or_reset(dev, gpio_unbind_unregister_notifier, &data->nb);
	if (ret)
		return ret;

	platform_set_drvdata(pdev, data);

	return 0;
}

static struct platform_driver gpio_unbind_consumer_driver = {
	.probe = gpio_unbind_consumer_probe,
	.driver = {
		.name = GPIO_UNBIND_TEST_CONSUMER,
	},
};

static void gpio_unbind_with_consumers(struct kunit *test)
{
	struct gpio_unbind_consumer_drvdata *cons_data;
	struct platform_device_info pdevinfo;
	struct property_entry properties[2];
	struct platform_device *prvd, *cons;
	bool bound = false;
	int ret;

	ret = kunit_platform_driver_register(test, &gpio_test_provider_driver);
	KUNIT_ASSERT_EQ(test, ret, 0);

	ret = kunit_platform_driver_register(test, &gpio_unbind_consumer_driver);
	KUNIT_ASSERT_EQ(test, ret, 0);

	pdevinfo = (struct platform_device_info){
		.name = GPIO_TEST_PROVIDER,
		.id = PLATFORM_DEVID_NONE,
		.swnode = &gpio_test_provider_swnode,
	};

	prvd = kunit_platform_device_register_full(test, &pdevinfo);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, prvd);

	properties[0] = PROPERTY_ENTRY_GPIO("foo-gpios",
					    &gpio_test_provider_swnode,
					    0, GPIO_ACTIVE_HIGH);
	properties[1] = (struct property_entry){ };

	/*
	 * This test deliberately keeps the consumer bound while the provider
	 * is unregistered. fw_devlink would force-unbind the consumer before
	 * the provider so use the FWNODE_FLAG_LINKS_ADDED flag to opt out of
	 * it as a workaround.
	 */
	cons = kunit_platform_device_alloc(test, GPIO_UNBIND_TEST_CONSUMER,
					   PLATFORM_DEVID_NONE);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, cons);

	ret = device_create_managed_software_node(&cons->dev, properties, NULL);
	KUNIT_ASSERT_EQ(test, ret, 0);

	fwnode_set_flag(dev_fwnode(&cons->dev), FWNODE_FLAG_LINKS_ADDED);

	ret = kunit_platform_device_add(test, cons);
	KUNIT_ASSERT_EQ(test, ret, 0);

	wait_for_device_probe();
	scoped_guard(device, &cons->dev)
		bound = device_is_bound(&cons->dev);

	KUNIT_ASSERT_TRUE(test, bound);

	kunit_platform_device_unregister(test, prvd);

	ret = blocking_notifier_call_chain(&gpio_unbind_notifier, 0, &cons->dev);
	KUNIT_ASSERT_EQ(test, ret, NOTIFY_OK);

	scoped_guard(device, &cons->dev) {
		cons_data = platform_get_drvdata(cons);
		ret = cons_data->set_retval;
	}

	KUNIT_ASSERT_EQ(test, ret, -ENODEV);
}

static struct kunit_case gpio_unbind_with_consumers_tests[] = {
	KUNIT_CASE(gpio_unbind_with_consumers),
	{ }
};

static struct kunit_suite gpio_unbind_with_consumers_test_suite = {
	.name = "gpio-unbind-with-consumers",
	.test_cases = gpio_unbind_with_consumers_tests,
};

kunit_test_suites(
	&gpio_swnode_lookup_test_suite,
	&gpio_swnode_probe_order_test_suite,
	&gpio_unbind_with_consumers_test_suite,
);

MODULE_DESCRIPTION("Test module for the GPIO subsystem");
MODULE_AUTHOR("Bartosz Golaszewski <bartosz.golaszewski@oss.qualcomm.com>");
MODULE_LICENSE("GPL");
