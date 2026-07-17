// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2025-2026, ITE. All Rights Reserved
 *
 */
#include <linux/bits.h>
#include <linux/err.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/unaligned.h>

#include "ucsi.h"

#define ITEPD_UCSI_VERSION_REG	0x80
#define ITEPD_UCSI_CCI_REG	0x84
#define ITEPD_UCSI_MSG_IN_REG	0x88
#define ITEPD_UCSI_CONTROL_REG	0x98

#define ITEPD_VENDOR_WC_INT	0xbc
#define ITEPD_VENDOR_INT	0xbd
#define ITEPD_ALERT_VDM_EVENT	BIT(0)
#define ITEPD_ALERT_UCSI_EVENT	BIT(1)

#define ITEPD_MSG_IN_MAX_LEN	0x28

#define ITEPD_EVENT_NONE	0
#define ITEPD_EVENT_UCSI	1
#define ITEPD_EVENT_VDM		2

struct itepd {
	struct i2c_client *client;
	struct ucsi *ucsi;
	struct mutex i2c_lock;		/* Serializes I2C accesses */
	struct mutex event_lock;	/* Serializes event processing (IRQ vs poll) */
	struct mutex received_lock;	/* Protects cci and msg_in */
	u8 msg_in[ITEPD_MSG_IN_MAX_LEN];
	u32 cci;
};

static u8 ucsi_itepd_get_len(u32 cci)
{
	if (cci & UCSI_CCI_COMMAND_COMPLETE)
		return UCSI_CCI_LENGTH(cci);
	return 0;
}

static int itepd_read_reg(struct itepd *itepd, u8 reg, void *data, u32 len)
{
	struct i2c_client *client = itepd->client;
	struct i2c_msg msg[2];
	u8 *tx_buf, *rx_buf;
	int ret;

	tx_buf = kzalloc(1, GFP_KERNEL);
	if (!tx_buf)
		return -ENOMEM;

	rx_buf = kzalloc(len, GFP_KERNEL);
	if (!rx_buf) {
		kfree(tx_buf);
		return -ENOMEM;
	}

	tx_buf[0] = reg;

	msg[0].addr	= client->addr;
	msg[0].flags	= 0;
	msg[0].len	= 1;
	msg[0].buf	= tx_buf;

	msg[1].addr	= client->addr;
	msg[1].flags	= I2C_M_RD;
	msg[1].len	= len;
	msg[1].buf	= rx_buf;

	mutex_lock(&itepd->i2c_lock);
	ret = i2c_transfer(client->adapter, msg, ARRAY_SIZE(msg));
	mutex_unlock(&itepd->i2c_lock);
	if (ret < 0) {
		dev_err_ratelimited(&client->dev, "reg 0x%02x read failed: %d\n",
				    reg, ret);
		goto out_free;
	}
	if (ret != ARRAY_SIZE(msg)) {
		ret = -EIO;
		goto out_free;
	}

	memcpy(data, rx_buf, len);
	ret = 0;

out_free:
	kfree(rx_buf);
	kfree(tx_buf);
	return ret;
}

static int itepd_write_reg(struct itepd *itepd, u8 reg, const void *data, u32 len)
{
	struct i2c_client *client = itepd->client;
	struct i2c_msg msg[1];
	u8 *tx_buf;
	int ret;

	tx_buf = kzalloc(len + 1, GFP_KERNEL);
	if (!tx_buf)
		return -ENOMEM;

	tx_buf[0] = reg;
	memcpy(tx_buf + 1, data, len);

	msg[0].addr	= client->addr;
	msg[0].flags	= 0;
	msg[0].len	= len + 1;
	msg[0].buf	= tx_buf;

	mutex_lock(&itepd->i2c_lock);
	ret = i2c_transfer(client->adapter, msg, ARRAY_SIZE(msg));
	mutex_unlock(&itepd->i2c_lock);
	if (ret < 0) {
		dev_err_ratelimited(&client->dev, "reg 0x%02x write failed: %d\n",
				    reg, ret);
		goto out_free;
	}
	if (ret != ARRAY_SIZE(msg)) {
		ret = -EIO;
		goto out_free;
	}

	ret = 0;

out_free:
	kfree(tx_buf);
	return ret;
}

