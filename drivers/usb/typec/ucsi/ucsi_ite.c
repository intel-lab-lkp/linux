// SPDX-License-Identifier: GPL-2.0-only
/*
 * UCSI I2C transport driver for ITE885x USB-C controllers
 *
 * ITE8853/ITE8800-ITE8805 are UCSI-compliant USB-C controllers found on
 * ASUS Z690/Z790/X670E motherboards. They communicate via I2C with
 * ITE-proprietary register offsets and interrupt registers.
 *
 * Note: Some BIOS implementations declare both MSFT8000 (generic UCSI) and
 * ITE8853 (vendor-specific) ACPI devices at the same I2C address. The i2c
 * core skips the generic device when a vendor-specific sibling exists,
 * allowing this driver to bind to the ITE8853 client with proper IRQ.
 */

#include <linux/acpi.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/module.h>

#include "ucsi.h"

/* ITE-specific I2C register offsets */
#define ITE_REG_CCI		0x84
#define ITE_REG_MESSAGE_IN	0x88
#define ITE_REG_CONTROL		0x98
#define ITE_REG_INT_ACK		0xbc
#define ITE_REG_INT_STATUS	0xbd

/* INT_STATUS register bits */
#define ITE_INT_VENDOR_ALERT	BIT(0)
#define ITE_INT_CCI		BIT(1)

/* INT_ACK register values */
#define ITE_ACK_VENDOR		0x01
#define ITE_ACK_CCI		0x02

struct ucsi_ite {
	struct device *dev;
	struct i2c_client *client;
	struct ucsi *ucsi;
};

static int ucsi_ite_read(struct ucsi_ite *ite, u8 reg, void *val, size_t len)
{
	struct i2c_msg msgs[] = {
		{
			.addr = ite->client->addr,
			.flags = 0,
			.len = 1,
			.buf = &reg,
		},
		{
			.addr = ite->client->addr,
			.flags = I2C_M_RD,
			.len = len,
			.buf = val,
		},
	};
	int ret;

	ret = i2c_transfer(ite->client->adapter, msgs, ARRAY_SIZE(msgs));
	if (ret != ARRAY_SIZE(msgs)) {
		dev_err(ite->dev, "i2c read 0x%02x failed: %d\n", reg, ret);
		return ret < 0 ? ret : -EIO;
	}

	return 0;
}

