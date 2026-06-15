// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2025-2026, ITE. All Rights Reserved
 */
#include <linux/auxiliary_bus.h>
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/gpio/consumer.h>

#include "itepd.h"

#define ITEPD_UCSI_VERSION_REG			(0x80)
#define ITEPD_UCSI_CCI_REG				(0x84)
#define ITEPD_UCSI_MSG_IN_REG			(0x88)
#define ITEPD_UCSI_CONTROL_REG			(0x98)
#define ITEPD_UCSI_MSG_OUT_REG			(0xA0)

#define ITEPD_VENDOR_WC_INT				(0xBC)
#define ITEPD_VENDOR_INT				(0xBD)
	#define ITEPD_ALERT_VDM_EVENT			BIT(0)
	#define ITEPD_ALERT_UCSI_EVENT			BIT(1)

struct itepd {
	struct device *dev;
	struct i2c_client *client;
	int irq;
	struct mutex i2c_lock; /* Protects I2C read/write operations */
	struct mutex cb_lock;  /* Protects concurrent access to callback state */
	unsigned long client_mask;
	struct auxiliary_device *ucsi_aux;
	struct auxiliary_device *altmode_aux;
	struct itepd_ucsi_cb *ucsi_cb;
	struct itepd_altmode_cb *altmode_cb;

	struct itepd_altmode_data altmode_data[ITEPD_MAX_PORTS];
};

/*
 * ITE Read/Write Function
 */

static int itepd_read_reg(struct itepd *itepd, u8 reg, void *data, u32 len)
{
	struct i2c_client *client = itepd->client;
	struct i2c_msg msg[] = {
		{
			.addr	= client->addr,
			.flags	= 0x0,
			.len	= 1,
			.buf	= &reg,
		},
		{
			.addr	= client->addr,
			.flags	= I2C_M_RD,
			.len	= len,
			.buf	= (u8 *)data,
		}
	};
	int ret;

	ret = i2c_transfer(client->adapter, msg, ARRAY_SIZE(msg));
	if (ret < 0) {
		dev_err(itepd->dev, "i2c_transfer read failed %d\n", ret);
		return ret;
	}

	return 0;
}

