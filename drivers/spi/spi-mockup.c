/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * SPI controller Testing Driver
 *
 * Copyright(c) 2022 Huawei Technologies Co., Ltd.
 */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/string.h>
#include <linux/slab.h>
#include <linux/platform_device.h>
#include <linux/spi/spi.h>
#include <linux/configfs.h>

#define MOCKUP_CHIPSELECT_MAX	U16_MAX

struct spi_mockup_host {
	struct config_group group;
	struct config_group targets_group;

	struct mutex lock;

	unsigned int bus_nr;
	u32 min_speed;
	u32 max_speed;
	u16 flags;
	u16 num_cs;

	struct platform_device *pdev;
	struct spi_controller *ctrl;
	unsigned long bitmap[BITS_TO_LONGS(MOCKUP_CHIPSELECT_MAX)];
};

static struct spi_mockup_host *to_spi_mockup_host(struct config_item *item)
{
	struct config_group *group = to_config_group(item);

	return container_of(group, struct spi_mockup_host, group);
}

static struct spi_mockup_host *
to_spi_mockup_host_from_targets(struct config_group *targets_group)
{
	return container_of(targets_group,
			    struct spi_mockup_host, targets_group);
}

struct spi_mockup_target {
	struct config_group group;
	unsigned short chip;
	char device_id[SPI_NAME_SIZE];
	struct spi_device *spi;
	struct spi_mockup_host *host;
};

static struct spi_mockup_target *to_spi_mockup_target(struct config_item *item)
{
	struct config_group *group = to_config_group(item);

	return container_of(group, struct spi_mockup_target, group);
}

static ssize_t
spi_mockup_target_device_id_show(struct config_item *item, char *buf)
{
	struct spi_mockup_target *target = to_spi_mockup_target(item);

	return sprintf(buf, "%s\n", target->device_id);
}

static ssize_t
spi_mockup_target_device_id_store(struct config_item *item,
				  const char *buf, size_t len)
{
	struct spi_mockup_target *target = to_spi_mockup_target(item);

	if (len > SPI_NAME_SIZE)
		return -EINVAL;

	memcpy(target->device_id, buf, len);

	return len;
}
CONFIGFS_ATTR(spi_mockup_target_, device_id);

static int __target_online(struct spi_mockup_target *target)
{
	struct spi_board_info info = {0};
	struct device *dev;

	if (target->spi)
		return -EBUSY;

	if (!target->host->pdev)
		return -ENODEV;

	target->chip = find_first_zero_bit(target->host->bitmap,
					   MOCKUP_CHIPSELECT_MAX);
	if (target->chip < 0)
		return target->chip;

	if (target->chip > target->host->num_cs)
		return -EBUSY;

	info.chip_select = target->chip;
	strncpy(info.modalias, target->device_id,
		strlen(target->device_id));

	target->spi = spi_new_device(target->host->ctrl, &info);
	if (!target->spi)
		return -ENOMEM;

	bitmap_set(target->host->bitmap, target->chip, 1);

	dev = &target->host->ctrl->dev;
	dev_info(dev, "Instantiated device %s at 0x%02x\n",
		 info.modalias, info.chip_select);

	return 0;
}

static int __target_offline(struct spi_mockup_target *target)
{
	struct device *dev;

	if (!target->spi)
		return -ENODEV;

	dev = &target->host->ctrl->dev;
	dev_info(dev, "Deleting device %s at 0x%02hx\n",
		 dev_name(&target->spi->dev), target->chip);

	spi_unregister_device(target->spi);
	target->spi = NULL;

	bitmap_clear(target->host->bitmap, target->chip, 1);


	return 0;
}

static ssize_t
spi_mockup_target_live_store(struct config_item *item,
			     const char *buf, size_t len)
{
	struct spi_mockup_target *target = to_spi_mockup_target(item);
	int ret;
	bool res;

	ret = kstrtobool(buf, &res);
	if (ret)
		return -EINVAL;

	if (!strlen(target->device_id))
		return -EINVAL;

	mutex_lock(&target->host->lock);
	if (res)
		ret = __target_online(target);
	else
		ret = __target_offline(target);
	mutex_unlock(&target->host->lock);

	return ret ? ret : len;
}

static ssize_t
spi_mockup_target_live_show(struct config_item *item, char *buf)
{
	struct spi_mockup_target *target = to_spi_mockup_target(item);

	return sprintf(buf, "%s\n", (target->spi) ? "true" : "false");
}
CONFIGFS_ATTR(spi_mockup_target_, live);


