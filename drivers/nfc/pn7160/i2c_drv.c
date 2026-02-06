// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2015, The Linux Foundation. All rights reserved.
 * Copyright (C) 2013-2021 NXP
 */

#include <linux/module.h>
#include <linux/interrupt.h>
#include <linux/delay.h>
#include <linux/uaccess.h>
#include <linux/gpio.h>
#include <linux/i2c.h>
#include <linux/pm.h>
#include <linux/fs.h>

#include "common.h"
#include "i2c_drv.h"

/**
 * i2c_disable_irq()
 *
 * Check if interrupt is disabled or not
 * and disable interrupt
 *
 * Return: int
 */
int i2c_disable_irq(struct nfc_dev *dev)
{
	struct i2c_dev *i2c_dev = dev->platform_data;
	unsigned long flags;

	if (!i2c_dev)
		return -EINVAL;

	spin_lock_irqsave(&i2c_dev->irq_enabled_lock, flags);
	if (i2c_dev->irq_enabled) {
		disable_irq_nosync(i2c_dev->client->irq);
		i2c_dev->irq_enabled = false;
	}
	spin_unlock_irqrestore(&i2c_dev->irq_enabled_lock, flags);

	return 0;
}
EXPORT_SYMBOL(i2c_disable_irq);

/**
 * i2c_enable_irq()
 *
 * Check if interrupt is enabled or not
 * and enable interrupt
 *
 * Return: int
 */
int i2c_enable_irq(struct nfc_dev *dev)
{
	struct i2c_dev *i2c_dev = dev->platform_data;
	unsigned long flags;

	if (!i2c_dev)
		return -EINVAL;

	spin_lock_irqsave(&i2c_dev->irq_enabled_lock, flags);
	if (!i2c_dev->irq_enabled) {
		i2c_dev->irq_enabled = true;
		enable_irq(i2c_dev->client->irq);
	}
	spin_unlock_irqrestore(&i2c_dev->irq_enabled_lock, flags);

	return 0;
}
EXPORT_SYMBOL(i2c_enable_irq);

static irqreturn_t i2c_irq_handler(int irq, void *dev_id)
{
	struct nfc_dev *nfc_dev = dev_id;
	struct i2c_dev *i2c_dev = nfc_dev->platform_data;

	if (!i2c_dev)
		return IRQ_NONE;

	if (device_may_wakeup(&i2c_dev->client->dev))
		pm_wakeup_event(&i2c_dev->client->dev, WAKEUP_SRC_TIMEOUT);

	i2c_disable_irq(nfc_dev);
	wake_up(&nfc_dev->read_wq);

	return IRQ_HANDLED;
}

int i2c_read(struct nfc_dev *nfc_dev, char *buf, size_t count, int timeout)
{
	struct i2c_dev *i2c_dev = nfc_dev->platform_data;
	struct platform_gpio *nfc_gpio = &nfc_dev->configs.gpio;
	int ret = -EINVAL;

	pr_debug("%s: reading %zu bytes.\n", __func__, count);

	if (!i2c_dev)
		return -EINVAL;

	if (timeout > NCI_CMD_RSP_TIMEOUT_MS)
		timeout = NCI_CMD_RSP_TIMEOUT_MS;

	if (count > MAX_NCI_BUFFER_SIZE)
		count = MAX_NCI_BUFFER_SIZE;

	if (!gpio_get_value(nfc_gpio->irq)) {
		while (1) {
			ret = 0;
			if (!i2c_dev->irq_enabled) {
				i2c_dev->irq_enabled = true;
				enable_irq(i2c_dev->client->irq);
			}
			if (!gpio_get_value(nfc_gpio->irq)) {
				if (timeout) {
					ret = wait_event_interruptible_timeout(
						nfc_dev->read_wq,
						!i2c_dev->irq_enabled,
						msecs_to_jiffies(timeout));

					if (ret <= 0) {
						pr_err("%s: timeout error\n",
						       __func__);
						goto err;
					}
				} else {
					ret = wait_event_interruptible(
						nfc_dev->read_wq,
						!i2c_dev->irq_enabled);
					if (ret) {
						pr_err("%s: err wakeup of wq\n",
						       __func__);
						goto err;
					}
				}
			}
			i2c_disable_irq(nfc_dev);

			if (gpio_get_value(nfc_gpio->irq))
				break;
			if (!gpio_get_value(nfc_gpio->ven)) {
				pr_info("%s: releasing read\n", __func__);
				ret = -EIO;
				goto err;
			}
			pr_warn("%s: spurious interrupt detected\n", __func__);
		}
	}

	memset(buf, 0x00, count);
	/* Read data */
	ret = i2c_master_recv(i2c_dev->client, buf, count);
	if (ret <= 0) {
		pr_err("%s: returned %d\n", __func__, ret);
		goto err;
	}
err:
	return ret;
}
EXPORT_SYMBOL(i2c_read);

