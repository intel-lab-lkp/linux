// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * eSPI (Enhanced Serial Peripheral Interface) core framework
 *
 * Copyright (c) 2026, Advanced Micro Devices, Inc.
 */

#define pr_fmt(fmt)	"espi: " fmt

#include <linux/acpi.h>
#include <linux/cleanup.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/xarray.h>
#include <linux/espi/espi.h>

static DEFINE_XARRAY_ALLOC(espi_controllers);

static int espi_bus_match(struct device *dev, const struct device_driver *drv)
{
	const struct espi_device *edev = to_espi_device(dev);
	const struct espi_driver *edrv = to_espi_driver(drv);

	if (edrv->id_table) {
		const struct espi_device_id *id = edrv->id_table;

		while (id->name[0]) {
			if (!strcmp(edev->modalias, id->name))
				return 1;
			id++;
		}
	}
	if (acpi_driver_match_device(dev, drv))
		return 1;

	return 0;
}

static int espi_bus_uevent(const struct device *dev, struct kobj_uevent_env *env)
{
	const struct espi_device *edev = to_espi_device(dev);

	return add_uevent_var(env, "MODALIAS=espi:%s", edev->modalias);
}

static int espi_bus_probe(struct device *dev)
{
	const struct espi_driver *edrv = to_espi_driver(dev->driver);
	struct espi_device *edev = to_espi_device(dev);

	if (edrv->probe)
		return edrv->probe(edev);
	return 0;
}

static void espi_bus_remove(struct device *dev)
{
	const struct espi_driver *edrv = to_espi_driver(dev->driver);
	struct espi_device *edev = to_espi_device(dev);

	if (edrv->remove)
		edrv->remove(edev);
}

const struct bus_type espi_bus_type = {
	.name	= "espi",
	.match	= espi_bus_match,
	.uevent	= espi_bus_uevent,
	.probe	= espi_bus_probe,
	.remove	= espi_bus_remove,
};
EXPORT_SYMBOL_GPL(espi_bus_type);

static ssize_t supported_channels_show(struct device *dev,
				       struct device_attribute *attr, char *buf)
{
	struct espi_controller *ctrl = to_espi_controller(dev);

	return sysfs_emit(buf, "0x%02x\n", ctrl->caps.supported_channels);
}
static DEVICE_ATTR_RO(supported_channels);

static ssize_t max_freq_mhz_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	struct espi_controller *ctrl = to_espi_controller(dev);

	return sysfs_emit(buf, "%u\n", ctrl->caps.max_freq_mhz);
}
static DEVICE_ATTR_RO(max_freq_mhz);

static ssize_t io_mode_show(struct device *dev,
			    struct device_attribute *attr, char *buf)
{
	struct espi_controller *ctrl = to_espi_controller(dev);
	static const char * const modes[] = { "single", "dual", "quad" };
	u8 m = ctrl->caps.io_mode;

	if (WARN_ON_ONCE(m > ESPI_IO_MODE_QUAD))
		return sysfs_emit(buf, "unknown\n");
	return sysfs_emit(buf, "%s\n", modes[m]);
}
static DEVICE_ATTR_RO(io_mode);

static ssize_t channel_enabled_show(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	struct espi_controller *ctrl = to_espi_controller(dev);

	return sysfs_emit(buf, "0x%02x\n", READ_ONCE(ctrl->channel_enabled));
}
static DEVICE_ATTR_RO(channel_enabled);

static struct attribute *espi_controller_attrs[] = {
	&dev_attr_supported_channels.attr,
	&dev_attr_max_freq_mhz.attr,
	&dev_attr_io_mode.attr,
	&dev_attr_channel_enabled.attr,
	NULL,
};
ATTRIBUTE_GROUPS(espi_controller);

static void espi_controller_release(struct device *dev)
{
	struct espi_controller *ctrl = to_espi_controller(dev);

	mutex_destroy(&ctrl->lock);
	mutex_destroy(&ctrl->device_list_lock);
	kfree(ctrl);
}

static const struct device_type espi_controller_type = {
	.groups	 = espi_controller_groups,
	.release = espi_controller_release,
};

