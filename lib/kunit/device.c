// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit basic device implementation
 *
 * Implementation of struct kunit_device helpers.
 *
 * Copyright (C) 2023, Google LLC.
 * Author: David Gow <davidgow@google.com>
 */

#include <linux/device.h>

#include <kunit/test.h>
#include <kunit/device.h>
#include <kunit/resource.h>


/* Wrappers for use with kunit_add_action() */
KUNIT_DEFINE_ACTION_WRAPPER(device_unregister_wrapper, device_unregister, struct device *);
KUNIT_DEFINE_ACTION_WRAPPER(driver_unregister_wrapper, driver_unregister, struct device_driver *);

static struct device kunit_bus = {
	.init_name = "kunit"
};

/* A device owned by a KUnit test. */
struct kunit_device {
	struct device dev;
	struct kunit *owner;
	/* Force binding to a specific driver. */
	struct device_driver *driver;
	/* The driver is managed by KUnit and unique to this device. */
	bool cleanup_driver;
};

static inline struct kunit_device *to_kunit_device(struct device *d)
{
	return container_of(d, struct kunit_device, dev);
}

static int kunit_bus_match(struct device *dev, struct device_driver *driver)
{
	struct kunit_device *kunit_dev = to_kunit_device(dev);

	if (kunit_dev->driver == driver)
		return 1;

	return 0;
}

static struct bus_type kunit_bus_type = {
	.name		= "kunit",
	.match		= kunit_bus_match
};

int kunit_bus_init(void)
{
	int error;

	error = bus_register(&kunit_bus_type);
	if (!error) {
		error = device_register(&kunit_bus);
		if (error)
			bus_unregister(&kunit_bus_type);
	}
	return error;
}
late_initcall(kunit_bus_init);

static void kunit_device_release(struct device *d)
{
	kfree(to_kunit_device(d));
}

struct device_driver *kunit_driver_create(struct kunit *test, const char *name)
{
	struct device_driver *driver;
	int err = -ENOMEM;

	driver = kunit_kzalloc(test, sizeof(*driver), GFP_KERNEL);

	if (!driver)
		return ERR_PTR(err);

	driver->name = name;
	driver->bus = &kunit_bus_type;
	driver->owner = THIS_MODULE;

	err = driver_register(driver);
	if (err) {
		kunit_kfree(test, driver);
		return ERR_PTR(err);
	}

	kunit_add_action(test, driver_unregister_wrapper, driver);
	return driver;
}
EXPORT_SYMBOL_GPL(kunit_driver_create);

struct kunit_device *__kunit_device_register_internal(struct kunit *test,
						      const char *name,
						      struct device_driver *drv)
{
	struct kunit_device *kunit_dev;
	int err = -ENOMEM;

	kunit_dev = kzalloc(sizeof(struct kunit_device), GFP_KERNEL);
	if (!kunit_dev)
		return ERR_PTR(err);

	kunit_dev->owner = test;

	err = dev_set_name(&kunit_dev->dev, "%s.%s", test->name, name);
	if (err) {
		kfree(kunit_dev);
		return ERR_PTR(err);
	}

	/* Set the expected driver pointer, so we match. */
	kunit_dev->driver = drv;

	kunit_dev->dev.release = kunit_device_release;
	kunit_dev->dev.bus = &kunit_bus_type;
	kunit_dev->dev.parent = &kunit_bus;

	err = device_register(&kunit_dev->dev);
	if (err) {
		put_device(&kunit_dev->dev);
		return ERR_PTR(err);
	}

	kunit_add_action(test, device_unregister_wrapper, &kunit_dev->dev);

	return kunit_dev;
}

struct device *kunit_device_register_with_driver(struct kunit *test,
						 const char *name,
						 struct device_driver *drv)
{
	struct kunit_device *kunit_dev = __kunit_device_register_internal(test, name, drv);

	if (IS_ERR_OR_NULL(kunit_dev))
		return (struct device *)kunit_dev; /* This is an error or NULL, so is compatible */

	return &kunit_dev->dev;
}
EXPORT_SYMBOL_GPL(kunit_device_register_with_driver);

struct device *kunit_device_register(struct kunit *test, const char *name)
{
	struct device_driver *drv = kunit_driver_create(test, name);
	struct kunit_device *dev;

	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, drv);

	dev = __kunit_device_register_internal(test, name, drv);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, dev);

	dev->cleanup_driver = true;

	return (struct device *)dev;
}
EXPORT_SYMBOL_GPL(kunit_device_register);

void kunit_device_unregister(struct kunit *test, struct device *dev)
{
	bool cleanup_driver = ((struct kunit_device *)dev)->cleanup_driver;
	struct device_driver *driver = ((struct kunit_device *)dev)->driver;

	kunit_release_action(test, device_unregister_wrapper, dev);
	if (cleanup_driver)
		kunit_release_action(test, driver_unregister_wrapper, driver);
}
EXPORT_SYMBOL_GPL(kunit_device_unregister);

