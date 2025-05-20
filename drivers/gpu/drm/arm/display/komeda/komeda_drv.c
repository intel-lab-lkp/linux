// SPDX-License-Identifier: GPL-2.0
/*
 * (C) COPYRIGHT 2018 ARM Limited. All rights reserved.
 * Author: James.Qian.Wang <james.qian.wang@arm.com>
 *
 */
#include <linux/debugfs.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <drm/clients/drm_client_setup.h>
#include <drm/drm_module.h>
#include <drm/drm_of.h>
#include "komeda_dev.h"
#include "komeda_kms.h"

struct komeda_drv {
	struct komeda_dev *mdev;
	struct komeda_kms_dev *kms;
};

static ssize_t
aclk_hz_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct komeda_dev *mdev = dev_to_mdev(dev);

	return sysfs_emit(buf, "%lu\n", clk_get_rate(mdev->aclk));
}
static DEVICE_ATTR_RO(aclk_hz);

static ssize_t
config_id_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct komeda_dev *mdev = dev_to_mdev(dev);
	struct komeda_pipeline *pipe = mdev->pipelines[0];
	union komeda_config_id config_id;
	int i;

	memset(&config_id, 0, sizeof(config_id));

	config_id.max_line_sz = pipe->layers[0]->hsize_in.end;
	config_id.n_pipelines = mdev->n_pipelines;
	config_id.n_scalers = pipe->n_scalers;
	config_id.n_layers = pipe->n_layers;
	config_id.n_richs = 0;
	for (i = 0; i < pipe->n_layers; i++) {
		if (pipe->layers[i]->layer_type == KOMEDA_FMT_RICH_LAYER)
			config_id.n_richs++;
	}
	return sysfs_emit(buf, "0x%08x\n", config_id.value);
}
static DEVICE_ATTR_RO(config_id);

static ssize_t
core_id_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct komeda_dev *mdev = dev_to_mdev(dev);

	return sysfs_emit(buf, "0x%08x\n", mdev->chip.core_id);
}
static DEVICE_ATTR_RO(core_id);

static struct attribute *komeda_sysfs_attrs[] = {
	&dev_attr_aclk_hz.attr,
	&dev_attr_config_id.attr,
	&dev_attr_core_id.attr,
	NULL,
};
ATTRIBUTE_GROUPS(komeda_sysfs);

struct komeda_dev *dev_to_mdev(struct device *dev)
{
	struct komeda_drv *mdrv = dev_get_drvdata(dev);

	return mdrv ? mdrv->mdev : NULL;
}

static void komeda_platform_remove(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct komeda_drv *mdrv = dev_get_drvdata(dev);

	komeda_kms_detach(mdrv->kms);

	if (pm_runtime_enabled(dev))
		pm_runtime_disable(dev);
	else
		komeda_dev_suspend(mdrv->mdev);

	komeda_dev_destroy(mdrv->mdev);

	dev_set_drvdata(dev, NULL);
	devm_kfree(dev, mdrv);
}

static void komeda_platform_shutdown(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct komeda_drv *mdrv = dev_get_drvdata(dev);

	komeda_kms_shutdown(mdrv->kms);
}

static int komeda_platform_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct komeda_drv *mdrv;
	int err;

	err = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(40));
	if (err)
		return dev_err_probe(dev, err, "DMA mask error\n");

	mdrv = devm_kzalloc(dev, sizeof(*mdrv), GFP_KERNEL);
	if (!mdrv)
		return -ENOMEM;

	mdrv->mdev = komeda_dev_create(dev);
	if (IS_ERR(mdrv->mdev)) {
		err = PTR_ERR(mdrv->mdev);
		goto free_mdrv;
	}

	pm_runtime_enable(dev);
	if (!pm_runtime_enabled(dev))
		komeda_dev_resume(mdrv->mdev);

	mdrv->kms = komeda_kms_attach(mdrv->mdev);
	if (IS_ERR(mdrv->kms)) {
		err = PTR_ERR(mdrv->kms);
		goto destroy_mdev;
	}

	dev_set_drvdata(dev, mdrv);
	drm_client_setup(&mdrv->kms->base, NULL);

	return 0;

destroy_mdev:
	if (pm_runtime_enabled(dev))
		pm_runtime_disable(dev);
	else
		komeda_dev_suspend(mdrv->mdev);

	komeda_dev_destroy(mdrv->mdev);

free_mdrv:
	devm_kfree(dev, mdrv);
	return err;
}

static const struct of_device_id komeda_of_match[] = {
	{ .compatible = "arm,mali-d71", .data = d71_identify, },
	{ .compatible = "arm,mali-d32", .data = d71_identify, },
	{},
};

MODULE_DEVICE_TABLE(of, komeda_of_match);

static int __maybe_unused komeda_rt_pm_suspend(struct device *dev)
{
	struct komeda_drv *mdrv = dev_get_drvdata(dev);

	return komeda_dev_suspend(mdrv->mdev);
}

static int __maybe_unused komeda_rt_pm_resume(struct device *dev)
{
	struct komeda_drv *mdrv = dev_get_drvdata(dev);

	return komeda_dev_resume(mdrv->mdev);
}

static int __maybe_unused komeda_pm_suspend(struct device *dev)
{
	struct komeda_drv *mdrv = dev_get_drvdata(dev);
	int res;

	res = drm_mode_config_helper_suspend(&mdrv->kms->base);

	if (!pm_runtime_status_suspended(dev))
		komeda_dev_suspend(mdrv->mdev);

	return res;
}

static int __maybe_unused komeda_pm_resume(struct device *dev)
{
	struct komeda_drv *mdrv = dev_get_drvdata(dev);

	if (!pm_runtime_status_suspended(dev))
		komeda_dev_resume(mdrv->mdev);

	return drm_mode_config_helper_resume(&mdrv->kms->base);
}

static const struct dev_pm_ops komeda_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(komeda_pm_suspend, komeda_pm_resume)
	SET_RUNTIME_PM_OPS(komeda_rt_pm_suspend, komeda_rt_pm_resume, NULL)
};

static struct platform_driver komeda_platform_driver = {
	.probe	= komeda_platform_probe,
	.remove = komeda_platform_remove,
	.shutdown = komeda_platform_shutdown,
	.driver	= {
		.name = "komeda",
		.of_match_table	= komeda_of_match,
		.dev_groups	= komeda_sysfs_groups,
		.pm = &komeda_pm_ops,
	},
};

drm_module_platform_driver(komeda_platform_driver);

MODULE_AUTHOR("James.Qian.Wang <james.qian.wang@arm.com>");
MODULE_DESCRIPTION("Komeda KMS driver");
MODULE_LICENSE("GPL v2");
