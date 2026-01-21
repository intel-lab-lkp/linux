// SPDX-License-Identifier: GPL-2.0-only
/*
 * Pogo switch/hall sensor driver
 *
 * Copyright (c) 2024 faiot.com
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/init.h>
#include <linux/fs.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/sched.h>
#include <linux/pm.h>
#include <linux/slab.h>
#include <linux/sysctl.h>
#include <linux/proc_fs.h>
#include <linux/delay.h>
#include <linux/platform_device.h>
#include <linux/input.h>
#include <linux/gpio_keys.h>
#include <linux/workqueue.h>
#include <linux/gpio.h>
#include <linux/gpio/consumer.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/spinlock.h>
#include <dt-bindings/input/gpio-keys.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/module.h>
#include <linux/of_gpio.h>

#define LID_DEV_NAME	"hall_sensor"
#define HALL_INPUT	"/dev/input/hall_dev"
#define KEY_HALL	84

struct hall_data {
	int gpio;			/* device use gpio number */
	int irq;			/* device request irq number */
	int active_low;			/* gpio active high or low for valid value */
	bool wakeup;			/* device can wakeup system or not */
	struct input_dev *hall_dev;
	struct device *dev;
	int detect_irq;
	struct workqueue_struct *hall_wq;
	struct delayed_work hall_debounce_work_det;
};

static struct hall_data *p_hall_data;
static struct class extbd_ctrl_class;
static int g_extbd_status;

static void hall_driver_remove(struct platform_device *dev);

static int hall_parse_dt(struct device *dev, struct hall_data *data)
{
	struct device_node *np = dev->of_node;

	data->gpio = of_get_named_gpio(np, "faiot,hall-int", 0);
	if (!gpio_is_valid(data->gpio)) {
		dev_err(dev, "hall gpio is not valid\n");
		return -EINVAL;
	}

	return 0;
}

static irqreturn_t hall_interrupt_handler(int irq, void *dev)
{
	if (p_hall_data == NULL || p_hall_data->hall_wq == NULL) {
		pr_err("zy : irq resource not rdy\n");
		return IRQ_HANDLED;
	}

	queue_delayed_work(p_hall_data->hall_wq,
			   &p_hall_data->hall_debounce_work_det,
			   msecs_to_jiffies(50));
	return IRQ_HANDLED;
}

static void hall_det_debounce_work_func(struct work_struct *work)
{
	int value;

	value = (gpio_get_value_cansleep(p_hall_data->gpio) ? 1 : 0) ^
		p_hall_data->active_low;
	if (value) {
		input_report_key(p_hall_data->hall_dev, KEY_HALL, 0);
		input_sync(p_hall_data->hall_dev);
		dev_info(&p_hall_data->hall_dev->dev,
			 "hall_interrupt_handler near\n");
	} else {
		input_report_key(p_hall_data->hall_dev, KEY_HALL, 1);
		input_sync(p_hall_data->hall_dev);
		dev_info(&p_hall_data->hall_dev->dev,
			 "hall_interrupt_handler far\n");
	}
}

static int hall_input_init(struct platform_device *pdev,
			   struct hall_data *data)
{
	int err;

	data->hall_dev = devm_input_allocate_device(&pdev->dev);
	if (!data->hall_dev) {
		dev_err(&pdev->dev, "input device allocation failed\n");
		return -EINVAL;
	}

	data->hall_dev->name = LID_DEV_NAME;
	data->hall_dev->phys = HALL_INPUT;
	set_bit(EV_KEY, data->hall_dev->evbit);
	set_bit(KEY_HALL, data->hall_dev->keybit);

	err = input_register_device(data->hall_dev);
	if (err < 0) {
		dev_err(&pdev->dev, "unable to register input device %s\n",
			LID_DEV_NAME);
		return err;
	}

	return 0;
}

static ssize_t extbd_status_store(const struct class *c,
				  const struct class_attribute *attr,
				  const char *buf, size_t count)
{
	if (kstrtoint(buf, 0, &g_extbd_status))
		return -EINVAL;

	return count;
}

static ssize_t extbd_status_show(const struct class *c,
				 const struct class_attribute *attr,
				 char *buf)
{
	g_extbd_status = (gpio_get_value_cansleep(p_hall_data->gpio) ? 1 : 0) ^
			 p_hall_data->active_low;

	return scnprintf(buf, PAGE_SIZE, "%d\n", g_extbd_status);
}

static CLASS_ATTR_RW(extbd_status);

static struct attribute *extbd_ctrl_class_attrs[] = {
	&class_attr_extbd_status.attr,
	NULL,
};

