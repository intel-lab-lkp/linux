// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2017-18 Linaro Limited

#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/reboot.h>
#include <linux/reboot-mode.h>
#include <linux/regmap.h>

#define PON_SOFT_RB_SPARE		0x8f

#define GEN1_REASON_SHIFT		2
#define GEN2_REASON_SHIFT		1

#define NO_REASON_SHIFT			0

#define PON_SW_RESET_S2_CTL				0x62
#define		PON_SW_RESET_S2_CTL_WARM_RST	0x01
#define PON_SW_RESET_S2_CTL2				0x63
#define		PON_SW_RESET_S2_CTL2_RST_EN	BIT(7)
#define PON_SW_RESET_GO					0x64
#define		PON_SW_RESET_GO_MAGIC		0xa5

struct qcom_pon {
	struct device *dev;
	struct regmap *regmap;
	u32 baseaddr;
	struct reboot_mode_driver reboot_mode;
	long reason_shift;
	bool warm_reset;
};

static int qcom_pon_reboot_mode_write(struct reboot_mode_driver *reboot,
				    unsigned int magic)
{
	struct qcom_pon *pon = container_of
			(reboot, struct qcom_pon, reboot_mode);
	int ret;

	ret = regmap_update_bits(pon->regmap,
				 pon->baseaddr + PON_SOFT_RB_SPARE,
				 GENMASK(7, pon->reason_shift),
				 magic << pon->reason_shift);
	if (ret < 0)
		dev_err(pon->dev, "update reboot mode bits failed\n");

	return ret;
}

static int pm8916_pon_reset(struct sys_off_data *data)
{
	struct qcom_pon *pon = data->cb_data;
	int ret;

	if (!pon->warm_reset || data->mode != REBOOT_WARM)
		return NOTIFY_DONE;

	ret = regmap_write(pon->regmap,
			   pon->baseaddr + PON_SW_RESET_S2_CTL,
			   PON_SW_RESET_S2_CTL_WARM_RST);
	if (ret)
		return NOTIFY_BAD;

	ret = regmap_update_bits(pon->regmap,
				 pon->baseaddr + PON_SW_RESET_S2_CTL2,
				 PON_SW_RESET_S2_CTL2_RST_EN,
				 PON_SW_RESET_S2_CTL2_RST_EN);
	if (ret)
		return NOTIFY_BAD;

	ret = regmap_write(pon->regmap, pon->baseaddr + PON_SW_RESET_GO,
			   PON_SW_RESET_GO_MAGIC);
	if (ret)
		return NOTIFY_BAD;

	mdelay(100);

	return NOTIFY_DONE;
}

static int qcom_pon_probe(struct platform_device *pdev)
{
	struct qcom_pon *pon;
	long reason_shift;
	int error;

	pon = devm_kzalloc(&pdev->dev, sizeof(*pon), GFP_KERNEL);
	if (!pon)
		return -ENOMEM;

	pon->dev = &pdev->dev;

	pon->regmap = dev_get_regmap(pdev->dev.parent, NULL);
	if (!pon->regmap) {
		dev_err(&pdev->dev, "failed to locate regmap\n");
		return -ENODEV;
	}

	error = of_property_read_u32(pdev->dev.of_node, "reg",
				     &pon->baseaddr);
	if (error)
		return error;

	reason_shift = (long)of_device_get_match_data(&pdev->dev);

	if (reason_shift != NO_REASON_SHIFT) {
		pon->reboot_mode.dev = &pdev->dev;
		pon->reason_shift = reason_shift;
		pon->reboot_mode.write = qcom_pon_reboot_mode_write;
		error = devm_reboot_mode_register(&pdev->dev, &pon->reboot_mode);
		if (error) {
			dev_err(&pdev->dev, "can't register reboot mode\n");
			return error;
		}
	}

	pon->warm_reset = of_property_read_bool(pdev->dev.of_node, "qcom,warm-reset");

	platform_set_drvdata(pdev, pon);

	/* Higher priority than psci to handle warm-reset properly */
	error = devm_register_sys_off_handler(&pdev->dev, SYS_OFF_MODE_RESTART, SYS_OFF_PRIO_HIGH,
					      pm8916_pon_reset, pon);
	if (error)
		return dev_err_probe(&pdev->dev, error, "reboot registration fail\n");

	return devm_of_platform_populate(&pdev->dev);
}

static const struct of_device_id qcom_pon_id_table[] = {
	{ .compatible = "qcom,pm8916-pon", .data = (void *)GEN1_REASON_SHIFT },
	{ .compatible = "qcom,pm8941-pon", .data = (void *)NO_REASON_SHIFT },
	{ .compatible = "qcom,pms405-pon", .data = (void *)GEN1_REASON_SHIFT },
	{ .compatible = "qcom,pm8998-pon", .data = (void *)GEN2_REASON_SHIFT },
	{ .compatible = "qcom,pmk8350-pon", .data = (void *)GEN2_REASON_SHIFT },
	{ }
};
MODULE_DEVICE_TABLE(of, qcom_pon_id_table);

static struct platform_driver qcom_pon_driver = {
	.probe = qcom_pon_probe,
	.driver = {
		.name = "qcom-pon",
		.of_match_table = qcom_pon_id_table,
	},
};
module_platform_driver(qcom_pon_driver);

MODULE_DESCRIPTION("Qualcomm Power On driver");
MODULE_LICENSE("GPL v2");
