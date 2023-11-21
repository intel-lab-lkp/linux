// SPDX-License-Identifier: GPL-2.0-only
/*
 * Framework to handle complex IIO aggregate devices.
 *
 * The typical architecture is to have one device as the frontend device which
 * can be "linked" against one or multiple backend devices. All the IIO and
 * userspace interface is expected to be registers/managed by the frontend
 * device which will callback into the backends when needed (to get/set some
 * configuration that it does not directly control).
 *
 * The framework interface is pretty simple:
 *   - Backends should register themselves with @devm_iio_backend_register()
 *   - Frontend devices should get backends with @devm_iio_backend_get()
 *
 * Also to note that the primary target for this framework are converters like
 * ADC/DACs so @iio_backend_ops will have some operations typical of converter
 * devices. On top of that, this is "generic" for all IIO which means any kind
 * of device can make use of the framework. That said, If the @iio_backend_ops
 * struct begins to grow out of control, we can always refactor things so that
 * the industrialio-backend.c is only left with the really generic stuff. Then,
 * we can build on top of it depending on the needs.
 *
 * Copyright (C) 2023 Analog Devices Inc.
 */
#define pr_fmt(fmt) "iio-backend: " fmt

#include <linux/cleanup.h>
#include <linux/device.h>
#include <linux/list.h>
#include <linux/kref.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/property.h>
#include <linux/slab.h>

#include <linux/iio/backend.h>

struct iio_backend {
	struct list_head entry;
	const struct iio_backend_ops *ops;
	struct device *dev;
	struct module *owner;
	void *priv;
	/*
	 * mutex used to synchronize backend callback access with concurrent
	 * calls to @iio_backend_unregister. The lock makes sure a device is
	 * not unregistered while a callback is being run.
	 */
	struct mutex lock;
	struct kref ref;
};

static LIST_HEAD(iio_back_list);
static DEFINE_MUTEX(iio_back_lock);

/*
 * Helper macros to properly call backend ops. The main point for these macros
 * is to properly lock the backend mutex on every call plus checking if the
 * backend device is still around (by looking at the *ops pointer).
 */
#define iio_backend_op_call(back, op, args...) ({ \
	struct iio_backend *__back = back; \
	int __ret; \
			\
	guard(mutex)(&__back->lock); \
	if (WARN_ON_ONCE(!__back->ops)) \
		__ret = -ENODEV; \
	else if (!__back->ops->op) \
		__ret = -EOPNOTSUPP; \
	else \
		__ret = __back->ops->op(__back, ##args); \
	\
	__ret; \
})

#define iio_backend_void_op_call(back, op, args...) { \
	struct iio_backend *__back = back; \
						\
	guard(mutex)(&__back->lock); \
	WARN_ON_ONCE(!__back->ops); \
		\
	if (__back->ops && __back->ops->op) \
		__back->ops->op(__back, ##args); \
	\
}

/**
 * iio_backend_chan_enable - Enable a backend channel.
 * @back:	Backend device.
 * @chan:	Channel number.
 *
 * RETURNS:
 * 0 on success, negative error number on failure.
 */
int iio_backend_chan_enable(struct iio_backend *back, unsigned int chan)
{
	return iio_backend_op_call(back, chan_enable, chan);
}
EXPORT_SYMBOL_GPL(iio_backend_chan_enable);

/**
 * iio_backend_chan_disable - Disable a backend channel.
 * @back:	Backend device.
 * @chan:	Channel number.
 *
 * RETURNS:
 * 0 on success, negative error number on failure.
 */
int iio_backend_chan_disable(struct iio_backend *back, unsigned int chan)
{
	return iio_backend_op_call(back, chan_disable, chan);
}
EXPORT_SYMBOL_GPL(iio_backend_chan_disable);

/**
 * iio_backend_chan_enable - Enable the backend.
 * @back:	Backend device
 *
 * RETURNS:
 * 0 on success, negative error number on failure.
 */
int iio_backend_enable(struct iio_backend *back)
{
	return iio_backend_op_call(back, enable);
}
EXPORT_SYMBOL_GPL(iio_backend_enable);

/**
 * iio_backend_disable - Disable the backend.
 * @back:	Backend device
 */
void iio_backend_disable(struct iio_backend *back)
{
	iio_backend_void_op_call(back, disable);
}
EXPORT_SYMBOL_GPL(iio_backend_disable);

/**
 * iio_backend_data_format_set - Configure the channel data format
 * @back:	Backend device
 * @chan:	Channel number.
 * @data:	Data format.
 *
 * Properly configure a channel with respect to the expected data format. A
 * @struct iio_backend_data_fmt must be passed with the settings.
 *
 * RETURNS:
 * 0 on success, negative error number on failure
 */
