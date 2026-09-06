// SPDX-License-Identifier: GPL-2.0-only
/*
 * SoC Information over Shared Memory.
 *
 * Copyright 2022-2024, Richard Acayan.
 */

#include <linux/err.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/soc/qcom/smem.h>
#include <linux/soc/qcom/socinfo.h>

#define MODEM_SMEM_VERSION 0

#define PLAT_VER_TO_MAJOR_ID(v) (((v) >> 16) & 0xff)
#define PLAT_VER_TO_MINOR_ID(v) ((v) & 0xff)

struct modem_smem_info {
	__le32 version;
	__le32 modem_flag;
	__le32 major_id;
	__le32 minor_id;
	__le32 subtype;
	__le32 platform;
	__le32 efs_magic;
	__le32 ftm_magic;
};

static int write_socinfo(struct modem_smem_info *target)
{
	struct socinfo *socinfo;
	u32 plat_ver;

	socinfo = qcom_smem_get(QCOM_SMEM_HOST_ANY, SMEM_HW_SW_BUILD_ID, NULL);
	if (IS_ERR(socinfo))
		return PTR_ERR(socinfo);

	/* hw_plat_subtype was added in socinfo format 0.6 */
	if (le32_to_cpu(socinfo->fmt) < SOCINFO_VERSION(0, 6))
		return -EOPNOTSUPP;

	plat_ver = le32_to_cpu(socinfo->plat_ver);

	target->version = cpu_to_le32(MODEM_SMEM_VERSION);
	target->major_id = cpu_to_le32(PLAT_VER_TO_MAJOR_ID(plat_ver));
	target->minor_id = cpu_to_le32(PLAT_VER_TO_MINOR_ID(plat_ver));
	target->platform = socinfo->hw_plat;
	target->subtype = socinfo->hw_plat_subtype;

	return 0;
}

static int modemsmem_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct modem_smem_info *info;
	u32 smem_id;
	int ret;

	ret = of_property_read_u32(dev->of_node, "qcom,smem-id", &smem_id);
	if (ret)
		return dev_err_probe(dev, ret, "Could not read smem id\n");

	ret = qcom_smem_alloc(QCOM_SMEM_HOST_ANY, smem_id, sizeof(*info));
	if (ret)
		return dev_err_probe(dev, ret, "Could not allocate modem smem\n");

	info = qcom_smem_get(QCOM_SMEM_HOST_ANY, smem_id, NULL);
	if (IS_ERR(info))
		return dev_err_probe(dev, PTR_ERR(info), "Could not get modem smem\n");

	ret = write_socinfo(info);
	if (ret)
		return dev_err_probe(dev, ret, "Could not submit info to modemsmem\n");

	return 0;
}

static const struct of_device_id modemsmem_of_match[] = {
	{ .compatible = "google,modemsmem" },
	{ }
};
MODULE_DEVICE_TABLE(of, modemsmem_of_match);

static struct platform_driver modemsmem_driver = {
	.probe = modemsmem_probe,
	.driver = {
		.name = "modemsmem",
		.of_match_table = modemsmem_of_match,
	},
};
module_platform_driver(modemsmem_driver);

MODULE_AUTHOR("Richard Acayan <mailingradian@gmail.com>");
MODULE_DESCRIPTION("SoC Information over SMEM driver");
MODULE_LICENSE("GPL");