static int ucsi_ite_write(struct ucsi_ite *ite, u8 reg, const void *val,
			  size_t len)
{
	struct i2c_msg msg = {
		.addr = ite->client->addr,
		.flags = 0,
	};
	u8 *buf;
	int ret;

	buf = kmalloc(len + 1, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	buf[0] = reg;
	memcpy(&buf[1], val, len);
	msg.len = len + 1;
	msg.buf = buf;

	ret = i2c_transfer(ite->client->adapter, &msg, 1);
	kfree(buf);
	if (ret != 1) {
		dev_err(ite->dev, "i2c write 0x%02x failed: %d\n", reg, ret);
		return ret < 0 ? ret : -EIO;
	}

	return 0;
}

static int ucsi_ite_read_version(struct ucsi *ucsi, u16 *version)
{
	/*
	 * The ITE8853 does not expose a UCSI VERSION register over I2C.
	 * The Windows driver never reads it. Report UCSI 1.0.
	 */
	*version = UCSI_VERSION_1_0;
	return 0;
}

static int ucsi_ite_read_cci(struct ucsi *ucsi, u32 *cci)
{
	struct ucsi_ite *ite = ucsi_get_drvdata(ucsi);

	return ucsi_ite_read(ite, ITE_REG_CCI, cci, sizeof(*cci));
}

static int ucsi_ite_read_message_in(struct ucsi *ucsi, void *val, size_t len)
{
	struct ucsi_ite *ite = ucsi_get_drvdata(ucsi);

	return ucsi_ite_read(ite, ITE_REG_MESSAGE_IN, val, len);
}

static int ucsi_ite_async_control(struct ucsi *ucsi, u64 command)
{
	struct ucsi_ite *ite = ucsi_get_drvdata(ucsi);

	return ucsi_ite_write(ite, ITE_REG_CONTROL, &command, sizeof(command));
}

static int ucsi_ite_sync_control(struct ucsi *ucsi, u64 command, u32 *cci,
				 void *data, size_t size)
{
	/*
	 * The ITE8853 handles PPM_RESET internally and does not accept it
	 * over I2C — the Windows driver never sends it. Fake a successful
	 * reset so the UCSI core can proceed with initialization.
	 */
	if ((command & 0xff) == UCSI_PPM_RESET) {
		if (cci)
			*cci = UCSI_CCI_RESET_COMPLETE;
		return 0;
	}

	return ucsi_sync_control_common(ucsi, command, cci, data, size);
}

static const struct ucsi_operations ucsi_ite_ops = {
	.read_version = ucsi_ite_read_version,
	.read_cci = ucsi_ite_read_cci,
	.poll_cci = ucsi_ite_read_cci,
	.read_message_in = ucsi_ite_read_message_in,
	.sync_control = ucsi_ite_sync_control,
	.async_control = ucsi_ite_async_control,
};

static irqreturn_t ucsi_ite_irq(int irq, void *data)
{
	struct ucsi_ite *ite = data;
	u8 status;
	u32 cci;
	u8 ack;
	int ret;

	ret = ucsi_ite_read(ite, ITE_REG_INT_STATUS, &status, sizeof(status));
	if (ret)
		return IRQ_NONE;

	if (!(status & (ITE_INT_CCI | ITE_INT_VENDOR_ALERT)))
		return IRQ_NONE;

	if (status & ITE_INT_CCI) {
		ack = ITE_ACK_CCI;
		ucsi_ite_write(ite, ITE_REG_INT_ACK, &ack, sizeof(ack));

		ret = ucsi_ite_read(ite, ITE_REG_CCI, &cci, sizeof(cci));
		if (!ret)
			ucsi_notify_common(ite->ucsi, cci);
	}

	if (status & ITE_INT_VENDOR_ALERT) {
		ack = ITE_ACK_VENDOR;
		ucsi_ite_write(ite, ITE_REG_INT_ACK, &ack, sizeof(ack));
	}

	return IRQ_HANDLED;
}

static int ucsi_ite_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct ucsi_ite *ite;
	int ret;

	ite = devm_kzalloc(dev, sizeof(*ite), GFP_KERNEL);
	if (!ite)
		return -ENOMEM;

	ite->dev = dev;
	ite->client = client;
	i2c_set_clientdata(client, ite);

	ite->ucsi = ucsi_create(dev, &ucsi_ite_ops);
	if (IS_ERR(ite->ucsi))
		return PTR_ERR(ite->ucsi);

	ucsi_set_drvdata(ite->ucsi, ite);

	ret = request_threaded_irq(client->irq, NULL, ucsi_ite_irq,
				   IRQF_ONESHOT, dev_name(dev), ite);
	if (ret) {
		dev_err(dev, "failed to request IRQ: %d\n", ret);
		goto err_destroy;
	}

	ret = ucsi_register(ite->ucsi);
	if (ret) {
		dev_err(dev, "failed to register UCSI: %d\n", ret);
		goto err_free_irq;
	}

	return 0;

err_free_irq:
	free_irq(client->irq, ite);
err_destroy:
	ucsi_destroy(ite->ucsi);
	return ret;
}

static void ucsi_ite_remove(struct i2c_client *client)
{
	struct ucsi_ite *ite = i2c_get_clientdata(client);

	ucsi_unregister(ite->ucsi);
	free_irq(client->irq, ite);
	ucsi_destroy(ite->ucsi);
}

static int ucsi_ite_suspend(struct device *dev)
{
	struct ucsi_ite *ite = dev_get_drvdata(dev);

	disable_irq(ite->client->irq);

	return 0;
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
	.remove = ucsi_ite_remove,
};
module_i2c_driver(ucsi_ite_driver);

MODULE_AUTHOR("Edward Blair <edward.blair@gmail.com>");
MODULE_DESCRIPTION("UCSI I2C transport driver for ITE885x USB-C controllers");
MODULE_LICENSE("GPL");