int iio_backend_data_format_set(struct iio_backend *back, unsigned int chan,
				const struct iio_backend_data_fmt *data)
{
	if (!data || data->type >= IIO_BACKEND_DATA_TYPE_MAX)
		return -EINVAL;

	return iio_backend_op_call(back, data_format_set, chan, data);
}
EXPORT_SYMBOL_GPL(iio_backend_data_format_set);

static void iio_backend_free(struct kref *ref)
{
	struct iio_backend *back = container_of(ref, struct iio_backend, ref);

	kfree(back);
}

static void iio_backend_release(void *arg)
{
	struct iio_backend *back = arg;

	module_put(back->owner);
	kref_put(&back->ref, iio_backend_free);
}

/**
 * devm_iio_backend_get - Get a backend device
 * @dev:	Device where to look for the backend.
 * @name:	Backend name.
 *
 * Get's the backend associated with @dev.
 *
 * RETURNS:
 * A backend pointer, negative error pointer otherwise.
 */
struct iio_backend *devm_iio_backend_get(struct device *dev, const char *name)
{
	struct fwnode_handle *fwnode;
	struct iio_backend *back;
	int index = 0, ret;

	if (name) {
		index = device_property_match_string(dev, "io-backends-names",
						     name);
		if (index < 0)
			return ERR_PTR(index);
	}

	fwnode = fwnode_find_reference(dev_fwnode(dev), "io-backends", index);
	if (IS_ERR(fwnode)) {
		dev_err(dev, "Cannot get Firmware reference\n");
		return ERR_CAST(fwnode);
	}

	guard(mutex)(&iio_back_lock);
	list_for_each_entry(back, &iio_back_list, entry) {
		struct device_link *link;

		if (!device_match_fwnode(back->dev, fwnode))
			continue;

		fwnode_handle_put(fwnode);
		kref_get(&back->ref);
		if (!try_module_get(back->owner)) {
			dev_err(dev, "Cannot get module reference\n");
			return ERR_PTR(-ENODEV);
		}

		ret = devm_add_action_or_reset(dev, iio_backend_release, back);
		if (ret)
			return ERR_PTR(ret);

		link = device_link_add(dev, back->dev,
				       DL_FLAG_AUTOREMOVE_CONSUMER);
		if (!link)
			dev_warn(dev, "Could not link to supplier(%s)\n",
				 dev_name(back->dev));

		dev_dbg(dev, "Found backend(%s) device\n", dev_name(back->dev));
		return back;
	}

	fwnode_handle_put(fwnode);
	return ERR_PTR(-EPROBE_DEFER);
}
EXPORT_SYMBOL_GPL(devm_iio_backend_get);

/**
 * iio_backend_get_priv - Get driver private data
 * @back	Backend device
 */
void *iio_backend_get_priv(const struct iio_backend *back)
{
	return back->priv;
}
EXPORT_SYMBOL_GPL(iio_backend_get_priv);

static void iio_backend_unregister(void *arg)
{
	struct iio_backend *back = arg;

	mutex_lock(&iio_back_lock);
	list_del(&back->entry);
	mutex_unlock(&iio_back_lock);

	mutex_lock(&back->lock);
	back->ops = NULL;
	mutex_unlock(&back->lock);
	kref_put(&back->ref, iio_backend_free);
}

/**
 * devm_iio_backend_register - Register a new backend device
 * @dev		Backend device being registered.
 * @ops		Backend ops
 * @priv	Device private data.
 *
 * @ops and @priv are both mandatory. Not providing them results in -EINVAL.
 *
 * RETURNS:
 * 0 on success, negative error number on failure.
 */
int devm_iio_backend_register(struct device *dev,
			      const struct iio_backend_ops *ops, void *priv)
{
	struct iio_backend *back;

	if (!ops || !priv) {
		dev_err(dev, "No backend ops or private data given\n");
		return -EINVAL;
	}

	back = kzalloc(sizeof(*back), GFP_KERNEL);
	if (!back)
		return -ENOMEM;

	kref_init(&back->ref);
	mutex_init(&back->lock);
	back->ops = ops;
	back->owner = dev->driver->owner;
	back->dev = dev;
	back->priv = priv;
	mutex_lock(&iio_back_lock);
	list_add(&back->entry, &iio_back_list);
	mutex_unlock(&iio_back_lock);

	return devm_add_action_or_reset(dev, iio_backend_unregister, back);
}
EXPORT_SYMBOL_GPL(devm_iio_backend_register);

MODULE_AUTHOR("Nuno Sa <nuno.sa@analog.com>");
MODULE_DESCRIPTION("Framework to handle complex IIO aggregate devices");
MODULE_LICENSE("GPL");