static int itepd_process_event(struct itepd *itepd, u32 *cci)
{
	u8 msg_in[ITEPD_MSG_IN_MAX_LEN] = {};
	__le32 le_cci;
	u8 event, ack;
	u8 len = 0;
	int ret;

	mutex_lock(&itepd->event_lock);

	ret = itepd_read_reg(itepd, ITEPD_VENDOR_INT, &event, sizeof(event));
	if (ret)
		goto out_unlock;

	event &= ITEPD_ALERT_VDM_EVENT | ITEPD_ALERT_UCSI_EVENT;
	if (!event) {
		mutex_lock(&itepd->received_lock);
		*cci = itepd->cci;
		mutex_unlock(&itepd->received_lock);
		ret = ITEPD_EVENT_NONE;
		goto out_unlock;
	}

	if (event & ITEPD_ALERT_UCSI_EVENT) {
		ret = itepd_read_reg(itepd, ITEPD_UCSI_CCI_REG, &le_cci,
				     sizeof(le_cci));
		if (ret)
			goto out_unlock;

		*cci = le32_to_cpu(le_cci);
		len = min_t(u8, ucsi_itepd_get_len(*cci), sizeof(msg_in));
		if (len) {
			ret = itepd_read_reg(itepd, ITEPD_UCSI_MSG_IN_REG,
					     msg_in, len);
			if (ret)
				goto out_unlock;
		}
	}

	ack = event;
	ret = itepd_write_reg(itepd, ITEPD_VENDOR_WC_INT, &ack, sizeof(ack));
	if (ret)
		goto out_unlock;

	if (event & ITEPD_ALERT_UCSI_EVENT) {
		mutex_lock(&itepd->received_lock);
		itepd->cci = *cci;
		memset(itepd->msg_in, 0, sizeof(itepd->msg_in));
		memcpy(itepd->msg_in, msg_in, len);
		mutex_unlock(&itepd->received_lock);
		ret = ITEPD_EVENT_UCSI;
	} else {
		mutex_lock(&itepd->received_lock);
		*cci = itepd->cci;
		mutex_unlock(&itepd->received_lock);
		ret = ITEPD_EVENT_VDM;
	}

out_unlock:
	mutex_unlock(&itepd->event_lock);
	return ret;
}

static int ucsi_itepd_read_version(struct ucsi *ucsi, u16 *version)
{
	struct itepd *itepd = ucsi_get_drvdata(ucsi);
	__le16 le_version;
	int ret;

	ret = itepd_read_reg(itepd, ITEPD_UCSI_VERSION_REG, &le_version,
			     sizeof(le_version));
	if (ret)
		return ret;

	*version = le16_to_cpu(le_version);

	return 0;
}

static int ucsi_itepd_read_cci(struct ucsi *ucsi, u32 *cci)
{
	struct itepd *itepd = ucsi_get_drvdata(ucsi);

	mutex_lock(&itepd->received_lock);
	*cci = itepd->cci;
	mutex_unlock(&itepd->received_lock);

	return 0;
}

static int ucsi_itepd_poll_cci(struct ucsi *ucsi, u32 *cci)
{
	struct itepd *itepd = ucsi_get_drvdata(ucsi);
	int ret;

	ret = itepd_process_event(itepd, cci);
	return ret < 0 ? ret : 0;
}

static int ucsi_itepd_read_message_in(struct ucsi *ucsi, void *val, size_t val_len)
{
	struct itepd *itepd = ucsi_get_drvdata(ucsi);

	mutex_lock(&itepd->received_lock);
	memcpy(val, itepd->msg_in, min(val_len, sizeof(itepd->msg_in)));
	mutex_unlock(&itepd->received_lock);

	return 0;
}

