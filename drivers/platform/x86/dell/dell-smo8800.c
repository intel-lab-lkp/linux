// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *  dell-smo8800.c - Dell Latitude ACPI SMO88XX freefall sensor driver
 *
 *  Copyright (C) 2012 Sonal Santan <sonal.santan@gmail.com>
 *  Copyright (C) 2014 Pali Rohár <pali@kernel.org>
 *  Copyright (C) 2023 Hans de Goede <hansg@kernel.org>
 *
 *  This is loosely based on lis3lv02d driver.
 */

#define DRIVER_NAME "smo8800"

#include <linux/device/bus.h>
#include <linux/dmi.h>
#include <linux/fs.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/miscdevice.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/notifier.h>
#include <linux/platform_device.h>
#include <linux/uaccess.h>
#include <linux/workqueue.h>

struct smo8800_device {
	u32 irq;                     /* acpi device irq */
	atomic_t counter;            /* count after last read */
	struct miscdevice miscdev;   /* for /dev/freefall */
	unsigned long misc_opened;   /* whether the device is open */
	wait_queue_head_t misc_wait; /* Wait queue for the misc dev */
	struct notifier_block i2c_nb;/* i2c bus notifier */
	struct work_struct i2c_work; /* Work for instantiating lis3lv02d i2c_client */
	struct i2c_client *i2c_dev;  /* i2c_client for lis3lv02d */
	struct device *dev;          /* acpi device */
};

static irqreturn_t smo8800_interrupt_quick(int irq, void *data)
{
	struct smo8800_device *smo8800 = data;

	atomic_inc(&smo8800->counter);
	wake_up_interruptible(&smo8800->misc_wait);
	return IRQ_WAKE_THREAD;
}

static irqreturn_t smo8800_interrupt_thread(int irq, void *data)
{
	struct smo8800_device *smo8800 = data;

	dev_info(smo8800->dev, "detected free fall\n");
	return IRQ_HANDLED;
}

static ssize_t smo8800_misc_read(struct file *file, char __user *buf,
				 size_t count, loff_t *pos)
{
	struct smo8800_device *smo8800 = container_of(file->private_data,
					 struct smo8800_device, miscdev);

	u32 data = 0;
	unsigned char byte_data;
	ssize_t retval = 1;

	if (count < 1)
		return -EINVAL;

	atomic_set(&smo8800->counter, 0);
	retval = wait_event_interruptible(smo8800->misc_wait,
				(data = atomic_xchg(&smo8800->counter, 0)));

	if (retval)
		return retval;

	retval = 1;

	byte_data = min_t(u32, data, 255);

	if (put_user(byte_data, buf))
		retval = -EFAULT;

	return retval;
}

static int smo8800_misc_open(struct inode *inode, struct file *file)
{
	struct smo8800_device *smo8800 = container_of(file->private_data,
					 struct smo8800_device, miscdev);

	if (test_and_set_bit(0, &smo8800->misc_opened))
		return -EBUSY; /* already open */

	atomic_set(&smo8800->counter, 0);
	return 0;
}

static int smo8800_misc_release(struct inode *inode, struct file *file)
{
	struct smo8800_device *smo8800 = container_of(file->private_data,
					 struct smo8800_device, miscdev);

	clear_bit(0, &smo8800->misc_opened); /* release the device */
	return 0;
}

static const struct file_operations smo8800_misc_fops = {
	.owner = THIS_MODULE,
	.read = smo8800_misc_read,
	.open = smo8800_misc_open,
	.release = smo8800_misc_release,
};

/*
 * Accelerometer's I2C address is not specified in DMI nor ACPI,
 * so it is needed to define mapping table based on DMI product names.
 */