static int itepd_write_reg(struct itepd *itepd, u8 reg, const void *data, u32 len)
{
	struct i2c_client *client = itepd->client;
	unsigned char *buf;
	struct i2c_msg msg[] = {
		{
			.addr	= client->addr,
			.flags	= 0x0,
		}
	};
	int ret;

	buf = kzalloc(len + sizeof(reg), GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	buf[0] = reg;
	memcpy(buf + sizeof(reg), (u8 *)data, len);

	msg[0].len = len + sizeof(reg);
	msg[0].buf = buf;

	ret = i2c_transfer(client->adapter, msg, ARRAY_SIZE(msg));
	if (ret < 0) {
		dev_err(itepd->dev, "i2c_transfer write failed %d\n", ret);
		kfree(buf);
		return ret;
	}

	kfree(buf);
	return 0;
}

/**
 * itepd_cmd_receive() - Receive UCSI command from ITEPD controller
 * @dev: Pointer to the device structure
 * @cmd: The command to be executed
 * @val: Buffer to store the received data
 * @val_len: Length of the buffer
 *
 * Return: 0 on success, or a negative error code on failure.
 */

int itepd_cmd_receive(struct device *dev, unsigned int cmd, void *val, size_t val_len)
{
	struct itepd *itepd = i2c_get_clientdata(to_i2c_client(dev->parent));
	int ret;

	if (!itepd)
		return -ENXIO;

	switch (cmd) {
	case ITEPD_RECEIVE_UCSI_VERSION:
		mutex_lock(&itepd->i2c_lock);
		ret = itepd_read_reg(itepd, ITEPD_UCSI_VERSION_REG, val,
				     min_t(size_t, val_len, 0x28));
		mutex_unlock(&itepd->i2c_lock);
		break;
	default:
		return -EINVAL;
	}

	return ret;
}
EXPORT_SYMBOL_GPL(itepd_cmd_receive);

int itepd_cmd_send(struct device *dev, unsigned int cmd, const void *val, size_t val_len)
{
	struct itepd *itepd = i2c_get_clientdata(to_i2c_client(dev->parent));
	int ret;

	if (!itepd)
		return -ENXIO;

	switch (cmd) {
	case ITEPD_SEND_UCSI_CONTROL:
		mutex_lock(&itepd->i2c_lock);
		ret = itepd_write_reg(itepd, ITEPD_UCSI_CONTROL_REG, val,
				      min_t(size_t, val_len, 8));
		mutex_unlock(&itepd->i2c_lock);
		break;
	case ITEPD_SEND_UCSI_MESSAGE_OUT:
		mutex_lock(&itepd->i2c_lock);
		ret = itepd_write_reg(itepd, ITEPD_UCSI_MSG_OUT_REG, val, val_len);
		mutex_unlock(&itepd->i2c_lock);
		break;
	default:
		return -EINVAL;
	}

	return ret;
}
EXPORT_SYMBOL_GPL(itepd_cmd_send);

int itepd_register_cb(struct device *dev, u8 id, void *cb)
{
	struct itepd *itepd = i2c_get_clientdata(to_i2c_client(dev->parent));

	if (!itepd)
		return -EPROBE_DEFER;

	switch (id) {
	case ITEPD_CLIENT_UCSI:
		if (itepd->ucsi_aux && dev == &itepd->ucsi_aux->dev) {
			mutex_lock(&itepd->cb_lock);
			itepd->ucsi_cb = (struct itepd_ucsi_cb *)cb;
			mutex_unlock(&itepd->cb_lock);
		} else {
			return -ENODEV;
		}
		break;
	case ITEPD_CLIENT_ALTMODE:
		if (itepd->altmode_aux && dev == &itepd->altmode_aux->dev) {
			mutex_lock(&itepd->cb_lock);
			itepd->altmode_cb = (struct itepd_altmode_cb *)cb;
			mutex_unlock(&itepd->cb_lock);
		} else {
			return -ENODEV;
		}
		break;
	default:
		return -EINVAL;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(itepd_register_cb);

int itepd_mode(struct device *dev, u8 port, u8 mux, u32 config, u32 status)
{
	struct itepd *itepd = i2c_get_clientdata(to_i2c_client(dev->parent));

	if (!itepd)
		return -ENXIO;

	if (itepd->ucsi_aux && dev == &itepd->ucsi_aux->dev) {
		itepd->altmode_data[port].port = port;
		itepd->altmode_data[port].mux = mux;
		itepd->altmode_data[port].dp_config = config;
		itepd->altmode_data[port].dp_status = status;
		if (itepd->altmode_cb)
			itepd->altmode_cb->notify(itepd->altmode_cb->priv,
						  &itepd->altmode_data[port]);
	} else {
		return -ENODEV;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(itepd_mode);

int itepd_hpd(struct device *dev, u8 port, u32 status)
{
	struct itepd *itepd = i2c_get_clientdata(to_i2c_client(dev->parent));

	if (!itepd)
		return -ENXIO;

	if (itepd->ucsi_aux && dev == &itepd->ucsi_aux->dev) {
		itepd->altmode_data[port].dp_status = status;
		if (itepd->altmode_cb)
			itepd->altmode_cb->notify(itepd->altmode_cb->priv,
						  &itepd->altmode_data[port]);
	} else {
		return -ENODEV;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(itepd_hpd);

/*
 * ITE Interrupt Function
 */

static irqreturn_t itepd_irq_process(struct itepd *itepd)
{
	u8 event;
	u8 clear = 0;
	u8 len;
	u32 cci;
	u8 msg_in[0x28];
	bool notify_ucsi = false;
	int ret;

	mutex_lock(&itepd->i2c_lock);

	ret = itepd_read_reg(itepd, ITEPD_VENDOR_INT, &event, 1);
	if (ret) {
		mutex_unlock(&itepd->i2c_lock);
		return IRQ_HANDLED;
	}

	mutex_lock(&itepd->cb_lock);
	if (event & ITEPD_ALERT_VDM_EVENT)
		clear |= ITEPD_ALERT_VDM_EVENT;

	if (event & ITEPD_ALERT_UCSI_EVENT) {
		clear |= ITEPD_ALERT_UCSI_EVENT;
		if (itepd->ucsi_cb) {
			ret = itepd_read_reg(itepd, ITEPD_UCSI_CCI_REG, &cci, sizeof(cci));
			if (ret)
				goto err_mutex_unlock_cb;
			len = itepd->ucsi_cb->get_len(itepd->ucsi_cb->priv, cci);

			if (len > 0) {
				ret = itepd_read_reg(itepd, ITEPD_UCSI_MSG_IN_REG, msg_in,
						     min_t(size_t, len, 0x28));
				if (ret)
					goto err_mutex_unlock_cb;
			}
			notify_ucsi = true;
		}
	}

	if (clear) {
		ret = itepd_write_reg(itepd, ITEPD_VENDOR_WC_INT, &clear, 1);
		if (ret)
			goto err_mutex_unlock_cb;
	}

	if (notify_ucsi)
		itepd->ucsi_cb->notify(itepd->ucsi_cb->priv, cci, msg_in);

	mutex_unlock(&itepd->cb_lock);
	mutex_unlock(&itepd->i2c_lock);

	return IRQ_HANDLED;

err_mutex_unlock_cb:
	mutex_unlock(&itepd->cb_lock);
	clear = (ITEPD_ALERT_VDM_EVENT | ITEPD_ALERT_UCSI_EVENT);
	itepd_write_reg(itepd, ITEPD_VENDOR_WC_INT, &clear, 1);
	mutex_unlock(&itepd->i2c_lock);
	return IRQ_HANDLED;
}

static irqreturn_t itepd_irq_thread_fn(int irq, void *data)
{
	struct itepd *itepd = data;

	return itepd_irq_process(itepd);
}

/*
 * ITE AUX Function
 */
static void itepd_ucsi_aux_release(struct device *dev)
{
	struct auxiliary_device *adev = container_of(dev, struct auxiliary_device, dev);

	of_node_put(dev->of_node);
	kfree(adev);
}

static void itepd_altmode_aux_release(struct device *dev)
{
	struct auxiliary_device *adev = container_of(dev, struct auxiliary_device, dev);

	of_node_put(dev->of_node);
	kfree(adev);
}

static int itepd_add_aux_device(struct itepd *itepd,
				struct auxiliary_device **aux_out,
				const char *name,
				void (*release)(struct device *))
{
	struct auxiliary_device *aux;
	int ret;

	aux = kzalloc_obj(*aux, GFP_KERNEL);
	if (!aux)
		return -ENOMEM;

	aux->name = name;
	aux->dev.parent = itepd->dev;
	aux->dev.release = release;
	device_set_of_node_from_dev(&aux->dev, itepd->dev);

	ret = auxiliary_device_init(aux);
	if (ret) {
		of_node_put(aux->dev.of_node);
		kfree(aux);
		return ret;
	}

	ret = auxiliary_device_add(aux);
	if (ret) {
		auxiliary_device_uninit(aux);
		return ret;
	}

	*aux_out = aux;
	return 0;
}

static void itepd_del_aux_device(struct auxiliary_device *aux)
{
	auxiliary_device_delete(aux);
	auxiliary_device_uninit(aux);
}

/*
 * ITE Probe/Remove
 */

static int itepd_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct itepd *itepd;
	const unsigned long *match_data;
	struct gpio_desc *desc;
	int ret;

	itepd = devm_kzalloc(dev, sizeof(struct itepd), GFP_KERNEL);
	if (!itepd)
		return -ENOMEM;

	itepd->dev = dev;
	itepd->client = client;
	itepd->irq = client->irq;
	mutex_init(&itepd->i2c_lock);
	mutex_init(&itepd->cb_lock);

	match_data = (unsigned long *)of_device_get_match_data(dev);

	if (!match_data)
		return -EINVAL;
	itepd->client_mask = *match_data;

	i2c_set_clientdata(client, itepd);

	if (itepd->irq > 0) {
		ret = request_threaded_irq(itepd->irq, NULL, itepd_irq_thread_fn,
					   IRQF_TRIGGER_FALLING | IRQF_ONESHOT,
					   dev_name(dev), itepd);
		if (ret < 0) {
			dev_err(dev, "request_threaded_irq failed - %d\n", ret);
			return ret;
		}
	}

	desc = devm_gpiod_get(dev, NULL, GPIOD_IN);

	if (IS_ERR(desc)) {
		dev_info(dev, "get gpios from DTS failed\n");
	} else {
		if (gpiod_get_value(desc))
			itepd_irq_process(itepd);
	}

	if (itepd->client_mask & BIT(ITEPD_CLIENT_ALTMODE)) {
		ret = itepd_add_aux_device(itepd, &itepd->altmode_aux, "altmode",
					   itepd_altmode_aux_release);
		if (ret)
			goto out_free_irq;
	}

	if (itepd->client_mask & BIT(ITEPD_CLIENT_UCSI)) {
		ret = itepd_add_aux_device(itepd, &itepd->ucsi_aux, "ucsi", itepd_ucsi_aux_release);
		if (ret)
			goto out_release_altmode_aux;
	}

	return 0;

out_release_altmode_aux:
	if (itepd->client_mask & BIT(ITEPD_CLIENT_ALTMODE))
		itepd_del_aux_device(itepd->altmode_aux);
out_free_irq:
	free_irq(itepd->irq, itepd);
	return ret;
}

static void itepd_remove(struct i2c_client *client)
{
	struct itepd *itepd = i2c_get_clientdata(client);

	if (itepd->client_mask & BIT(ITEPD_CLIENT_UCSI))
		itepd_del_aux_device(itepd->ucsi_aux);
	if (itepd->client_mask & BIT(ITEPD_CLIENT_ALTMODE))
		itepd_del_aux_device(itepd->altmode_aux);
	free_irq(itepd->irq, itepd);
}

static const unsigned long itepd_rb3gen2_client_mask =
	BIT(ITEPD_CLIENT_ALTMODE) | BIT(ITEPD_CLIENT_UCSI);

static const struct of_device_id itepd_of_match_table[] = {
	{ .compatible = "ite,itepd-it885x", .data = &itepd_rb3gen2_client_mask },
	{}
};
MODULE_DEVICE_TABLE(of, itepd_of_match_table);

static struct i2c_driver itepd_driver = {
	.driver = {
		.name = "itepd",
		.of_match_table = itepd_of_match_table,
	},
	.probe = itepd_probe,
	.remove = itepd_remove,
};

module_i2c_driver(itepd_driver);

MODULE_AUTHOR("Jeson Yang <jeson.yang@ite.com.tw>");
MODULE_DESCRIPTION("ITEPD driver for ITE Type-C PD controller");
MODULE_LICENSE("GPL");
