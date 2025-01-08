// SPDX-License-Identifier: GPL-2.0
/*
 * SoC bus driver for Qualcomm SC7180 SoCs
 */

#include <linux/device.h>
#include <linux/init.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>

static int qcom_soc_sc7180_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;

	return of_platform_populate(np, NULL, NULL, dev);
}

static const struct of_device_id qcom_soc_sc7180_match[] = {
	{ .compatible = "qcom,soc-sc7180", },
	{ }
};
MODULE_DEVICE_TABLE(of, qcom_soc_sc7180_match);

static struct platform_driver qcom_soc_sc7180_driver = {
	.probe = qcom_soc_sc7180_probe,
	.driver = {
		.name = "qcom-soc-sc7180",
		.of_match_table = qcom_soc_sc7180_match,
		.suppress_bind_attrs = true,
	},
};

static int __init qcom_soc_sc7180_driver_init(void)
{
	return platform_driver_register(&qcom_soc_sc7180_driver);
}
/* Register before simple-bus driver. */
arch_initcall(qcom_soc_sc7180_driver_init);

static void __exit qcom_soc_sc7180_driver_exit(void)
{
	platform_driver_unregister(&qcom_soc_sc7180_driver);
}
module_exit(qcom_soc_sc7180_driver_exit);

MODULE_DESCRIPTION("Qualcomm SC7180 SoC Driver");
MODULE_LICENSE("GPL");
