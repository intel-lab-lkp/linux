// SPDX-License-Identifier: GPL-2.0-only
/*
 * UCSI I2C transport driver for ITE885x USB-C controllers
 *
 * ITE8853/ITE8800-ITE8805 are UCSI-compliant USB-C controllers found on
 * desktop motherboards. They communicate over I2C using UCSI registers at
 * ITE-specific offsets and signal events through a vendor interrupt register.
 */

#include <linux/acpi.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/mutex.h>

#include "ucsi.h"

#define ITE_REG_CCI		0x84
#define ITE_REG_MESSAGE_IN	0x88
#define ITE_REG_CONTROL		0x98
#define ITE_REG_INT_ACK		0xbc
#define ITE_REG_INT_STATUS	0xbd

#define ITE_INT_VENDOR_ALERT	BIT(0)
#define ITE_INT_CCI		BIT(1)
#define ITE_INT_MASK		(ITE_INT_VENDOR_ALERT | ITE_INT_CCI)

#define ITE_MESSAGE_IN_MAX_LEN	0x10

enum ucsi_ite_event {
	ITE_EVENT_NONE,
	ITE_EVENT_CCI,
	ITE_EVENT_VENDOR,
};

struct ucsi_ite {
	struct i2c_client *client;
	struct ucsi *ucsi;
	struct mutex event_lock;	/* Serializes IRQ and polling */
	struct mutex received_lock;	/* Protects CCI and message_in */
	u8 message_in[ITE_MESSAGE_IN_MAX_LEN];
	u32 cci;
	bool registered;
};

static int ucsi_ite_read(struct ucsi_ite *ite, u8 reg, void *val, size_t len)
{
	struct i2c_client *client = ite->client;
	struct i2c_msg msgs[] = {
		{
			.addr = client->addr,
			.len = 1,
			.buf = &reg,
		},
		{
			.addr = client->addr,
			.flags = I2C_M_RD,
			.len = len,
			.buf = val,
		},
	};
	int ret;

	ret = i2c_transfer(client->adapter, msgs, ARRAY_SIZE(msgs));
	if (ret == ARRAY_SIZE(msgs))
		return 0;

	ret = ret < 0 ? ret : -EIO;
	dev_err_ratelimited(&client->dev,
			    "register 0x%02x read failed: %d\n", reg, ret);
	return ret;
}

static int ucsi_ite_write(struct ucsi_ite *ite, u8 reg, const void *val,
			  size_t len)
{
	struct i2c_client *client = ite->client;
	u8 buf[sizeof(u64) + 1];
	struct i2c_msg msg = {
		.addr = client->addr,
		.len = len + 1,
		.buf = buf,
	};
	int ret;

	if (len > sizeof(buf) - 1)
		return -EINVAL;

	buf[0] = reg;
	memcpy(&buf[1], val, len);

	ret = i2c_transfer(client->adapter, &msg, 1);
	if (ret == 1)
		return 0;

	ret = ret < 0 ? ret : -EIO;
	dev_err_ratelimited(&client->dev,
			    "register 0x%02x write failed: %d\n", reg, ret);
	return ret;
}

