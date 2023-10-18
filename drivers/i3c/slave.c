// SPDX-License-Identifier: GPL-2.0
/*
 * configfs to configure the I3C Slave
 *
 * Copyright (C) 2023 NXP
 * Author: Frank Li <Frank.Li@nxp.com>
 */

#include <linux/configfs.h>
#include <linux/module.h>
#include <linux/i3c/slave.h>
#include <linux/idr.h>
#include <linux/slab.h>

static DEFINE_MUTEX(func_lock);
static struct class *i3c_slave_ctrl_class;

static void i3c_slave_func_dev_release(struct device *dev)
{
	struct i3c_slave_func *func = to_i3c_slave_func(dev);

	kfree(func->name);
	kfree(func);
}

static const struct device_type i3c_slave_func_type = {
	.release        = i3c_slave_func_dev_release,
};

static int i3c_slave_func_match_driver(struct device *dev, struct device_driver *drv)
{
	return !strncmp(dev_name(dev), drv->name, strlen(drv->name));
}

static int i3c_slave_func_device_probe(struct device *dev)
{
	struct i3c_slave_func *func = to_i3c_slave_func(dev);
	struct i3c_slave_func_driver *driver = to_i3c_slave_func_driver(dev->driver);

	if (!driver->probe)
		return -ENODEV;

	func->driver = driver;

	return driver->probe(func);
}

static void i3c_slave_func_device_remove(struct device *dev)
{
	struct i3c_slave_func *func = to_i3c_slave_func(dev);
	struct i3c_slave_func_driver *driver = to_i3c_slave_func_driver(dev->driver);

	if (driver->remove)
		driver->remove(func);
	func->driver = NULL;
}

static const struct bus_type i3c_slave_func_bus_type = {
	.name = "i3c_slave_func",
	.probe = i3c_slave_func_device_probe,
	.remove = i3c_slave_func_device_remove,
	.match = i3c_slave_func_match_driver,
};

static void i3c_slave_ctrl_release(struct device *dev)
{
	kfree(to_i3c_slave_ctrl(dev));
}

static void devm_i3c_slave_ctrl_release(struct device *dev, void *res)
{
	struct i3c_slave_ctrl *ctrl = *(struct i3c_slave_ctrl **)res;

	i3c_slave_ctrl_destroy(ctrl);
}

struct i3c_slave_ctrl *
__devm_i3c_slave_ctrl_create(struct device *dev, const struct i3c_slave_ctrl_ops *ops,
			     struct module *owner)
{
	struct i3c_slave_ctrl **ptr, *ctrl;

	ptr = devres_alloc(devm_i3c_slave_ctrl_release, sizeof(*ptr), GFP_KERNEL);
	if (!ptr)
		return ERR_PTR(-ENOMEM);

	ctrl = __i3c_slave_ctrl_create(dev, ops, owner);
	if (!IS_ERR(ctrl)) {
		*ptr = ctrl;
		devres_add(dev, ptr);
	} else {
		devres_free(ptr);
	}

	return ctrl;
}

static int devm_i3c_slave_ctrl_match(struct device *dev, void *res, void *match_data)
{
	struct i3c_slave_ctrl **ptr = res;

	return *ptr == match_data;
}

/**
 * __i3c_slave_ctrl_create() - create a new slave controller device
 * @dev: device that is creating the new slave controller
 * @ops: function pointers for performing slave controller  operations
 * @owner: the owner of the module that creates the slave controller device
 *
 * Return: Pointer to struct i3c_slave_ctrl
 */
struct i3c_slave_ctrl *
__i3c_slave_ctrl_create(struct device *dev, const struct i3c_slave_ctrl_ops *ops,
			struct module *owner)
{
	struct i3c_slave_ctrl *ctrl;
	int ret;

	if (WARN_ON(!dev))
		return ERR_PTR(-EINVAL);

	ctrl = kzalloc(sizeof(*ctrl), GFP_KERNEL);
	if (!ctrl)
		return ERR_PTR(-ENOMEM);

	device_initialize(&ctrl->dev);
	ctrl->dev.class = i3c_slave_ctrl_class;
	ctrl->dev.parent = dev;
	ctrl->dev.release = i3c_slave_ctrl_release;
	ctrl->ops = ops;

	ret = dev_set_name(&ctrl->dev, "%s", dev_name(dev));
	if (ret)
		goto put_dev;

	ret = device_add(&ctrl->dev);
	if (ret)
		goto put_dev;

	ctrl->group = i3c_slave_cfs_add_ctrl_group(ctrl);
	if (!ctrl->group)
		goto put_dev;

	return ctrl;

put_dev:
	put_device(&ctrl->dev);
	kfree(ctrl);

	return ERR_PTR(ret);
}
EXPORT_SYMBOL_GPL(__i3c_slave_ctrl_create);