struct espi_controller *espi_controller_alloc(struct device *parent,
					      unsigned int size)
{
	struct espi_controller *ctrl;

	if (!parent)
		return ERR_PTR(-EINVAL);

	ctrl = kzalloc(sizeof(*ctrl) + size, GFP_KERNEL);
	if (!ctrl)
		return ERR_PTR(-ENOMEM);

	device_initialize(&ctrl->dev);
	ctrl->dev.parent = parent;
	ctrl->dev.type = &espi_controller_type;

	mutex_init(&ctrl->lock);
	INIT_LIST_HEAD(&ctrl->device_list);
	mutex_init(&ctrl->device_list_lock);
	BLOCKING_INIT_NOTIFIER_HEAD(&ctrl->notifier_list);

	if (size)
		espi_controller_set_devdata(ctrl, (void *)ctrl + sizeof(*ctrl));

	return ctrl;
}
EXPORT_SYMBOL_GPL(espi_controller_alloc);

int espi_controller_register(struct espi_controller *ctrl)
{
	int ret;
	u32 id;

	if (!ctrl || !ctrl->ops)
		return -EINVAL;

	ret = xa_alloc(&espi_controllers, &id, ctrl, xa_limit_31b,
		       GFP_KERNEL);
	if (ret)
		return ret;

	ctrl->bus_num = id;
	ret = dev_set_name(&ctrl->dev, "espi%d", ctrl->bus_num);
	if (ret)
		goto err_erase;

	if (ctrl->ops->setup) {
		ret = ctrl->ops->setup(ctrl);
		if (ret) {
			dev_err(&ctrl->dev, "controller setup failed: %d\n", ret);
			goto err_erase;
		}
	}

	ret = device_add(&ctrl->dev);
	if (ret) {
		dev_err(&ctrl->dev, "device_add failed: %d\n", ret);
		if (ctrl->ops->cleanup)
			ctrl->ops->cleanup(ctrl);
		goto err_erase;
	}

	dev_info(&ctrl->dev, "registered: channels=0x%02x freq=%uMHz\n",
		 ctrl->caps.supported_channels, ctrl->caps.max_freq_mhz);
	return 0;

err_erase:
	xa_erase(&espi_controllers, ctrl->bus_num);
	ctrl->bus_num = -1;
	return ret;
}
EXPORT_SYMBOL_GPL(espi_controller_register);

void espi_controller_unregister(struct espi_controller *ctrl)
{
	if (!ctrl)
		return;
	/*
	 * Remove from the lookup table before dropping the device reference,
	 * so a concurrent espi_controller_get_by_bus_num() can never take a
	 * reference on a controller that is going away.
	 */
	xa_erase(&espi_controllers, ctrl->bus_num);
	if (ctrl->ops && ctrl->ops->cleanup)
		ctrl->ops->cleanup(ctrl);
	device_unregister(&ctrl->dev);
}
EXPORT_SYMBOL_GPL(espi_controller_unregister);

void espi_controller_put(struct espi_controller *ctrl)
{
	if (ctrl)
		put_device(&ctrl->dev);
}
EXPORT_SYMBOL_GPL(espi_controller_put);

struct espi_controller *espi_controller_get_by_bus_num(int bus_num)
{
	struct espi_controller *ctrl;

	guard(spinlock)(&espi_controllers.xa_lock);
	ctrl = xa_load(&espi_controllers, bus_num);
	if (ctrl)
		get_device(&ctrl->dev);
	return ctrl;
}
EXPORT_SYMBOL_GPL(espi_controller_get_by_bus_num);

int espi_get_capabilities(struct espi_controller *ctrl,
			  struct espi_capabilities *caps)
{
	if (!ctrl || !caps)
		return -EINVAL;
	guard(mutex)(&ctrl->lock);
	*caps = ctrl->caps;
	return 0;
}
EXPORT_SYMBOL_GPL(espi_get_capabilities);

bool espi_channel_is_enabled(struct espi_controller *ctrl, u8 channel)
{
	if (!ctrl || channel >= ESPI_CHANNEL_COUNT)
		return false;
	guard(mutex)(&ctrl->lock);
	return !!(ctrl->channel_enabled & BIT(channel));
}
EXPORT_SYMBOL_GPL(espi_channel_is_enabled);

int espi_get_configuration(struct espi_controller *ctrl,
			   u32 slave_reg_addr, u32 *config)
{
	if (!ctrl || !ctrl->ops || !ctrl->ops->get_configuration)
		return -EOPNOTSUPP;
	if (!config)
		return -EINVAL;
	guard(mutex)(&ctrl->lock);
	return ctrl->ops->get_configuration(ctrl, slave_reg_addr, config);
}
EXPORT_SYMBOL_GPL(espi_get_configuration);

