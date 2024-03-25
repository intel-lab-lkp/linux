// SPDX-License-Identifier: GPL-2.0
/*
 * UCSI driver for ChromeOS EC
 *
 * Copyright 2024 Google LLC.
 */

#include <linux/container_of.h>
#include <linux/dev_printk.h>
#include <linux/module.h>
#include <linux/platform_data/cros_ec_commands.h>
#include <linux/platform_data/cros_usbpd_notify.h>
#include <linux/platform_data/cros_ec_proto.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/usb/ucsi.h>
#include <linux/wait.h>

#define DRV_NAME "cros-ec-ucsi"

#define MAX_EC_DATA_SIZE 256
#define WRITE_TMO_MS 500

#define COMMAND_PENDING 1
#define ACK_PENDING     2

#define UCSI_COMMAND(_cmd_) ((_cmd_) & 0xff)
#define UCSI_ACK_CC_CI 0x04

struct cros_ucsi_data {
	struct device *dev;
	struct ucsi *ucsi;

	struct cros_ec_device *ec;
	struct notifier_block nb;
	struct work_struct work;

	struct completion complete;
	unsigned long flags;
};

static int cros_ucsi_read(struct ucsi *ucsi, unsigned int offset, void *val,
			  size_t val_len)
{
	struct cros_ucsi_data *udata = ucsi_get_drvdata(ucsi);
	struct ec_params_ucsi_ppm_get req = {
		.offset = offset,
		.size = val_len,
	};
	int ret;

	if (val_len > MAX_EC_DATA_SIZE) {
		dev_err(udata->dev, "Can't read %zu bytes. Too big.", val_len);
		return -EINVAL;
	}

	ret = cros_ec_cmd(udata->ec, 0, EC_CMD_UCSI_PPM_GET,
			  &req, sizeof(req), val, val_len);
	if (ret < 0) {
		dev_warn(udata->dev, "Failed to send EC message UCSI_PPM_GET: error=%d", ret);
		return ret;
	}
	return 0;
}

static int cros_ucsi_async_write(struct ucsi *ucsi, unsigned int offset,
				 const void *val, size_t val_len)
{
	struct cros_ucsi_data *udata = ucsi_get_drvdata(ucsi);
	uint8_t ec_buffer[MAX_EC_DATA_SIZE + sizeof(struct ec_params_ucsi_ppm_set)];
	struct ec_params_ucsi_ppm_set *req = (struct ec_params_ucsi_ppm_set *)ec_buffer;
	int ret = 0;

	if (val_len > MAX_EC_DATA_SIZE) {
		dev_err(udata->dev, "Can't write %zu bytes. Too big.", val_len);
		return -EINVAL;
	}

	memset(req, 0, sizeof(ec_buffer));
	req->offset = offset;
	memcpy(req->data, val, val_len);
	ret = cros_ec_cmd(udata->ec, 0, EC_CMD_UCSI_PPM_SET,
			  req, sizeof(struct ec_params_ucsi_ppm_set) + val_len, NULL, 0);

	if (ret < 0) {
		dev_warn(udata->dev, "Failed to send EC message UCSI_PPM_SET: error=%d", ret);
		return ret;
	}
	return 0;
}

static int cros_ucsi_sync_write(struct ucsi *ucsi, unsigned int offset,
				const void *val, size_t val_len)
{
	struct cros_ucsi_data *udata = ucsi_get_drvdata(ucsi);
	bool ack = UCSI_COMMAND(*(u64 *)val) == UCSI_ACK_CC_CI;
	int ret;

	if (ack)
		set_bit(ACK_PENDING, &udata->flags);
	else
		set_bit(COMMAND_PENDING, &udata->flags);

	ret = cros_ucsi_async_write(ucsi, offset, val, val_len);
	if (ret)
		goto out;

	if (!wait_for_completion_timeout(&udata->complete, WRITE_TMO_MS))
		ret = -ETIMEDOUT;

out:
	if (ack)
		clear_bit(ACK_PENDING, &udata->flags);
	else
		clear_bit(COMMAND_PENDING, &udata->flags);
	return ret;
}