static int ucsi_ite_process_event(struct ucsi_ite *ite, u32 *cci)
{
	u8 message_in[ITE_MESSAGE_IN_MAX_LEN] = {};
	__le32 raw_cci;
	u8 status;
	u8 len = 0;
	int event;
	int err = 0;
	int ret;

	mutex_lock(&ite->event_lock);

	ret = ucsi_ite_read(ite, ITE_REG_INT_STATUS, &status, sizeof(status));
	if (ret)
		goto out_unlock;

	status &= ITE_INT_MASK;
	if (!status) {
		mutex_lock(&ite->received_lock);
		*cci = ite->cci;
		mutex_unlock(&ite->received_lock);
		ret = ITE_EVENT_NONE;
		goto out_unlock;
	}

	if (status & ITE_INT_CCI) {
		err = ucsi_ite_read(ite, ITE_REG_CCI, &raw_cci,
				    sizeof(raw_cci));
		if (!err) {
			*cci = le32_to_cpu(raw_cci);
			len = UCSI_CCI_LENGTH(*cci);

			if (len > sizeof(message_in)) {
				len = sizeof(message_in);
				*cci &= ~GENMASK(15, 8);
				*cci |= UCSI_SET_CCI_LENGTH(len);
			}
			if (len) {
				err = ucsi_ite_read(ite, ITE_REG_MESSAGE_IN,
						    message_in, len);
			}
		}
	}

	/* Acknowledge each latched event with the value expected by the PPM. */
	if (status & ITE_INT_VENDOR_ALERT) {
		u8 ack = ITE_INT_VENDOR_ALERT;

		ret = ucsi_ite_write(ite, ITE_REG_INT_ACK, &ack, sizeof(ack));
		if (ret)
			goto out_unlock;
	}

	if ((status & ITE_INT_CCI) && !err) {
		u8 ack = ITE_INT_CCI;

		ret = ucsi_ite_write(ite, ITE_REG_INT_ACK, &ack, sizeof(ack));
		if (ret)
			goto out_unlock;
	}

	if (err) {
		ret = err;
		goto out_unlock;
	}

	if (status & ITE_INT_CCI) {
		mutex_lock(&ite->received_lock);
		ite->cci = *cci;
		memset(ite->message_in, 0, sizeof(ite->message_in));
		memcpy(ite->message_in, message_in, len);
		mutex_unlock(&ite->received_lock);
		event = ITE_EVENT_CCI;
	} else {
		mutex_lock(&ite->received_lock);
		*cci = ite->cci;
		mutex_unlock(&ite->received_lock);
		event = ITE_EVENT_VENDOR;
	}

	ret = event;

out_unlock:
	mutex_unlock(&ite->event_lock);
	return ret;
}

static int ucsi_ite_read_version(struct ucsi *ucsi, u16 *version)
{
	/* The ITE interface does not expose a VERSION register. */
	*version = UCSI_VERSION_1_0;
	return 0;
}

static int ucsi_ite_read_cci(struct ucsi *ucsi, u32 *cci)
{
	struct ucsi_ite *ite = ucsi_get_drvdata(ucsi);

	mutex_lock(&ite->received_lock);
	*cci = ite->cci;
	mutex_unlock(&ite->received_lock);

	return 0;
}

static int ucsi_ite_poll_cci(struct ucsi *ucsi, u32 *cci)
{
	struct ucsi_ite *ite = ucsi_get_drvdata(ucsi);
	int ret;

	ret = ucsi_ite_process_event(ite, cci);
	return ret < 0 ? ret : 0;
}

static int ucsi_ite_read_message_in(struct ucsi *ucsi, void *val, size_t len)
{
	struct ucsi_ite *ite = ucsi_get_drvdata(ucsi);

	if (len > sizeof(ite->message_in))
		return -EINVAL;

	mutex_lock(&ite->received_lock);
	memcpy(val, ite->message_in, len);
	mutex_unlock(&ite->received_lock);

	return 0;
}

static int ucsi_ite_async_control(struct ucsi *ucsi, u64 command)
{
	struct ucsi_ite *ite = ucsi_get_drvdata(ucsi);
	__le64 raw_command = cpu_to_le64(command);
	int ret;

	if (UCSI_COMMAND(command) == UCSI_PPM_RESET) {
		/* The PPM handles reset internally; do not write it over I2C. */
		mutex_lock(&ite->event_lock);
		mutex_lock(&ite->received_lock);
		ite->cci = UCSI_CCI_RESET_COMPLETE;
		memset(ite->message_in, 0, sizeof(ite->message_in));
		mutex_unlock(&ite->received_lock);
		mutex_unlock(&ite->event_lock);
		return 0;
	}

	mutex_lock(&ite->event_lock);
	mutex_lock(&ite->received_lock);
	ite->cci = 0;
	memset(ite->message_in, 0, sizeof(ite->message_in));
	mutex_unlock(&ite->received_lock);
	ret = ucsi_ite_write(ite, ITE_REG_CONTROL, &raw_command,
			     sizeof(raw_command));
	mutex_unlock(&ite->event_lock);

	return ret;
}

