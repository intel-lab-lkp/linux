// SPDX-License-Identifier: GPL-2.0

#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>

#include "svc-i3c.h"

static bool svc_i3c_is_master(struct device *dev)
{
	const char *mode = NULL;

	device_property_read_string(dev, "mode", &mode);

	if (!mode)
		return true;

	if (strncmp(mode, "target", 6) == 0)
		return false;

	return true;
}

static int svc_i3c_probe(struct platform_device *pdev)
{
	if (svc_i3c_is_master(&pdev->dev))
		return svc_i3c_master_probe(pdev);

	return svc_i3c_target_probe(pdev);
}

static void svc_i3c_remove(struct platform_device *pdev)
{
	if (svc_i3c_is_master(&pdev->dev))
		return svc_i3c_master_remove(pdev);

	svc_i3c_target_remove(pdev);
}

static int __maybe_unused svc_i3c_runtime_suspend(struct device *dev)
{
	if (svc_i3c_is_master(dev))
		return svc_i3c_master_runtime_suspend(dev);

	return -EINVAL;
}

static int __maybe_unused svc_i3c_runtime_resume(struct device *dev)
{
	if (svc_i3c_is_master(dev))
		return svc_i3c_master_runtime_resume(dev);

	return -EINVAL;
}

static const struct dev_pm_ops svc_i3c_pm_ops = {
	SET_NOIRQ_SYSTEM_SLEEP_PM_OPS(pm_runtime_force_suspend,
				      pm_runtime_force_resume)
	SET_RUNTIME_PM_OPS(svc_i3c_runtime_suspend,
			   svc_i3c_runtime_resume, NULL)
};

static const struct of_device_id svc_i3c_master_of_match_tbl[] = {
	{ .compatible = "silvaco,i3c-master-v1"},
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(of, svc_i3c_master_of_match_tbl);

static struct platform_driver svc_i3c_master = {
	.probe = svc_i3c_probe,
	.remove_new = svc_i3c_remove,
	.driver = {
		.name = "silvaco-i3c-master",
		.of_match_table = svc_i3c_master_of_match_tbl,
		.pm = &svc_i3c_pm_ops,
	},
};
module_platform_driver(svc_i3c_master);