/**
 * devm_i3c_slave_ctrl_destroy() - destroy the slave controller device
 * @dev: device that is creating the new slave controller device
 * @ops: function pointers for performing slave controller operations
 * @owner: the owner of the module that creates the slave controller device
 *
 * Invoke to create a new slave controller device and add it to i3c_slave class. While at that, it
 * also associates the device with the i3c_slave using devres. On driver detach, release function is
 * invoked on the devres data, then devres data is freed.
 */
void devm_i3c_slave_ctrl_destroy(struct device *dev, struct i3c_slave_ctrl *ctrl)
{
	int r;

	r = devres_destroy(dev, devm_i3c_slave_ctrl_release, devm_i3c_slave_ctrl_match,
			   ctrl);
	dev_WARN_ONCE(dev, r, "couldn't find I3C controller resource\n");
}
EXPORT_SYMBOL_GPL(devm_i3c_slave_ctrl_destroy);

/**
 * i3c_slave_ctrl_destroy() - destroy the slave controller device
 * @ctrl: the slave controller device that has to be destroyed
 *
 * Invoke to destroy the I3C slave device
 */
void i3c_slave_ctrl_destroy(struct i3c_slave_ctrl *ctrl)
{
	i3c_slave_cfs_remove_ctrl_group(ctrl->group);
	device_unregister(&ctrl->dev);
}
EXPORT_SYMBOL_GPL(i3c_slave_ctrl_destroy);

/**
 * i3c_slave_ctrl_add_func() - bind I3C slave function to an slave controller
 * @ctrl: the controller device to which the slave function should be added
 * @func: the slave function to be added
 *
 * An I3C slave device can have only one functions.
 */
int i3c_slave_ctrl_add_func(struct i3c_slave_ctrl *ctrl, struct i3c_slave_func *func)
{
	if (ctrl->func)
		return -EBUSY;

	ctrl->func = func;
	func->ctrl = ctrl;

	return 0;
}
EXPORT_SYMBOL_GPL(i3c_slave_ctrl_add_func);

/**
 * i3c_slave_ctrl_remove_func() - unbind I3C slave function to an slave controller
 * @ctrl: the controller device to which the slave function should be removed
 * @func: the slave function to be removed
 *
 * An I3C slave device can have only one functions.
 */
void i3c_slave_ctrl_remove_func(struct i3c_slave_ctrl *ctrl, struct i3c_slave_func *func)
{
	ctrl->func = NULL;
}
EXPORT_SYMBOL_GPL(i3c_slave_ctrl_remove_func);

/**
 * i3c_slave_ctrl() - get the I3C slave controller
 * @name: device name of the slave controller
 *
 * Invoke to get struct i3c_slave_ctrl * corresponding to the device name of the
 * slave controller
 */
struct i3c_slave_ctrl *i3c_slave_ctrl_get(const char *name)
{
	int ret = -EINVAL;
	struct i3c_slave_ctrl *ctrl;
	struct device *dev;
	struct class_dev_iter iter;

	class_dev_iter_init(&iter, i3c_slave_ctrl_class, NULL, NULL);
	while ((dev = class_dev_iter_next(&iter))) {
		if (strcmp(name, dev_name(dev)))
			continue;

		ctrl = to_i3c_slave_ctrl(dev);
		if (!try_module_get(ctrl->ops->owner)) {
			ret = -EINVAL;
			goto err;
		}

		class_dev_iter_exit(&iter);
		get_device(&ctrl->dev);
		return ctrl;
	}

err:
	class_dev_iter_exit(&iter);
	return ERR_PTR(ret);
}
EXPORT_SYMBOL_GPL(i3c_slave_ctrl_get);

/**
 * i3c_slave_ctrl_put() - release the I3C endpoint controller
 * @ctrl: slave controller returned by pci_slave_get()
 *
 * release the refcount the caller obtained by invoking i3c_slave_ctrl_get()
 */
void i3c_slave_ctrl_put(struct i3c_slave_ctrl *ctrl)
{
	if (!ctrl || IS_ERR(ctrl))
		return;

	module_put(ctrl->ops->owner);
	put_device(&ctrl->dev);
}
EXPORT_SYMBOL_GPL(i3c_slave_ctrl_put);

/**
 * i3c_slave_ctrl_hotjoin() - trigger device hotjoin
 * @ctrl:  slave controller
 *
 * return: 0: success, others failure
 */
int i3c_slave_ctrl_hotjoin(struct i3c_slave_ctrl *ctrl)
{
	if (!ctrl || IS_ERR(ctrl))
		return -EINVAL;

	if (!ctrl->ops->hotjoin)
		return -EINVAL;

	return ctrl->ops->hotjoin(ctrl);
}
EXPORT_SYMBOL_GPL(i3c_slave_ctrl_hotjoin);