ATTRIBUTE_GROUPS(extbd_ctrl_class);

static int hall_driver_probe(struct platform_device *dev)
{
	struct hall_data *data;
	int err;
	int irq_flags;

	dev_info(&dev->dev, "hall_driver probe\n");

	data = devm_kzalloc(&dev->dev, sizeof(struct hall_data), GFP_KERNEL);
	if (data == NULL) {
		err = -ENOMEM;
		dev_err(&dev->dev, "failed to allocate memory %d\n", err);
		goto exit;
	}

	data->dev = &dev->dev;
	dev_set_drvdata(&dev->dev, data);

	err = hall_parse_dt(&dev->dev, data);
	if (err < 0) {
		dev_err(&dev->dev, "Failed to parse device tree\n");
		goto exit;
	}

	err = hall_input_init(dev, data);
	if (err < 0) {
		dev_err(&dev->dev, "input init failed\n");
		goto exit;
	}

	if (!gpio_is_valid(data->gpio)) {
		dev_err(&dev->dev, "gpio is not valid\n");
		err = -EINVAL;
		goto free_gpio;
	}

	irq_flags = IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING | IRQF_ONESHOT;
	err = gpio_request_one(data->gpio, GPIOF_IN, "hall_sensor_irq");
	if (err) {
		dev_err(&dev->dev, "unable to request gpio %d\n", data->gpio);
		goto exit;
	}

	data->irq = gpio_to_irq(data->gpio);
	err = devm_request_threaded_irq(&dev->dev, data->irq, NULL,
					hall_interrupt_handler,
					irq_flags, "hall_sensor", data);
	if (err < 0)
		goto free_irq;

	data->hall_wq = create_singlethread_workqueue("hall_wq");
	if (!data->hall_wq)
		return -EINVAL;

	INIT_DELAYED_WORK(&data->hall_debounce_work_det,
			  hall_det_debounce_work_func);

	/*
	 * err = sysfs_create_group(&data->dev->kobj, &hall_attr_group);
	 * if (err) {
	 *     printk(KERN_ERR "%s sysfs_create_group fail\n", __func__);
	 *     return err;
	 * }
	 */

	extbd_ctrl_class.name = "extbd_ctrl";
	extbd_ctrl_class.class_groups = extbd_ctrl_class_groups;
	err = class_register(&extbd_ctrl_class);
	if (err < 0)
		dev_err(&dev->dev, "Failed to create extbd_ctrl_class rc=%d\n",
			err);

	p_hall_data = data;
	device_init_wakeup(&dev->dev, data->wakeup);
	enable_irq_wake(data->irq);

	dev_info(&dev->dev, "guh hall probe end");

	return 0;

free_irq:
	disable_irq_wake(data->irq);
	device_init_wakeup(&dev->dev, 0);
free_gpio:
	gpio_free(data->gpio);
exit:
	return err;
}

static void hall_driver_remove(struct platform_device *dev)
{
	struct hall_data *data = dev_get_drvdata(&dev->dev);

	disable_irq_wake(data->irq);
	device_init_wakeup(&dev->dev, 0);
	if (data->gpio)
		gpio_free(data->gpio);
}

static int hall_driver_suspend(struct platform_device *dev,
			       pm_message_t state)
{
	/*
	 * struct hall_data *data = dev_get_drvdata(&dev->dev);
	 * gpio_direction_output(data->extbd_power, 0);
	 */

	return 0;
}

static int hall_driver_resume(struct platform_device *dev)
{
	/*
	 * struct hall_data *data = dev_get_drvdata(&dev->dev);
	 * gpio_direction_output(data->extbd_power, 1);
	 */

	return 0;
}

static struct platform_device_id hall_id[] = {
	{ LID_DEV_NAME, 0 },
	{ },
};

#ifdef CONFIG_OF
static const struct of_device_id hall_match_table[] = {
	{ .compatible = "hall-switch", },
	{ },
};
#endif

static struct platform_driver hall_driver = {
	.driver = {
		.name = LID_DEV_NAME,
		.owner = THIS_MODULE,
		.of_match_table = of_match_ptr(hall_match_table),
	},
	.probe = hall_driver_probe,
	.remove = hall_driver_remove,
	.suspend = hall_driver_suspend,
	.resume = hall_driver_resume,
	.id_table = hall_id,
};

static int __init hall_init(void)
{
	return platform_driver_register(&hall_driver);
}

static void __exit hall_exit(void)
{
	platform_driver_unregister(&hall_driver);
}

module_init(hall_init);
module_exit(hall_exit);

MODULE_DESCRIPTION("Hall sensor driver");
MODULE_LICENSE("GPL");