static struct configfs_attribute *spi_mockup_target_attrs[] = {
	&spi_mockup_target_attr_device_id,
	&spi_mockup_target_attr_live,
	NULL,
};

static void spi_mockup_target_release(struct config_item *item)
{
	struct spi_mockup_target *target = to_spi_mockup_target(item);

	__target_offline(target);
	kfree(target);
}

static struct configfs_item_operations spi_mockup_target_item_ops = {
	.release = spi_mockup_target_release,
};

static const struct config_item_type spi_mockup_target_item_type = {
	.ct_owner	= THIS_MODULE,
	.ct_attrs	= spi_mockup_target_attrs,
	.ct_item_ops    = &spi_mockup_target_item_ops,
};

static struct config_group *
spi_mockup_target_make_group(struct config_group *group, const char *name)
{
	struct spi_mockup_target *target;
	struct spi_mockup_host *host = to_spi_mockup_host_from_targets(group);

	target = kzalloc(sizeof(*target), GFP_KERNEL);
	if (!target)
		return ERR_PTR(-ENOMEM);

	target->host = host;

	config_group_init_type_name(&target->group, name,
				    &spi_mockup_target_item_type);

	return &target->group;
}

static struct configfs_group_operations spi_mockup_targets_group_ops = {
	.make_group = spi_mockup_target_make_group,
};

static void spi_mockup_targets_group_release(struct config_item *item)
{
	struct spi_mockup_target *target = to_spi_mockup_target(item);

	kfree(target);
}

static struct configfs_item_operations spi_mockup_targets_group_item_ops = {
	.release = spi_mockup_targets_group_release,
};

static const struct config_item_type spi_mockup_target_type = {
	.ct_owner     = THIS_MODULE,
	.ct_group_ops = &spi_mockup_targets_group_ops,
	.ct_item_ops  = &spi_mockup_targets_group_item_ops,
};

static ssize_t __host_online(struct spi_mockup_host *host)
{
	int ret;
	struct platform_device_info pdevinfo = {0};

	if (host->pdev)
		return -EEXIST;

	pdevinfo.name = "spi-mockup";
	pdevinfo.id = host->bus_nr;

	/* Use the pointer of host as the data, then probe
	 * can init the host->ctrl */
	pdevinfo.data = &host;
	pdevinfo.size_data = sizeof(&host);

	host->pdev = platform_device_register_full(&pdevinfo);
	if (IS_ERR(host->pdev)) {
		ret = PTR_ERR(host->pdev);
		host->pdev = NULL;
		return ret;
	}

	return 0;
}

static ssize_t __host_offline(struct spi_mockup_host *host)
{
	if (!host->pdev)
		return -ENODEV;

	if (!bitmap_empty(host->bitmap, host->num_cs))
		return -EBUSY;

	platform_device_unregister(host->pdev);
	host->pdev = NULL;
	host->ctrl = NULL;

	return 0;
}

static ssize_t
spi_mockup_live_store(struct config_item *item, const char *buf, size_t len)
{
	struct spi_mockup_host *host = to_spi_mockup_host(item);
	int ret;
	bool res;

	ret = kstrtobool(buf, &res);
	if (ret)
		return ret;

	mutex_lock(&host->lock);
	if (res)
		ret = __host_online(host);
	else
		ret = __host_offline(host);
	mutex_unlock(&host->lock);

	return ret ? ret : len;
}

static ssize_t
spi_mockup_live_show(struct config_item *item, char *buf)
{
	struct spi_mockup_host *host = to_spi_mockup_host(item);

	return sprintf(buf, "%s", (host->pdev) ? "true" : "false");
}
CONFIGFS_ATTR(spi_mockup_, live);


#define SPI_MOCKUP_ATTR(type, name)					  \
static ssize_t spi_mockup_ ## name ## _store(struct config_item *item,	  \
					     const char *buf, size_t len) \
{									  \
	int ret;							  \
	type val;							  \
	struct spi_mockup_host *host = to_spi_mockup_host(item);	  \
									  \
	mutex_lock(&host->lock);					  \
	if (host->pdev) {						  \
		ret = -EBUSY;						  \
		goto out;						  \
	}								  \
									  \
	ret = kstrto ## type(buf, 0, &val);				  \
	if (ret)							  \
		goto out;						  \
									  \
	host->name = val;						  \
out:									  \
	mutex_unlock(&host->lock);					  \
	return ret ? ret : len;						  \
}									  \
static ssize_t spi_mockup_ ## name ## _show(struct config_item *item,	  \
					    char *buf)			  \
{									  \
	struct spi_mockup_host *host = to_spi_mockup_host(item);	  \
	return sprintf(buf, "%u", host->name);                            \
}                                                                         \
CONFIGFS_ATTR(spi_mockup_, name)					  \