/**
 * i3c_slave_func_bind() - Notify the function driver that the function device has been bound to a
 *			   controller device
 * @func: the function device which has been bound to the controller device
 *
 * Invoke to notify the function driver that it has been bound to a controller device
 */
int i3c_slave_func_bind(struct i3c_slave_func *func)
{
	struct device *dev = &func->dev;
	int ret;

	if (!func->driver) {
		dev_WARN(dev, "func device not bound to driver\n");
		return -EINVAL;
	}

	if (!try_module_get(func->driver->owner))
		return -EAGAIN;

	mutex_lock(&func->lock);
	ret = func->driver->ops->bind(func);
	if (!ret)
		func->is_bound = true;
	mutex_unlock(&func->lock);

	return ret;
}
EXPORT_SYMBOL_GPL(i3c_slave_func_bind);

/**
 * i3c_slave_func_unbind() - Notify the function driver that the binding between the function device
 *			     and controller device has been lost
 * @func: the function device which has lost the binding with the controller device
 *
 * Invoke to notify the function driver that the binding between the function device and controller
 * device has been lost.
 */
void i3c_slave_func_unbind(struct i3c_slave_func *func)
{
	if (!func->driver) {
		dev_WARN(&func->dev, "func device not bound to driver\n");
		return;
	}

	mutex_lock(&func->lock);
	if (func->is_bound)
		func->driver->ops->unbind(func);
	mutex_unlock(&func->lock);

	module_put(func->driver->owner);
}
EXPORT_SYMBOL_GPL(i3c_slave_func_unbind);

/**
 * i3c_slave_func_create() - create a new I3C function device
 * @drv_name: the driver name of the I3C function device.
 * @name: the name of the function device.
 *
 * Invoke to create a new I3C function device by providing the name of the function device.
 */
struct i3c_slave_func *i3c_slave_func_create(const char *drv_name, const char *name)
{
	struct i3c_slave_func *func;
	struct device *dev;
	int ret;

	func = kzalloc(sizeof(*func), GFP_KERNEL);
	if (!func)
		return ERR_PTR(-ENOMEM);

	dev = &func->dev;
	device_initialize(dev);
	dev->bus = &i3c_slave_func_bus_type;
	dev->type = &i3c_slave_func_type;
	mutex_init(&func->lock);

	ret = dev_set_name(dev, "%s.%s", drv_name, name);
	if (ret) {
		put_device(dev);
		return ERR_PTR(ret);
	}

	ret = device_add(dev);
	if (ret) {
		put_device(dev);
		return ERR_PTR(ret);
	}

	return func;
}
EXPORT_SYMBOL_GPL(i3c_slave_func_create);

/**
 * __i3c_slave_func_register_driver() - register a new I3C function driver
 * @driver: structure representing I3C function driver
 * @owner: the owner of the module that registers the I3C function driver
 *
 * Invoke to register a new I3C function driver.
 */
int __i3c_slave_func_register_driver(struct i3c_slave_func_driver *driver, struct module *owner)
{
	int ret = -EEXIST;

	if (!driver->ops)
		return -EINVAL;

	if (!driver->ops->bind || !driver->ops->unbind)
		return -EINVAL;

	driver->driver.bus = &i3c_slave_func_bus_type;
	driver->driver.owner = owner;

	ret = driver_register(&driver->driver);
	if (ret)
		return ret;

	i3c_slave_cfs_add_func_group(driver->driver.name);

	return 0;
}
EXPORT_SYMBOL_GPL(__i3c_slave_func_register_driver);

/**
 * i3c_slave_func_unregister_driver() - unregister the I3C function driver
 * @driver: the I3C function driver that has to be unregistered
 *
 * Invoke to unregister the I3C function driver.
 */
void i3c_slave_func_unregister_driver(struct i3c_slave_func_driver *fd)
{
	mutex_lock(&func_lock);
	mutex_unlock(&func_lock);
}
EXPORT_SYMBOL_GPL(i3c_slave_func_unregister_driver);

static int __init i3c_slave_init(void)
{
	int ret;

	i3c_slave_ctrl_class = class_create("i3c_slave");
	if (IS_ERR(i3c_slave_ctrl_class)) {
		pr_err("failed to create i3c slave class --> %ld\n", PTR_ERR(i3c_slave_ctrl_class));
		return PTR_ERR(i3c_slave_ctrl_class);
	}

	ret = bus_register(&i3c_slave_func_bus_type);
	if (ret) {
		class_destroy(i3c_slave_ctrl_class);
		pr_err("failed to register i3c slave func bus --> %d\n", ret);
		return ret;
	}

	return 0;
}
module_init(i3c_slave_init);

static void __exit i3c_slave_exit(void)
{
	class_destroy(i3c_slave_ctrl_class);
	bus_unregister(&i3c_slave_func_bus_type);
}
module_exit(i3c_slave_exit);