int i2c_write(struct nfc_dev *nfc_dev, const char *buf, size_t count,
	      int max_retry_cnt)
{
	struct i2c_dev *i2c_dev = nfc_dev->platform_data;
	struct platform_gpio *nfc_gpio = &nfc_dev->configs.gpio;
	int ret = -EINVAL;
	int retry_cnt;

	if (!i2c_dev)
		return -EINVAL;

	if (count > MAX_DL_BUFFER_SIZE)
		count = MAX_DL_BUFFER_SIZE;

	pr_debug("%s: writing %zu bytes.\n", __func__, count);
	/*
	 * Wait for any pending read for max 15ms before write
	 * This is to avoid any packet corruption during read, when
	 * the host cmds resets NFCC during any parallel read operation
	 */
	for (retry_cnt = 1; retry_cnt <= MAX_WRITE_IRQ_COUNT; retry_cnt++) {
		if (gpio_get_value(nfc_gpio->irq)) {
			pr_warn("%s: irq high during write, wait\n", __func__);
			usleep_range(NFC_WRITE_IRQ_WAIT_TIME_US,
				     NFC_WRITE_IRQ_WAIT_TIME_US + 100);
		} else {
			break;
		}
		if (retry_cnt == MAX_WRITE_IRQ_COUNT &&
		    gpio_get_value(nfc_gpio->irq)) {
			pr_warn("%s: allow after maximum wait\n", __func__);
		}
	}

	for (retry_cnt = 1; retry_cnt <= max_retry_cnt; retry_cnt++) {
		ret = i2c_master_send(i2c_dev->client, buf, count);
		if (ret <= 0) {
			pr_warn("%s: write failed ret(%d), maybe in standby\n",
				__func__, ret);
			usleep_range(WRITE_RETRY_WAIT_TIME_US,
				     WRITE_RETRY_WAIT_TIME_US + 100);
		} else if (ret != count) {
			pr_err("%s: failed to write %d\n", __func__, ret);
			ret = -EIO;
		} else if (ret == count)
			break;
	}
	return ret;
}
EXPORT_SYMBOL(i2c_write);

ssize_t nfc_i2c_dev_read(struct file *filp, char __user *buf, size_t count,
			 loff_t *offset)
{
	int ret;
	struct nfc_dev *nfc_dev = filp->private_data;

	if (filp->f_flags & O_NONBLOCK) {
		pr_err("%s: f_flags has nonblock. try again\n", __func__);
		return -EAGAIN;
	}
	mutex_lock(&nfc_dev->read_mutex);
	ret = i2c_read(nfc_dev, nfc_dev->read_kbuf, count, 0);
	if (ret > 0) {
		if (copy_to_user(buf, nfc_dev->read_kbuf, ret)) {
			pr_warn("%s: failed to copy to user space\n", __func__);
			ret = -EFAULT;
		}
	}
	mutex_unlock(&nfc_dev->read_mutex);
	return ret;
}
EXPORT_SYMBOL(nfc_i2c_dev_read);

ssize_t nfc_i2c_dev_write(struct file *filp, const char __user *buf,
			  size_t count, loff_t *offset)
{
	int ret;
	struct nfc_dev *nfc_dev = filp->private_data;

	if (count > MAX_DL_BUFFER_SIZE)
		count = MAX_DL_BUFFER_SIZE;

	mutex_lock(&nfc_dev->write_mutex);
	if (copy_from_user(nfc_dev->write_kbuf, buf, count)) {
		pr_err("%s: failed to copy from user space\n", __func__);
		mutex_unlock(&nfc_dev->write_mutex);
		return -EFAULT;
	}
	ret = i2c_write(nfc_dev, nfc_dev->write_kbuf, count, NO_RETRY);
	mutex_unlock(&nfc_dev->write_mutex);
	return ret;
}
EXPORT_SYMBOL(nfc_i2c_dev_write);

static const struct file_operations nfc_i2c_dev_fops = {
	.owner = THIS_MODULE,
	.llseek = noop_llseek,
	.read = nfc_i2c_dev_read,
	.write = nfc_i2c_dev_write,
	.open = nfc_dev_open,
	.release = nfc_dev_close,
	.unlocked_ioctl = nfc_dev_ioctl,
};

