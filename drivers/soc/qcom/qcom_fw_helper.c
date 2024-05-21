// SPDX-License-Identifier: GPL-2.0-only
/*
 * Qualcomm Firmware loading data
 *
 * Copyright (C) 2024 Linaro Ltd
 */

#include <linux/cleanup.h>
#include <linux/device.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/soc/qcom/fw_helper.h>

static DEFINE_MUTEX(qcom_fw_mutex);
static const char *fw_path;

static const struct of_device_id qcom_fw_paths[] = {
	/* device-specific entries */
	{ .compatible = "thundercomm,db845c", .data = "qcom/sdm845/Thundercomm/db845c", },
	{ .compatible = "qcom,qrb5165-rb5", .data = "qcom/sm8250/Thundercomm/RB5", },
	/* SoC default entries */
	{ .compatible = "qcom,apq8016", .data = "qcom/apq8016", },
	{ .compatible = "qcom,apq8096", .data = "qcom/apq8096", },
	{ .compatible = "qcom,sdm845", .data = "qcom/sdm845", },
	{ .compatible = "qcom,sm8250", .data = "qcom/sm8250", },
	{ .compatible = "qcom,sm8350", .data = "qcom/sm8350", },
	{ .compatible = "qcom,sm8450", .data = "qcom/sm8450", },
	{ .compatible = "qcom,sm8550", .data = "qcom/sm8550", },
	{ .compatible = "qcom,sm8650", .data = "qcom/sm8650", },
	{},
};

static int qcom_fw_ensure_init(void)
{
	const struct of_device_id *match;
	struct device_node *root;

	if (fw_path)
		return 0;

	root = of_find_node_by_path("/");
	if (!root)
		return -ENODEV;

	match = of_match_node(qcom_fw_paths, root);
	of_node_put(root);
	if (!match || !match->data) {
		pr_notice("Platform not supported by qcom_fw_helper\n");
		return -ENODEV;
	}

	fw_path = match->data;

	return 0;
}

const char *qcom_get_board_fw(const char *firmware)
{
	if (strchr(firmware, '/'))
		return kstrdup(firmware, GFP_KERNEL);

	scoped_guard(mutex, &qcom_fw_mutex) {
		if (!qcom_fw_ensure_init())
			return kasprintf(GFP_KERNEL, "%s/%s", fw_path, firmware);
	}

	return kstrdup(firmware, GFP_KERNEL);
}
EXPORT_SYMBOL_GPL(qcom_get_board_fw);

const char *devm_qcom_get_board_fw(struct device *dev, const char *firmware)
{
	if (strchr(firmware, '/'))
		return devm_kstrdup(dev, firmware, GFP_KERNEL);

	scoped_guard(mutex, &qcom_fw_mutex) {
		if (!qcom_fw_ensure_init())
			return devm_kasprintf(dev, GFP_KERNEL, "%s/%s", fw_path, firmware);
	}

	return devm_kstrdup(dev, firmware, GFP_KERNEL);
}
EXPORT_SYMBOL_GPL(devm_qcom_get_board_fw);

MODULE_DESCRIPTION("Firmware helpers for Qualcomm devices");
MODULE_LICENSE("GPL");
