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

	struct mutex lock;

	unsigned int bus_nr;
	u32 min_speed;
	u32 max_speed;
	u16 flags;
	u16 num_cs;

	struct platform_device *pdev;
	struct spi_controller *ctrl;
};

static struct spi_mockup_host *to_spi_mockup_host(struct config_item *item)
{
	struct config_group *group = to_config_group(item);

	return container_of(group, struct spi_mockup_host, group);
}

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