static const struct dmi_system_id smo8800_lis3lv02d_devices[] = {
	/*
	 * Dell platform team told us that these Latitude devices have
	 * ST microelectronics accelerometer at I2C address 0x29.
	 */
	{
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Dell Inc."),
			DMI_MATCH(DMI_PRODUCT_NAME, "Latitude E5250"),
		},
		.driver_data = (void *)0x29L,
	},
	{
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Dell Inc."),
			DMI_MATCH(DMI_PRODUCT_NAME, "Latitude E5450"),
		},
		.driver_data = (void *)0x29L,
	},
	{
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Dell Inc."),
			DMI_MATCH(DMI_PRODUCT_NAME, "Latitude E5550"),
		},
		.driver_data = (void *)0x29L,
	},
	{
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Dell Inc."),
			DMI_MATCH(DMI_PRODUCT_NAME, "Latitude E6440"),
		},
		.driver_data = (void *)0x29L,
	},
	{
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Dell Inc."),
			DMI_MATCH(DMI_PRODUCT_NAME, "Latitude E6440 ATG"),
		},
		.driver_data = (void *)0x29L,
	},
	{
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Dell Inc."),
			DMI_MATCH(DMI_PRODUCT_NAME, "Latitude E6540"),
		},
		.driver_data = (void *)0x29L,
	},
	/*
	 * Additional individual entries were added after verification.
	 */
	{
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Dell Inc."),
			DMI_MATCH(DMI_PRODUCT_NAME, "Latitude 5480"),
		},
		.driver_data = (void *)0x29L,
	},
	{
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Dell Inc."),
			DMI_MATCH(DMI_PRODUCT_NAME, "Precision 3540"),
		},
		.driver_data = (void *)0x29L,
	},
	{
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Dell Inc."),
			DMI_MATCH(DMI_PRODUCT_NAME, "Vostro V131"),
		},
		.driver_data = (void *)0x1dL,
	},
	{
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Dell Inc."),
			DMI_MATCH(DMI_PRODUCT_NAME, "Vostro 5568"),
		},
		.driver_data = (void *)0x29L,
	},
	{
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Dell Inc."),
			DMI_MATCH(DMI_PRODUCT_NAME, "XPS 15 7590"),
		},
		.driver_data = (void *)0x29L,
	},
	{ }
};

static int smo8800_find_i801(struct device *dev, void *data)
{
	struct i2c_adapter *adap, **adap_ret = data;

	adap = i2c_verify_adapter(dev);
	if (!adap)
		return 0;

	if (!strstarts(adap->name, "SMBus I801 adapter"))
		return 0;

	*adap_ret = i2c_get_adapter(adap->nr);
	return 1;
}

static void smo8800_instantiate_i2c_client(struct work_struct *work)
{
	struct smo8800_device *smo8800 =
		container_of(work, struct smo8800_device, i2c_work);
	const struct dmi_system_id *lis3lv02d_dmi_id;
	struct i2c_board_info info = { };
	struct i2c_adapter *adap = NULL;

	if (smo8800->i2c_dev)
		return;

	bus_for_each_dev(&i2c_bus_type, NULL, &adap, smo8800_find_i801);
	if (!adap)
		return;

	lis3lv02d_dmi_id = dmi_first_match(smo8800_lis3lv02d_devices);
	if (!lis3lv02d_dmi_id)
		goto out_put_adapter;

	info.addr = (long)lis3lv02d_dmi_id->driver_data;
	strscpy(info.type, "lis3lv02d", I2C_NAME_SIZE);

	smo8800->i2c_dev = i2c_new_client_device(adap, &info);
	if (IS_ERR(smo8800->i2c_dev)) {
		dev_err(smo8800->dev, "error %ld registering %s i2c_client\n",
			PTR_ERR(smo8800->i2c_dev), info.type);
		smo8800->i2c_dev = NULL;
	} else {
		dev_dbg(smo8800->dev, "registered %s i2c_client on address 0x%02x\n",
			info.type, info.addr);
	}

out_put_adapter:
	i2c_put_adapter(adap);
}

static int smo8800_i2c_bus_notify(struct notifier_block *nb,
				  unsigned long action, void *data)
{
	struct smo8800_device *smo8800 =
		container_of(nb, struct smo8800_device, i2c_nb);
	struct device *dev = data;
	struct i2c_client *client;
	struct i2c_adapter *adap;

	switch (action) {
	case BUS_NOTIFY_ADD_DEVICE:
		adap = i2c_verify_adapter(dev);
		if (!adap)
			break;

		if (strstarts(adap->name, "SMBus I801 adapter"))
			queue_work(system_long_wq, &smo8800->i2c_work);
		break;
	case BUS_NOTIFY_REMOVED_DEVICE:
		client = i2c_verify_client(dev);
		if (!client)
			break;

		if (smo8800->i2c_dev == client) {
			dev_dbg(smo8800->dev, "accelerometer i2c_client removed\n");
			smo8800->i2c_dev = NULL;
		}
		break;
	default:
		break;
	}

	return 0;
}