SPI_MOCKUP_ATTR(u32, min_speed);
SPI_MOCKUP_ATTR(u32, max_speed);
SPI_MOCKUP_ATTR(u16, flags);
SPI_MOCKUP_ATTR(u16, num_cs);

static struct configfs_attribute *spi_mockup_host_attrs[] = {
	&spi_mockup_attr_live,
	&spi_mockup_attr_min_speed,
	&spi_mockup_attr_max_speed,
	&spi_mockup_attr_flags,
	&spi_mockup_attr_num_cs,
	NULL,
};

static void spi_mockup_host_release(struct config_item *item)
{
	struct spi_mockup_host *host = to_spi_mockup_host(item);

	__host_offline(host);
	kfree(host);
}

static struct configfs_item_operations spi_mockup_host_item_ops = {
	.release = spi_mockup_host_release,
};

static const struct config_item_type spi_mockup_host_config_group_type = {
	.ct_owner	= THIS_MODULE,
	.ct_attrs	= spi_mockup_host_attrs,
	.ct_item_ops	= &spi_mockup_host_item_ops,
};

static struct config_group *
spi_mockup_host_make_group(struct config_group *group, const char *name)
{
	int ret, nchar;
	unsigned int nr;
	struct spi_mockup_host *host;

	ret = sscanf(name, "spi%u%n", &nr, &nchar);
	if (ret != 1 || nchar != strlen(name))
		return ERR_PTR(-EINVAL);

	host = kzalloc(sizeof(*host), GFP_KERNEL);
	if (!host)
		return ERR_PTR(-ENOMEM);

	host->bus_nr = nr;
	host->num_cs = MOCKUP_CHIPSELECT_MAX;
	mutex_init(&host->lock);

	config_group_init_type_name(&host->group, name,
				    &spi_mockup_host_config_group_type);

	config_group_init_type_name(&host->targets_group, "targets",
				    &spi_mockup_target_type);
	configfs_add_default_group(&host->targets_group, &host->group);

	return &host->group;
}

static struct configfs_group_operations spi_mockup_host_group_ops = {
	.make_group = spi_mockup_host_make_group,
};

static const struct config_item_type spi_mockup_host_type = {
	.ct_owner	= THIS_MODULE,
	.ct_group_ops	= &spi_mockup_host_group_ops,
};

static struct configfs_subsystem spi_mockup_config_subsys = {
	.su_group = {
		.cg_item = {
			.ci_namebuf = "spi-mockup",
			.ci_type = &spi_mockup_host_type,
		}
	}
};

static int
spi_mockup_transfer(struct spi_controller *ctrl, struct spi_message *msg)
{
	msg->status = 0;
	spi_finalize_current_message(ctrl);

	return 0;
}

static int
spi_mockup_probe(struct platform_device *pdev)
{
	int ret;
	struct spi_controller *ctrl;
	struct spi_mockup_host **host;

	host = dev_get_platdata(&pdev->dev);
	if (!host || !(*host))
		return -EINVAL;

	ctrl = spi_alloc_host(&pdev->dev, 0);
	if (!ctrl) {
		pr_err("failed to alloc spi controller\n");
		return -ENOMEM;
	}

	platform_set_drvdata(pdev, ctrl);

	ctrl->bus_num = pdev->id;
	ctrl->mode_bits = SPI_MODE_USER_MASK;
	ctrl->dev.of_node = pdev->dev.of_node;
	ctrl->transfer_one_message = spi_mockup_transfer;
	ctrl->min_speed_hz = (*host)->min_speed;
	ctrl->max_speed_hz = (*host)->max_speed;
	ctrl->num_chipselect = (*host)->num_cs;
	ctrl->flags = (*host)->flags;

	ret = devm_spi_register_controller(&pdev->dev, ctrl);
	if (ret) {
		spi_controller_put(ctrl);
		return ret;
	}

	(*host)->ctrl = ctrl;

	return 0;
}

static struct platform_driver spi_mockup_driver = {
	.probe = spi_mockup_probe,
	.driver = {
		.name = "spi-mockup",
	},
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