int nfc_i2c_dev_probe(struct i2c_client *client)
{
	int ret = 0;
	struct nfc_dev *nfc_dev = NULL;
	struct i2c_dev *i2c_dev = NULL;
	struct platform_configs nfc_configs;
	struct platform_gpio *nfc_gpio = &nfc_configs.gpio;

	pr_debug("%s: enter\n", __func__);
	/* retrieve details of gpios from dt */
	ret = nfc_parse_dt(&client->dev, &nfc_configs, PLATFORM_IF_I2C);
	if (ret) {
		pr_err("%s: failed to parse dt\n", __func__);
		goto err;
	}

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {
		pr_err("%s: need I2C_FUNC_I2C\n", __func__);
		ret = -ENODEV;
		goto err;
	}

	/* 分配 nfc_dev 和 i2c_dev */
	nfc_dev = kzalloc(sizeof(struct nfc_dev), GFP_KERNEL);
	if (nfc_dev == NULL) {
		ret = -ENOMEM;
		goto err;
	}

	i2c_dev = kzalloc(sizeof(struct i2c_dev), GFP_KERNEL);
	if (i2c_dev == NULL) {
		ret = -ENOMEM;
		goto err_free_nfc_dev;
	}

	i2c_dev->client = client;
	nfc_dev->platform_data = i2c_dev;
	nfc_dev->interface = PLATFORM_IF_I2C;
	nfc_dev->nfc_state = NFC_STATE_NCI;

	nfc_dev->read_kbuf = kzalloc(MAX_NCI_BUFFER_SIZE, GFP_DMA | GFP_KERNEL);
	if (!nfc_dev->read_kbuf) {
		ret = -ENOMEM;
		goto err_free_i2c_dev;
	}

	nfc_dev->write_kbuf = kzalloc(MAX_DL_BUFFER_SIZE, GFP_DMA | GFP_KERNEL);
	if (!nfc_dev->write_kbuf) {
		ret = -ENOMEM;
		goto err_free_read_kbuf;
	}

	nfc_dev->nfc_read = i2c_read;
	nfc_dev->nfc_write = i2c_write;
	nfc_dev->nfc_enable_intr = i2c_enable_irq;
	nfc_dev->nfc_disable_intr = i2c_disable_irq;

	ret = configure_gpio(nfc_gpio->ven, GPIO_OUTPUT);
	if (ret) {
		pr_err("%s: unable to request nfc reset gpio [%d]\n", __func__,
		       nfc_gpio->ven);
		goto err_free_write_kbuf;
	}

	ret = configure_gpio(nfc_gpio->irq, GPIO_IRQ);
	if (ret <= 0) {
		pr_err("%s: unable to request nfc irq gpio [%d]\n", __func__,
		       nfc_gpio->irq);
		goto err_free_gpio;
	}

	client->irq = ret;
	ret = configure_gpio(nfc_gpio->dwl_req, GPIO_OUTPUT);
	if (ret) {
		pr_err("%s: unable to request nfc firm downl gpio [%d]\n",
		       __func__, nfc_gpio->dwl_req);
	}

	/* copy the retrieved gpio details from DT */
	memcpy(&nfc_dev->configs, &nfc_configs,
	       sizeof(struct platform_configs));

	/* init mutex and queues */
	init_waitqueue_head(&nfc_dev->read_wq);
	mutex_init(&nfc_dev->read_mutex);
	mutex_init(&nfc_dev->write_mutex);
	mutex_init(&nfc_dev->dev_ref_mutex);
	spin_lock_init(&i2c_dev->irq_enabled_lock);

	ret = nfc_misc_register(nfc_dev, &nfc_i2c_dev_fops, DEV_COUNT,
				NFC_CHAR_DEV_NAME, CLASS_NAME);
	if (ret) {
		pr_err("%s: nfc_misc_register failed\n", __func__);
		goto err_mutex_destroy;
	}

	/* interrupt initializations */
	pr_info("%s: requesting IRQ %d\n", __func__, client->irq);
	i2c_dev->irq_enabled = true;
	ret = request_irq(client->irq, i2c_irq_handler, IRQF_TRIGGER_HIGH,
			  client->name, nfc_dev);
	if (ret) {
		pr_err("%s: request_irq failed\n", __func__);
		goto err_nfc_misc_unregister;
	}

	i2c_disable_irq(nfc_dev);
	gpio_set_ven(nfc_dev, 1);
	gpio_set_ven(nfc_dev, 0);
	gpio_set_ven(nfc_dev, 1);
	device_init_wakeup(&client->dev, true);
	i2c_set_clientdata(client, nfc_dev);
	i2c_dev->irq_wake_up = false;

	pr_info("%s: probing nfc i2c successfully\n", __func__);
	return 0;

err_nfc_misc_unregister:
	nfc_misc_unregister(nfc_dev, DEV_COUNT);
err_mutex_destroy:
	mutex_destroy(&nfc_dev->dev_ref_mutex);
	mutex_destroy(&nfc_dev->read_mutex);
	mutex_destroy(&nfc_dev->write_mutex);
err_free_gpio:
	gpio_free_all(nfc_dev);
err_free_write_kbuf:
	kfree(nfc_dev->write_kbuf);
err_free_read_kbuf:
	kfree(nfc_dev->read_kbuf);
err_free_i2c_dev:
	kfree(i2c_dev);
err_free_nfc_dev:
	kfree(nfc_dev);
err:
	pr_err("%s: probing not successful, check hardware\n", __func__);
	return ret;
}
EXPORT_SYMBOL(nfc_i2c_dev_probe);

