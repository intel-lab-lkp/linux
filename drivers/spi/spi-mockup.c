// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * SPI controller Testing Driver
 *
 * Copyright(c) 2022 Huawei Technologies Co., Ltd.
 */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/string.h>
#include <linux/slab.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/spi/spi.h>

#define CREATE_TRACE_POINTS
#include <trace/events/spi_mockup.h>

#define MOCKUP_CHIPSELECT_MAX		8

struct mockup_spi {
	struct mutex lock;
	struct spi_device *devs[MOCKUP_CHIPSELECT_MAX];
};

static struct spi_controller *to_spi_controller(struct device *dev)
{
	return container_of(dev, struct spi_controller, dev);
}

static ssize_t
new_device_store(struct device *dev, struct device_attribute *attr,
		 const char *buf, size_t count)
{
	struct spi_controller *ctrl = to_spi_controller(dev);
	struct spi_board_info info;
	struct mockup_spi *mock;
	struct spi_device *spi;
	char *blank, end;
	int status;

	memset(&info, 0, sizeof(struct spi_board_info));

	blank = strchr(buf, ' ');
	if (!blank) {
		dev_err(dev, "%s: Extra parameters\n", "new_device");
		return -EINVAL;
	}

	if (blank - buf > SPI_NAME_SIZE - 1) {
		dev_err(dev, "%s: Invalid device name\n", "new_device");
		return -EINVAL;
	}

	memcpy(info.modalias, buf, blank - buf);

	status = sscanf(++blank, "%hi%c", &info.chip_select, &end);
	if (status < 1) {
		dev_err(dev, "%s: Can't parse SPI chipselect\n", "new_device");
		return -EINVAL;
	}

	if (status > 1 && end != '\n') {
		dev_err(dev, "%s: Extra parameters\n", "new_device");
		return -EINVAL;
	}

	if (info.chip_select >= ctrl->num_chipselect) {
		dev_err(dev, "%s: Invalid chip_select\n", "new_device");
		return -EINVAL;
	}

	mock = spi_controller_get_devdata(ctrl);
	mutex_lock(&mock->lock);

	if (mock->devs[info.chip_select]) {
		dev_err(dev, "%s: Chipselect %d already in use\n",
			"new_device", info.chip_select);
		mutex_unlock(&mock->lock);
		return -EINVAL;
	}

	spi = spi_new_device(ctrl, &info);
	if (!spi) {
		mutex_unlock(&mock->lock);
		return -ENOMEM;
	}
	mock->devs[info.chip_select] = spi;

	mutex_unlock(&mock->lock);

	dev_info(dev, "%s: Instantiated device %s at 0x%02x\n", "new_device",
		 info.modalias, info.chip_select);

	return count;
}
static DEVICE_ATTR_WO(new_device);

static ssize_t
delete_device_store(struct device *dev, struct device_attribute *attr,
		    const char *buf, size_t count)
{
	struct spi_controller *ctrl = to_spi_controller(dev);
	struct mockup_spi *mock;
	struct spi_device *spi;
	unsigned short chip;
	char end;
	int res;

	/* Parse parameters, reject extra parameters */
	res = sscanf(buf, "%hi%c", &chip, &end);
	if (res < 1) {
		dev_err(dev, "%s: Can't parse SPI address\n", "delete_device");
		return -EINVAL;
	}
	if (res > 1  && end != '\n') {
		dev_err(dev, "%s: Extra parameters\n", "delete_device");
		return -EINVAL;
	}

	if (chip >= ctrl->num_chipselect) {
		dev_err(dev, "%s: Invalid chip_select\n", "delete_device");
		return -EINVAL;
	}

	mock = spi_controller_get_devdata(ctrl);
	mutex_lock(&mock->lock);

	spi = mock->devs[chip];
	if (!spi) {
		mutex_unlock(&mock->lock);
		dev_err(dev, "%s: Invalid chip_select\n", "delete_device");
		return -ENOENT;
	}

	dev_info(dev, "%s: Deleting device %s at 0x%02hx\n", "delete_device",
		 dev_name(&spi->dev), chip);

	spi_unregister_device(spi);
	mock->devs[chip] = NULL;

	mutex_unlock(&mock->lock);

	return count;
}
static DEVICE_ATTR_WO(delete_device);

static struct attribute *spi_mockup_attrs[] = {
	&dev_attr_new_device.attr,
	&dev_attr_delete_device.attr,
	NULL
};
ATTRIBUTE_GROUPS(spi_mockup);

static int spi_mockup_transfer_writeable(struct spi_message *msg)
{
	struct spi_msg_ctx *ctx;
	struct spi_transfer *t;
	int ret = 0;

	ctx = kmalloc(sizeof(*ctx), GFP_ATOMIC);
	if (!ctx)
		return -ENOMEM;

	list_for_each_entry(t, &msg->transfers, transfer_list) {
		if (t->len > SPI_BUFSIZ_MAX)
			return -E2BIG;

		memset(ctx, 0, sizeof(*ctx));
		ctx->cs_off = t->cs_off;
		ctx->cs_change = t->cs_change;
		ctx->tx_nbits = t->tx_nbits;
		ctx->rx_nbits = t->rx_nbits;

		if (t->tx_nbits)
			memcpy(ctx->data, t->tx_buf, t->len);

		trace_spi_transfer_writeable(ctx, msg->spi->chip_select, t->len);

		if (ctx->ret) {
			ret = ctx->ret;
			break;
		}

		if (t->rx_nbits)
			memcpy(t->rx_buf, ctx->data, t->len);
		msg->actual_length += t->len;
	}

	kfree(ctx);

	return ret;
}

static int spi_mockup_transfer(struct spi_controller *ctrl,
			       struct spi_message *msg)
{
	int ret = 0;

	if (trace_spi_transfer_writeable_enabled())
		ret = spi_mockup_transfer_writeable(msg);

	msg->status = ret;
	spi_finalize_current_message(ctrl);

	return ret;
}

static int spi_mockup_probe(struct platform_device *pdev)
{
	int ret;
	struct mockup_spi *mock;
	struct spi_controller *ctrl;

	ctrl = spi_alloc_host(&pdev->dev, sizeof(struct mockup_spi));
	if (!ctrl) {
		pr_err("failed to alloc spi controller\n");
		return -ENOMEM;
	}

	platform_set_drvdata(pdev, ctrl);

	ctrl->dev.of_node = pdev->dev.of_node;
	ctrl->dev.groups = spi_mockup_groups;
	ctrl->num_chipselect = MOCKUP_CHIPSELECT_MAX;
	ctrl->mode_bits = SPI_MODE_USER_MASK;
	ctrl->bus_num = 0;
	ctrl->transfer_one_message = spi_mockup_transfer;

	mock = spi_controller_get_devdata(ctrl);
	mutex_init(&mock->lock);

	ret = devm_spi_register_controller(&pdev->dev, ctrl);
	if (ret) {
		spi_controller_put(ctrl);
		return ret;
	}

	return 0;
}

static const struct of_device_id spi_mockup_match[] = {
	{ .compatible = "spi-mockup", },
	{ }
};
MODULE_DEVICE_TABLE(of, spi_mockup_match);

static struct platform_driver spi_mockup_driver = {
	.probe = spi_mockup_probe,
	.driver = {
		.name = "spi-mockup",
		.of_match_table = spi_mockup_match,
	},
};
module_platform_driver(spi_mockup_driver);

MODULE_AUTHOR("Wei Yongjun <weiyongjun1@huawei.com>");
MODULE_DESCRIPTION("SPI controller Testing Driver");
MODULE_LICENSE("GPL");
