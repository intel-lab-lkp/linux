// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2025 MediaTek Inc.
 * Author: Ning li <ning.li@mediatek.com>
 * Author: Xueqi Zhang <xueqi.zhang@mediatek.com>
 */
#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_platform.h>

#include "arm-smmu-v3.h"

#define MTK_SMMU_COMP_STR_LEN		64
#define SMMU_REQUIRE_PARENT		BIT(5)
#define MTK_SMMU_HAS_FLAG(pdata, _x)    (!!(((pdata)->flags) & (_x)))

enum mtk_smmu_type {
	MTK_SMMU_MM,
	MTK_SMMU_APU,
	MTK_SMMU_TYPE_NUM,
};

struct mtk_smmu_v3_plat {
	enum mtk_smmu_type	smmu_type;
	u32			flags;
};

struct mtk_smmu_v3 {
	struct arm_smmu_device	smmu;
	const struct mtk_smmu_v3_plat *plat_data;
};

static const struct mtk_smmu_v3_plat mt8196_data_mm = {
	.smmu_type		= MTK_SMMU_MM,
};

static const struct mtk_smmu_v3_plat mt8196_data_apu = {
	.smmu_type		= MTK_SMMU_APU,
	.flags			= SMMU_REQUIRE_PARENT,
};

struct mtk_smmu_v3_of_device_data {
	char			compatible[MTK_SMMU_COMP_STR_LEN];
	const void		*data;
};

static const struct mtk_smmu_v3_of_device_data mtk_smmu_v3_of_ids[] = {
	{ .compatible = "mediatek,mt8196-apu-smmu", .data = &mt8196_data_apu},
	{ .compatible = "mediatek,mt8196-mm-smmu", .data = &mt8196_data_mm},
};

static inline struct mtk_smmu_v3 *to_mtk_smmu_v3(struct arm_smmu_device *smmu)
{
	return container_of(smmu, struct mtk_smmu_v3, smmu);
}

static const struct mtk_smmu_v3_plat *mtk_smmu_v3_get_plat_data(const struct device_node *np)
{
	const struct mtk_smmu_v3_of_device_data *of_device = mtk_smmu_v3_of_ids;
	int i;

	for (i = 0; i < ARRAY_SIZE(mtk_smmu_v3_of_ids); i++, of_device++) {
		if (of_device_is_compatible(np, of_device->compatible))
			return of_device->data;
	}
	return NULL;
}

struct arm_smmu_device *arm_smmu_v3_impl_mtk_init(struct arm_smmu_device *smmu)
{
	struct mtk_smmu_v3 *mtk_smmu_v3;
	struct device *dev = smmu->dev;
	struct platform_device *parent_pdev;
	struct device_node *parent_node;

	mtk_smmu_v3 = devm_krealloc(dev, smmu, sizeof(*mtk_smmu_v3), GFP_KERNEL);
	if (!mtk_smmu_v3)
		return ERR_PTR(-ENOMEM);

	mtk_smmu_v3->plat_data = mtk_smmu_v3_get_plat_data(dev->of_node);
	if (!mtk_smmu_v3->plat_data) {
		dev_err(dev, "Get platform data fail\n");
		return ERR_PTR(-EINVAL);
	}

	if (MTK_SMMU_HAS_FLAG(mtk_smmu_v3->plat_data, SMMU_REQUIRE_PARENT)) {
		parent_node = of_parse_phandle(dev->of_node, "mediatek,smmu-parent", 0);
		if (!parent_node) {
			dev_err(dev, "Lack its parent node.\n");
			return ERR_PTR(-EINVAL);
		}
		if (!of_device_is_available(parent_node)) {
			of_node_put(parent_node);
			return ERR_PTR(-EINVAL);
		}

		parent_pdev = of_find_device_by_node(parent_node);
		of_node_put(parent_node);
		if (!parent_pdev) {
			dev_err(dev, "Lack its parent devices.\n");
			return ERR_PTR(-ENODEV);
		}

		if (!platform_get_drvdata(parent_pdev)) {
			dev_err(dev, "Delay since its parent driver is not ready.\n");
			return ERR_PTR(-EPROBE_DEFER);
		}
	}

	return &mtk_smmu_v3->smmu;
}