int espi_set_configuration(struct espi_controller *ctrl,
			   u32 slave_reg_addr, u32 config)
{
	if (!ctrl || !ctrl->ops || !ctrl->ops->set_configuration)
		return -EOPNOTSUPP;
	guard(mutex)(&ctrl->lock);
	return ctrl->ops->set_configuration(ctrl, slave_reg_addr, config);
}
EXPORT_SYMBOL_GPL(espi_set_configuration);

int espi_inband_reset(struct espi_controller *ctrl)
{
	if (!ctrl || !ctrl->ops || !ctrl->ops->inband_reset)
		return -EOPNOTSUPP;
	guard(mutex)(&ctrl->lock);
	return ctrl->ops->inband_reset(ctrl);
}
EXPORT_SYMBOL_GPL(espi_inband_reset);

int espi_get_status(struct espi_controller *ctrl,
		    struct espi_slave_status *status)
{
	if (!ctrl || !ctrl->ops || !ctrl->ops->get_status)
		return -EOPNOTSUPP;
	if (!status)
		return -EINVAL;
	guard(mutex)(&ctrl->lock);
	return ctrl->ops->get_status(ctrl, status);
}
EXPORT_SYMBOL_GPL(espi_get_status);

int espi_enable_channel(struct espi_controller *ctrl, u8 channel)
{
	int ret;

	if (!ctrl || !ctrl->ops || !ctrl->ops->enable_channel)
		return -EOPNOTSUPP;
	if (channel >= ESPI_CHANNEL_COUNT)
		return -EINVAL;
	guard(mutex)(&ctrl->lock);
	ret = ctrl->ops->enable_channel(ctrl, channel);
	if (!ret)
		ctrl->channel_enabled |= BIT(channel);
	return ret;
}
EXPORT_SYMBOL_GPL(espi_enable_channel);

int espi_disable_channel(struct espi_controller *ctrl, u8 channel)
{
	int ret;

	if (!ctrl || !ctrl->ops || !ctrl->ops->disable_channel)
		return -EOPNOTSUPP;
	if (channel >= ESPI_CHANNEL_COUNT)
		return -EINVAL;
	guard(mutex)(&ctrl->lock);
	ret = ctrl->ops->disable_channel(ctrl, channel);
	if (!ret)
		ctrl->channel_enabled &= ~BIT(channel);
	return ret;
}
EXPORT_SYMBOL_GPL(espi_disable_channel);

int espi_periph_io_read(struct espi_controller *ctrl,
			u16 port, u8 width, u32 *value)
{
	if (!ctrl || !ctrl->ops || !ctrl->ops->periph_io_read)
		return -EOPNOTSUPP;
	guard(mutex)(&ctrl->lock);
	return ctrl->ops->periph_io_read(ctrl, port, width, value);
}
EXPORT_SYMBOL_GPL(espi_periph_io_read);

int espi_periph_io_write(struct espi_controller *ctrl,
			 u16 port, u8 width, u32 value)
{
	if (!ctrl || !ctrl->ops || !ctrl->ops->periph_io_write)
		return -EOPNOTSUPP;
	guard(mutex)(&ctrl->lock);
	return ctrl->ops->periph_io_write(ctrl, port, width, value);
}
EXPORT_SYMBOL_GPL(espi_periph_io_write);

int espi_periph_mem_read(struct espi_controller *ctrl,
			 u32 addr, void *buf, size_t len)
{
	if (!ctrl || !ctrl->ops || !ctrl->ops->periph_mem_read)
		return -EOPNOTSUPP;
	guard(mutex)(&ctrl->lock);
	return ctrl->ops->periph_mem_read(ctrl, addr, buf, len);
}
EXPORT_SYMBOL_GPL(espi_periph_mem_read);

int espi_periph_mem_write(struct espi_controller *ctrl,
			  u32 addr, const void *buf, size_t len)
{
	if (!ctrl || !ctrl->ops || !ctrl->ops->periph_mem_write)
		return -EOPNOTSUPP;
	guard(mutex)(&ctrl->lock);
	return ctrl->ops->periph_mem_write(ctrl, addr, buf, len);
}
EXPORT_SYMBOL_GPL(espi_periph_mem_write);