static const struct ucsi_operations ucsi_ite_ops = {
	.read_version = ucsi_ite_read_version,
	.read_cci = ucsi_ite_read_cci,
	.poll_cci = ucsi_ite_poll_cci,
	.read_message_in = ucsi_ite_read_message_in,
	.sync_control = ucsi_sync_control_common,
	.async_control = ucsi_ite_async_control,
};

static irqreturn_t ucsi_ite_irq(int irq, void *data)
{
	struct ucsi_ite *ite = data;
	u32 cci;
	int ret;

	ret = ucsi_ite_process_event(ite, &cci);
	if (ret == ITE_EVENT_NONE)
		return IRQ_NONE;
	if (ret < 0)
		return IRQ_HANDLED;

	if (ret == ITE_EVENT_CCI)
		ucsi_notify_common(ite->ucsi, cci);

	return IRQ_HANDLED;
}

static void ucsi_ite_destroy(void *data)
{
	struct ucsi_ite *ite = data;

	if (ite->registered)
		ucsi_unregister(ite->ucsi);
	ucsi_destroy(ite->ucsi);
}

static int ucsi_ite_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct ucsi_ite *ite;
	u32 cci;
	int ret;

	if (client->irq <= 0)
		return dev_err_probe(dev, -ENODEV, "no IRQ provided\n");
	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C))
		return dev_err_probe(dev, -EOPNOTSUPP,
				     "adapter does not support I2C transfers\n");

	ite = devm_kzalloc(dev, sizeof(*ite), GFP_KERNEL);
	if (!ite)
		return -ENOMEM;

	ite->client = client;
	mutex_init(&ite->event_lock);
	mutex_init(&ite->received_lock);
	i2c_set_clientdata(client, ite);

	ite->ucsi = ucsi_create(dev, &ucsi_ite_ops);
	if (IS_ERR(ite->ucsi))
		return dev_err_probe(dev, PTR_ERR(ite->ucsi),
				     "failed to create UCSI interface\n");

	ret = devm_add_action_or_reset(dev, ucsi_ite_destroy, ite);
	if (ret)
		return ret;

	ucsi_set_drvdata(ite->ucsi, ite);

	ret = devm_request_threaded_irq(dev, client->irq, NULL,
					ucsi_ite_irq, IRQF_ONESHOT,
					dev_name(dev), ite);
	if (ret)
		return dev_err_probe(dev, ret, "failed to request IRQ\n");

	ret = ucsi_ite_process_event(ite, &cci);
	if (ret < 0)
		dev_warn(dev, "initial event processing failed: %d\n", ret);

	ret = ucsi_register(ite->ucsi);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to register UCSI interface\n");

	ite->registered = true;
	return 0;
}

static int ucsi_ite_suspend(struct device *dev)
{
	struct ucsi_ite *ite = dev_get_drvdata(dev);
	int ret;

	disable_irq(ite->client->irq);
	ret = ucsi_suspend(ite->ucsi);
	if (ret)
		enable_irq(ite->client->irq);

	return ret;
}

static int ucsi_ite_resume(struct device *dev)
{
	struct ucsi_ite *ite = dev_get_drvdata(dev);

	enable_irq(ite->client->irq);
	return ucsi_resume(ite->ucsi);
}

static DEFINE_SIMPLE_DEV_PM_OPS(ucsi_ite_pm, ucsi_ite_suspend,
				ucsi_ite_resume);

static const struct acpi_device_id ucsi_ite_acpi_ids[] = {
	{ "ITE8853" },
	{ "ITE8800" },
	{ "ITE8801" },
	{ "ITE8802" },
	{ "ITE8803" },
	{ "ITE8804" },
	{ "ITE8805" },
	{ }
};
MODULE_DEVICE_TABLE(acpi, ucsi_ite_acpi_ids);

static struct i2c_driver ucsi_ite_driver = {
	.driver = {
		.name = "ucsi_ite",
		.acpi_match_table = ucsi_ite_acpi_ids,
		.pm = pm_sleep_ptr(&ucsi_ite_pm),
	},
	.probe = ucsi_ite_probe,
};
module_i2c_driver(ucsi_ite_driver);

MODULE_AUTHOR("Edward Blair <edward.blair@gmail.com>");
MODULE_DESCRIPTION("UCSI I2C transport driver for ITE885x USB-C controllers");
MODULE_LICENSE("GPL");
