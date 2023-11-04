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
#include <linux/configfs.h>

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
	ctrl->bus_num = pdev->id;
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

struct spi_mockup_device {
	struct config_group group;
	unsigned int bus_nr;
	struct mutex lock;
	struct platform_device *pdev;
};

static struct spi_mockup_device *to_spi_mockup_dev(struct config_item *item)
{
	struct config_group *group = to_config_group(item);

	return container_of(group, struct spi_mockup_device, group);
}

static ssize_t
spi_mockup_enable_store(struct config_item *item, const char *page, size_t len)
{
	int ret = len;
	struct platform_device_info pdevinfo = {0};
	struct spi_mockup_device *dev = to_spi_mockup_dev(item);

	mutex_lock(&dev->lock);
	if (dev->pdev) {
		ret = -EEXIST;
		goto out;
	}

	pdevinfo.name = "spi-mockup";
	pdevinfo.id = dev->bus_nr;
	dev->pdev = platform_device_register_full(&pdevinfo);
	if (IS_ERR(dev->pdev)) {
		ret = PTR_ERR(dev->pdev);
		dev->pdev = NULL;
		goto out;
	}
out:
	mutex_unlock(&dev->lock);
	return ret;
}
CONFIGFS_ATTR_WO(spi_mockup_, enable);

static ssize_t
spi_mockup_disable_store(struct config_item *item, const char *page, size_t len)
{
	int ret = len;
	struct spi_mockup_device *dev = to_spi_mockup_dev(item);

	mutex_lock(&dev->lock);
	if (!dev->pdev) {
		ret = -ENODEV;
		goto out;
	}

	platform_device_unregister(dev->pdev);
	dev->pdev = NULL;
out:
	mutex_unlock(&dev->lock);
	return ret;
}
CONFIGFS_ATTR_WO(spi_mockup_, disable);

static struct configfs_attribute *spi_mockup_configfs_attrs[] = {
	&spi_mockup_attr_enable,
	&spi_mockup_attr_disable,
	NULL,
};

static const struct config_item_type spi_mockup_device_config_group_type = {
	.ct_owner	= THIS_MODULE,
	.ct_attrs	= spi_mockup_configfs_attrs,
};

static struct config_group *
spi_mockup_config_make_device_group(struct config_group *group,
				    const char *name)
{
	int ret, nchar;
	unsigned int nr;
	struct spi_mockup_device *dev;

	ret = sscanf(name, "spi%u%n", &nr, &nchar);
	if (ret != 1 || nchar != strlen(name))
		return ERR_PTR(-EINVAL);

	dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return ERR_PTR(-ENOMEM);

	dev->bus_nr = nr;
	mutex_init(&dev->lock);

	config_group_init_type_name(&dev->group, name,
				    &spi_mockup_device_config_group_type);

	return &dev->group;
}

static void spi_mockup_config_group_release(struct config_item *item)
{
	struct spi_mockup_device *dev = to_spi_mockup_dev(item);

	kfree(dev);
}

static struct configfs_item_operations spi_mockup_config_item_ops = {
	.release = spi_mockup_config_group_release,
};

static struct configfs_group_operations spi_mockup_config_group_ops = {
	.make_group = spi_mockup_config_make_device_group,
};

static const struct config_item_type spi_mockup_config_type = {
	.ct_owner	= THIS_MODULE,
	.ct_group_ops	= &spi_mockup_config_group_ops,
	.ct_item_ops	= &spi_mockup_config_item_ops,
};

static struct configfs_subsystem spi_mockup_config_subsys = {
	.su_group = {
		.cg_item = {
			.ci_namebuf = "spi-mockup",
			.ci_type = &spi_mockup_config_type,
		}
	}
};

static int __init spi_mockup_init(void)
{
	int ret;

	ret = platform_driver_register(&spi_mockup_driver);
	if (ret) {
		pr_err("spi mockup driver registering failed with %d\n", ret);
		return ret;
	}

	config_group_init(&spi_mockup_config_subsys.su_group);
	mutex_init(&spi_mockup_config_subsys.su_mutex);
	ret = configfs_register_subsystem(&spi_mockup_config_subsys);
	if (ret) {
		pr_err("spi mockup configfs registering failed with %d\n", ret);
		mutex_destroy(&spi_mockup_config_subsys.su_mutex);
		platform_driver_unregister(&spi_mockup_driver);
		return ret;
	}

	return ret;
}
module_init(spi_mockup_init);

static void __exit spi_mockup_exit(void)
{
	configfs_unregister_subsystem(&spi_mockup_config_subsys);
	mutex_destroy(&spi_mockup_config_subsys.su_mutex);
	return platform_driver_unregister(&spi_mockup_driver);
}
module_exit(spi_mockup_exit);

MODULE_AUTHOR("Wei Yongjun <weiyongjun1@huawei.com>");
MODULE_DESCRIPTION("SPI controller Testing Driver");
MODULE_LICENSE("GPL");