static int ucsi_itepd_async_control(struct ucsi *ucsi, u64 command)
{
	struct itepd *itepd = ucsi_get_drvdata(ucsi);
	__le64 le_cmd = cpu_to_le64(command);

	if (UCSI_COMMAND(command) == UCSI_PPM_RESET) {
		mutex_lock(&itepd->received_lock);
		itepd->cci = 0;
		mutex_unlock(&itepd->received_lock);
	}

	return itepd_write_reg(itepd, ITEPD_UCSI_CONTROL_REG, &le_cmd,
			       sizeof(le_cmd));
}

static const struct ucsi_operations ucsi_itepd_ops = {
	.read_version		= ucsi_itepd_read_version,
	.read_cci		= ucsi_itepd_read_cci,
	.poll_cci		= ucsi_itepd_poll_cci,
	.read_message_in	= ucsi_itepd_read_message_in,
	.sync_control		= ucsi_sync_control_common,
	.async_control		= ucsi_itepd_async_control,
};

static irqreturn_t itepd_irq_process(struct itepd *itepd)
{
	u32 cci;
	int ret;

	ret = itepd_process_event(itepd, &cci);
	if (ret < 0 || ret == ITEPD_EVENT_NONE)
		return IRQ_NONE;

	if (ret == ITEPD_EVENT_UCSI)
		ucsi_notify_common(itepd->ucsi, cci);

	return IRQ_HANDLED;
}

static irqreturn_t itepd_irq_thread_fn(int irq, void *data)
{
	struct itepd *itepd = data;

	return itepd_irq_process(itepd);
}

static int itepd_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct itepd *itepd;
	u32 cci;
	int ret;

	if (client->irq <= 0)
		return dev_err_probe(dev, -ENODEV, "no IRQ provided\n");

	itepd = devm_kzalloc(dev, sizeof(*itepd), GFP_KERNEL);
	if (!itepd)
		return -ENOMEM;

	itepd->client = client;
	mutex_init(&itepd->i2c_lock);
	mutex_init(&itepd->event_lock);
	mutex_init(&itepd->received_lock);
	i2c_set_clientdata(client, itepd);

	itepd->ucsi = ucsi_create(dev, &ucsi_itepd_ops);
	if (IS_ERR(itepd->ucsi))
		return PTR_ERR(itepd->ucsi);

	ucsi_set_drvdata(itepd->ucsi, itepd);

	ret = request_threaded_irq(client->irq, NULL, itepd_irq_thread_fn,
				   IRQF_ONESHOT, dev_name(dev), itepd);
	if (ret) {
		dev_err(dev, "request_threaded_irq failed - %d\n", ret);
		goto out_ucsi_destroy;
	}

	itepd_process_event(itepd, &cci);

	ret = ucsi_register(itepd->ucsi);
	if (ret) {
		dev_err(dev, "failed to register UCSI: %d\n", ret);
		goto out_free_irq;
	}

	return 0;

out_free_irq:
	free_irq(client->irq, itepd);
out_ucsi_destroy:
	ucsi_destroy(itepd->ucsi);
	return ret;
}

static void itepd_remove(struct i2c_client *client)
{
	struct itepd *itepd = i2c_get_clientdata(client);

	ucsi_unregister(itepd->ucsi);
	free_irq(client->irq, itepd);
	ucsi_destroy(itepd->ucsi);
}

static const struct of_device_id itepd_of_match_table[] = {
	{ .compatible = "ite,it8851" },
	{ .compatible = "ite,it8853" },
	{}
};
MODULE_DEVICE_TABLE(of, itepd_of_match_table);

static const struct i2c_device_id itepd_id_table[] = {
	{ "ucsi_itepd", 0 },
	{}
};
MODULE_DEVICE_TABLE(i2c, itepd_id_table);

static struct i2c_driver itepd_driver = {
	.driver = {
		.name = "ucsi_itepd",
		.of_match_table = itepd_of_match_table,
	},
	.probe = itepd_probe,
	.remove = itepd_remove,
	.id_table = itepd_id_table,
};
module_i2c_driver(itepd_driver);

MODULE_AUTHOR("Jeson Yang <jeson.yang@ite.com.tw>");
MODULE_DESCRIPTION("UCSI driver for ITE IT8851 and IT8853 Type-C PD controllers");
MODULE_LICENSE("GPL");