int espi_vwire_get(struct espi_controller *ctrl, u8 index, u8 *value, u8 *valid)
{
	if (!ctrl || !ctrl->ops || !ctrl->ops->vwire_get)
		return -EOPNOTSUPP;
	guard(mutex)(&ctrl->lock);
	return ctrl->ops->vwire_get(ctrl, index, value, valid);
}
EXPORT_SYMBOL_GPL(espi_vwire_get);

int espi_vwire_put(struct espi_controller *ctrl, u8 index, u8 value, u8 valid)
{
	if (!ctrl || !ctrl->ops || !ctrl->ops->vwire_put)
		return -EOPNOTSUPP;
	guard(mutex)(&ctrl->lock);
	return ctrl->ops->vwire_put(ctrl, index, value, valid);
}
EXPORT_SYMBOL_GPL(espi_vwire_put);

int espi_oob_send(struct espi_controller *ctrl, const void *buf, size_t len, u8 tag)
{
	if (!ctrl || !ctrl->ops || !ctrl->ops->oob_send)
		return -EOPNOTSUPP;
	guard(mutex)(&ctrl->lock);
	return ctrl->ops->oob_send(ctrl, buf, len, tag);
}
EXPORT_SYMBOL_GPL(espi_oob_send);

int espi_oob_recv(struct espi_controller *ctrl, void *buf, size_t *len, u8 *tag)
{
	if (!ctrl || !ctrl->ops || !ctrl->ops->oob_recv)
		return -EOPNOTSUPP;
	guard(mutex)(&ctrl->lock);
	return ctrl->ops->oob_recv(ctrl, buf, len, tag);
}
EXPORT_SYMBOL_GPL(espi_oob_recv);

int espi_flash_read(struct espi_controller *ctrl, u32 offset, void *buf, size_t len)
{
	if (!ctrl || !ctrl->ops || !ctrl->ops->flash_read)
		return -EOPNOTSUPP;
	guard(mutex)(&ctrl->lock);
	return ctrl->ops->flash_read(ctrl, offset, buf, len);
}
EXPORT_SYMBOL_GPL(espi_flash_read);

int espi_flash_write(struct espi_controller *ctrl, u32 offset, const void *buf, size_t len)
{
	if (!ctrl || !ctrl->ops || !ctrl->ops->flash_write)
		return -EOPNOTSUPP;
	guard(mutex)(&ctrl->lock);
	return ctrl->ops->flash_write(ctrl, offset, buf, len);
}
EXPORT_SYMBOL_GPL(espi_flash_write);

int espi_flash_erase(struct espi_controller *ctrl, u32 offset, size_t len)
{
	if (!ctrl || !ctrl->ops || !ctrl->ops->flash_erase)
		return -EOPNOTSUPP;
	guard(mutex)(&ctrl->lock);
	return ctrl->ops->flash_erase(ctrl, offset, len);
}
EXPORT_SYMBOL_GPL(espi_flash_erase);

/*
 * espi_handle_alert - dispatch a hardware alert to the controller
 *
 * Must be called from process context (threaded IRQ or workqueue).
 *
 * ctrl->lock is NOT held across ops->handle_alert so that the driver
 * callback can call espi_notify_event() without deadlocking: notifier
 * callbacks may in turn call channel APIs that also acquire ctrl->lock.
 * The driver is responsible for taking ctrl->lock around any register
 * accesses that need serialisation with the channel API.
 */
int espi_handle_alert(struct espi_controller *ctrl)
{
	if (!ctrl || !ctrl->ops || !ctrl->ops->handle_alert)
		return -EOPNOTSUPP;
	return ctrl->ops->handle_alert(ctrl);
}
EXPORT_SYMBOL_GPL(espi_handle_alert);

int __espi_register_driver(struct module *owner, struct espi_driver *drv)
{
	drv->driver.owner = owner;
	drv->driver.bus = &espi_bus_type;
	return driver_register(&drv->driver);
}
EXPORT_SYMBOL_GPL(__espi_register_driver);

void espi_unregister_driver(struct espi_driver *drv)
{
	driver_unregister(&drv->driver);
}
EXPORT_SYMBOL_GPL(espi_unregister_driver);

static int __init espi_init(void)
{
	int ret = bus_register(&espi_bus_type);

	if (ret)
		pr_err("failed to register eSPI bus: %d\n", ret);
	return ret;
}
postcore_initcall(espi_init);

MODULE_AUTHOR("Krishnamoorthi M <krishnamoorthi.m@amd.com>");
MODULE_DESCRIPTION("eSPI core framework");
MODULE_LICENSE("GPL");