struct ucsi_operations cros_ucsi_ops = {
	.read = cros_ucsi_read,
	.async_write = cros_ucsi_async_write,
	.sync_write = cros_ucsi_sync_write,
};

static void cros_ucsi_work(struct work_struct *work)
{
	struct cros_ucsi_data *udata = container_of(work, struct cros_ucsi_data, work);
	u32 cci;
	int ret;

	ret = cros_ucsi_read(udata->ucsi, UCSI_CCI, &cci, sizeof(cci));
	if (ret)
		return;

	if (UCSI_CCI_CONNECTOR(cci))
		ucsi_connector_change(udata->ucsi, UCSI_CCI_CONNECTOR(cci));

	if (cci & UCSI_CCI_ACK_COMPLETE && test_bit(ACK_PENDING, &udata->flags))
		complete(&udata->complete);
	if (cci & UCSI_CCI_COMMAND_COMPLETE &&
	    test_bit(COMMAND_PENDING, &udata->flags))
		complete(&udata->complete);
}

static int cros_ucsi_event(struct notifier_block *nb,
			   unsigned long host_event, void *_notify)
{
	struct cros_ucsi_data *udata = container_of(nb, struct cros_ucsi_data, nb);

	if (!(host_event & PD_EVENT_PPM))
		return NOTIFY_OK;

	dev_dbg(udata->dev, "UCSI notification received");
	flush_work(&udata->work);
	schedule_work(&udata->work);

	return NOTIFY_OK;
}

static int cros_ucsi_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct cros_ucsi_data *udata;
	int ret;

	udata = devm_kzalloc(dev, sizeof(*udata), GFP_KERNEL);
	if (!udata)
		return -ENOMEM;

	udata->dev = dev;

	udata->ec = ((struct cros_ec_dev *) dev_get_drvdata(pdev->dev.parent))->ec_dev;
	if (!udata->ec) {
		dev_err(dev, "couldn't find parent EC device\n");
		return -ENODEV;
	}

	platform_set_drvdata(pdev, udata);

	INIT_WORK(&udata->work, cros_ucsi_work);
	init_completion(&udata->complete);

	udata->ucsi = ucsi_create(udata->dev, &cros_ucsi_ops);
	if (IS_ERR(udata->ucsi)) {
		dev_err(dev, "failed to allocate UCSI instance\n");
		return PTR_ERR(udata->ucsi);
	}

	ucsi_set_drvdata(udata->ucsi, udata);

	ret = ucsi_register(udata->ucsi);
	if (ret) {
		ucsi_destroy(udata->ucsi);
		udata->ucsi = NULL;
		return ret;
	}

	udata->nb.notifier_call = cros_ucsi_event;
	ret = cros_usbpd_register_notify(&udata->nb);
	return ret;
}

static int cros_ucsi_remove(struct platform_device *dev)
{
	struct cros_ucsi_data *udata = platform_get_drvdata(dev);

	if (udata->ucsi) {
		ucsi_unregister(udata->ucsi);
		ucsi_destroy(udata->ucsi);
		udata->ucsi = NULL;
	}
	return 0;
}

static int cros_ucsi_suspend(struct device *dev)
{
	struct cros_ucsi_data *udata = dev_get_drvdata(dev);

	cancel_work_sync(&udata->work);

	return 0;
}

static int cros_ucsi_resume(struct device *dev)
{
	struct cros_ucsi_data *udata = dev_get_drvdata(dev);

	return ucsi_resume(udata->ucsi);
}

static SIMPLE_DEV_PM_OPS(cros_ucsi_pm_ops, cros_ucsi_suspend,
			 cros_ucsi_resume);

static struct platform_driver cros_ucsi_driver = {
	.driver = {
		.name = DRV_NAME,
		.pm = &cros_ucsi_pm_ops,
	},
	.probe = cros_ucsi_probe,
	.remove = cros_ucsi_remove,
};

module_platform_driver(cros_ucsi_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("UCSI driver for ChromeOS EC.");
MODULE_ALIAS("platform:" DRV_NAME);