static int smo8800_probe(struct platform_device *device)
{
	int err;
	struct smo8800_device *smo8800;

	smo8800 = devm_kzalloc(&device->dev, sizeof(*smo8800), GFP_KERNEL);
	if (!smo8800) {
		dev_err(&device->dev, "failed to allocate device data\n");
		return -ENOMEM;
	}

	smo8800->dev = &device->dev;
	smo8800->miscdev.minor = MISC_DYNAMIC_MINOR;
	smo8800->miscdev.name = "freefall";
	smo8800->miscdev.fops = &smo8800_misc_fops;
	smo8800->i2c_nb.notifier_call = smo8800_i2c_bus_notify;

	init_waitqueue_head(&smo8800->misc_wait);
	INIT_WORK(&smo8800->i2c_work, smo8800_instantiate_i2c_client);

	err = misc_register(&smo8800->miscdev);
	if (err) {
		dev_err(&device->dev, "failed to register misc dev: %d\n", err);
		return err;
	}

	platform_set_drvdata(device, smo8800);

	err = platform_get_irq(device, 0);
	if (err < 0)
		goto error;
	smo8800->irq = err;

	err = request_threaded_irq(smo8800->irq, smo8800_interrupt_quick,
				   smo8800_interrupt_thread,
				   IRQF_TRIGGER_RISING | IRQF_ONESHOT,
				   DRIVER_NAME, smo8800);
	if (err) {
		dev_err(&device->dev,
			"failed to request thread for IRQ %d: %d\n",
			smo8800->irq, err);
		goto error;
	}

	dev_dbg(&device->dev, "device /dev/freefall registered with IRQ %d\n",
		 smo8800->irq);

	if (dmi_check_system(smo8800_lis3lv02d_devices)) {
		/*
		 * Register i2c-bus notifier + queue initial scan for lis3lv02d
		 * i2c_client instantiation.
		 */
		err = bus_register_notifier(&i2c_bus_type, &smo8800->i2c_nb);
		if (err)
			goto error_free_irq;

		queue_work(system_long_wq, &smo8800->i2c_work);
	} else {
		dev_warn(&device->dev,
			 "lis3lv02d accelerometer is present on SMBus but its address is unknown, skipping registration\n");
	}

	return 0;

error_free_irq:
	free_irq(smo8800->irq, smo8800);
error:
	misc_deregister(&smo8800->miscdev);
	return err;
}

static void smo8800_remove(struct platform_device *device)
{
	struct smo8800_device *smo8800 = platform_get_drvdata(device);

	if (dmi_check_system(smo8800_lis3lv02d_devices)) {
		bus_unregister_notifier(&i2c_bus_type, &smo8800->i2c_nb);
		cancel_work_sync(&smo8800->i2c_work);
		i2c_unregister_device(smo8800->i2c_dev);
	}

	free_irq(smo8800->irq, smo8800);
	misc_deregister(&smo8800->miscdev);
	dev_dbg(&device->dev, "device /dev/freefall unregistered\n");
}

static const struct acpi_device_id smo8800_ids[] = {
	{ "SMO8800", 0 },
	{ "SMO8801", 0 },
	{ "SMO8810", 0 },
	{ "SMO8811", 0 },
	{ "SMO8820", 0 },
	{ "SMO8821", 0 },
	{ "SMO8830", 0 },
	{ "SMO8831", 0 },
	{ "", 0 },
};
MODULE_DEVICE_TABLE(acpi, smo8800_ids);

static struct platform_driver smo8800_driver = {
	.probe = smo8800_probe,
	.remove_new = smo8800_remove,
	.driver = {
		.name = DRIVER_NAME,
		.acpi_match_table = smo8800_ids,
	},
};
module_platform_driver(smo8800_driver);

MODULE_DESCRIPTION("Dell Latitude freefall driver (ACPI SMO88XX)");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Sonal Santan, Pali Rohár");