void nfc_i2c_dev_remove(struct i2c_client *client)
{
	struct nfc_dev *nfc_dev = i2c_get_clientdata(client);
	struct i2c_dev *i2c_dev;

	if (!nfc_dev) {
		pr_err("%s: device doesn't exist anymore\n", __func__);
		return;
	}

	i2c_dev = nfc_dev->platform_data;

	pr_info("%s: remove device\n", __func__);

	if (nfc_dev->dev_ref_count > 0) {
		pr_err("%s: device already in use\n", __func__);
		return;
	}

	device_init_wakeup(&client->dev, false);
	free_irq(client->irq, nfc_dev);
	nfc_misc_unregister(nfc_dev, DEV_COUNT);
	mutex_destroy(&nfc_dev->read_mutex);
	mutex_destroy(&nfc_dev->write_mutex);
	mutex_destroy(&nfc_dev->dev_ref_mutex);
	gpio_free_all(nfc_dev);
	kfree(nfc_dev->read_kbuf);
	kfree(nfc_dev->write_kbuf);
	kfree(i2c_dev);
	kfree(nfc_dev);
}
EXPORT_SYMBOL(nfc_i2c_dev_remove);

int nfc_i2c_dev_suspend(struct device *device)
{
	struct i2c_client *client = to_i2c_client(device);
	struct nfc_dev *nfc_dev = i2c_get_clientdata(client);
	struct i2c_dev *i2c_dev;

	if (!nfc_dev)
		return 0;

	i2c_dev = nfc_dev->platform_data;
	if (!i2c_dev)
		return 0;

	if (device_may_wakeup(&client->dev) && i2c_dev->irq_enabled) {
		if (!enable_irq_wake(client->irq))
			i2c_dev->irq_wake_up = true;
	}
	return 0;
}
EXPORT_SYMBOL(nfc_i2c_dev_suspend);

int nfc_i2c_dev_resume(struct device *device)
{
	struct i2c_client *client = to_i2c_client(device);
	struct nfc_dev *nfc_dev = i2c_get_clientdata(client);
	struct i2c_dev *i2c_dev;

	if (!nfc_dev)
		return 0;

	i2c_dev = nfc_dev->platform_data;
	if (!i2c_dev)
		return 0;

	if (device_may_wakeup(&client->dev) && i2c_dev->irq_wake_up) {
		if (!disable_irq_wake(client->irq))
			i2c_dev->irq_wake_up = false;
	}
	return 0;
}
EXPORT_SYMBOL(nfc_i2c_dev_resume);

static const struct i2c_device_id nfc_i2c_dev_id[] = {
	{ "nxpnfc", 0 },
	{}
};
MODULE_DEVICE_TABLE(i2c, nfc_i2c_dev_id);

static const struct of_device_id nfc_i2c_dev_match_table[] = {
	{ .compatible = "nxp,nxpnfc" },
	{}
};
MODULE_DEVICE_TABLE(of, nfc_i2c_dev_match_table);

static const struct dev_pm_ops nfc_i2c_dev_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(nfc_i2c_dev_suspend, nfc_i2c_dev_resume)
};

static struct i2c_driver nfc_i2c_dev_driver = {
	.id_table = nfc_i2c_dev_id,
	.probe = nfc_i2c_dev_probe,
	.remove = nfc_i2c_dev_remove,
	.driver = {
		.name = "nxpnfc",
		.pm = &nfc_i2c_dev_pm_ops,
		.of_match_table = nfc_i2c_dev_match_table,
		.probe_type = PROBE_PREFER_ASYNCHRONOUS,
	},
};

static int __init nfc_i2c_dev_init(void)
{
	int ret = 0;

	pr_info("%s: Loading NXP NFC I2C driver\n", __func__);
	ret = i2c_add_driver(&nfc_i2c_dev_driver);
	if (ret != 0)
		pr_err("%s: NFC I2C add driver error ret %d\n", __func__, ret);
	return ret;
}

module_init(nfc_i2c_dev_init);

static void __exit nfc_i2c_dev_exit(void)
{
	pr_info("%s: Unloading NXP NFC I2C driver\n", __func__);
	i2c_del_driver(&nfc_i2c_dev_driver);
}

module_exit(nfc_i2c_dev_exit);

MODULE_DESCRIPTION("NXP NFC I2C driver");
MODULE_LICENSE("GPL");
