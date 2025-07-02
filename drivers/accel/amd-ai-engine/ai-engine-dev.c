// SPDX-License-Identifier: GPL-2.0
/*
 * AMD AI Engine device driver
 *
 * Copyright(C) 2025 Advanced Micro Devices, Inc. All rights reserved.
 */

#include <linux/amd-ai-engine.h>
#include <linux/clk.h>
#include <linux/device.h>
#include <linux/io.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>

#include "ai-engine-internal.h"

/**
 * aie_partition_request() - Request an AI engine partition
 * @dev: AI engine device pointer
 * @req: AI engine partition request arguments
 *
 * Return: pointer to the AI engine partition, error pointer value for failure.
 *
 * This function searches through the aie device aperture list to request a
 * partition given start column and number of columns in @req. If the partition
 * can be found, it will try to request it. User can only use the AI engine
 * partition after it is successfully requested.
 */
void *aie_partition_request(struct device *dev, struct aie_partition_req *req)
{
	struct aie_device *adev = dev_get_drvdata(dev);
	struct aie_partition *apart = NULL;
	struct aie_aperture *laperture;

	if (!req)
		return ERR_PTR(-EINVAL);

	list_for_each_entry(laperture, &adev->apertures, node) {
		apart = aie_aperture_request_part(laperture, req);
		if (PTR_ERR(apart) == -ERANGE) {
			continue;
		} else if (PTR_ERR(apart) == -EBUSY) {
			/* if requesting full aperture, try next aperture in list */
			if (req->num_col == 0)
				continue;
			dev_err(laperture->dev,
				"failed to request partition (%u,%u), already in use.",
				req->start_col, req->num_col);
			return ERR_PTR(PTR_ERR(apart));
		} else if (IS_ERR(apart)) {
			dev_err(laperture->dev,
				"failed to create partition (%u, %u).",
				req->start_col, req->num_col);
			return ERR_PTR(PTR_ERR(apart));
		}
		break;
	}

	if (IS_ERR_OR_NULL(apart)) {
		dev_err(adev->dev,
			"failed to request partition (%u, %u): invalid partition.",
			req->start_col, req->num_col);
		return ERR_PTR(-EINVAL);
	}

	dev_info(adev->dev, "Partition (%u, %u) created successfully.",
		 apart->range.start.col, apart->range.size.col);
	return apart;
}
EXPORT_SYMBOL_GPL(aie_partition_request);

/**
 * aie_partition_release() - Decrease refcount of the AI engine partition
 * @apart: AI engine partition device pointer
 */
void aie_partition_release(void *apart)
{
	aie_part_release((struct aie_partition *)apart);
}
EXPORT_SYMBOL_GPL(aie_partition_release);

static const struct of_device_id amd_aie_aperture_of_match[] = {
	{ .compatible = "xlnx,ai-engine-aperture", },
	{ /* end of table */ },
};
MODULE_DEVICE_TABLE(of, amd_aie_aperture_of_match);

static struct platform_driver amd_aie_aperture_driver = {
	.probe			= aie_aperture_probe,
	.remove			= aie_aperture_remove,
	.driver			= {
		.name		= "amd-aie-aperture",
		.of_match_table	= amd_aie_aperture_of_match,
	},
};

static int amd_ai_engine_probe(struct platform_device *pdev)
{
	struct aie_device *adev;
	u32 pm_reg[2];
	u8 regs_u8[2];
	u8 aie_gen;
	int ret;

	adev = devm_kzalloc(&pdev->dev, sizeof(*adev), GFP_KERNEL);
	if (!adev)
		return -ENOMEM;

	platform_set_drvdata(pdev, adev);
	INIT_LIST_HEAD(&adev->apertures);
	mutex_init(&adev->mlock);

	ret = of_property_read_u8_array(pdev->dev.of_node, "xlnx,shim-rows",
					regs_u8, ARRAY_SIZE(regs_u8));
	if (ret < 0) {
		dev_err(&pdev->dev,
			"no SHIM rows information in device tree");
		return ret;
	}
	adev->ttype_attr[AIE_TILE_TYPE_SHIMPL].start_row = regs_u8[0];
	adev->ttype_attr[AIE_TILE_TYPE_SHIMPL].num_rows = regs_u8[1];
	adev->ttype_attr[AIE_TILE_TYPE_SHIMNOC].start_row = regs_u8[0];
	adev->ttype_attr[AIE_TILE_TYPE_SHIMNOC].num_rows = regs_u8[1];

	ret = of_property_read_u8_array(pdev->dev.of_node, "xlnx,core-rows",
					regs_u8, ARRAY_SIZE(regs_u8));
	if (ret < 0) {
		dev_err(&pdev->dev,
			"Failed to read core rows information");
		return ret;
	}
	adev->ttype_attr[AIE_TILE_TYPE_TILE].start_row = regs_u8[0];
	adev->ttype_attr[AIE_TILE_TYPE_TILE].num_rows = regs_u8[1];

	ret = of_property_read_u8_array(pdev->dev.of_node, "xlnx,mem-rows",
					regs_u8, ARRAY_SIZE(regs_u8));
	if (ret < 0) {
		dev_err(&pdev->dev,
			"Failed to read mem rows information");
		return ret;
	}
	adev->ttype_attr[AIE_TILE_TYPE_MEMORY].start_row = regs_u8[0];
	adev->ttype_attr[AIE_TILE_TYPE_MEMORY].num_rows = regs_u8[1];

	ret = of_property_read_u8(pdev->dev.of_node, "xlnx,aie-gen", &aie_gen);
	if (ret < 0) {
		dev_warn(&pdev->dev,
			 "no aie dev generation information in device tree");
		return ret;
	}
	adev->dev_gen = aie_gen;
	if (aie_gen == AIE_DEVICE_GEN_AIE) {
		aie_device_init(adev);
	} else {
		dev_err(&pdev->dev, "Invalid device generation");
		return -EINVAL;
	}

	/*
	 * AI Engine platform management node ID is required for requesting
	 * services from firmware driver.
	 */
	ret = of_property_read_u32_array(pdev->dev.of_node, "power-domains",
					 pm_reg, ARRAY_SIZE(pm_reg));
	if (ret < 0) {
		dev_err(&pdev->dev,
			"Failed to read power manangement information");
		return ret;
	}
	adev->pm_node_id = pm_reg[1];

	adev->clk = devm_clk_get(&pdev->dev, "aclk0");
	if (IS_ERR(adev->clk)) {
		dev_err(&pdev->dev, "Failed to get device clock.");
		return PTR_ERR(adev->clk);
	}

	adev->dev = &pdev->dev;
	dev_info(&pdev->dev,
		 "AMD AI Engine device %s probed. Device generation: %u. Clock frequency: %ldHz.",
		 dev_name(&pdev->dev), aie_gen, clk_get_rate(adev->clk));
	return of_platform_populate(pdev->dev.of_node, NULL, NULL, &pdev->dev);
}

static const struct of_device_id amd_ai_engine_of_match[] = {
	{ .compatible = "xlnx,ai-engine-v2.0", },
	{ /* end of table */ },
};
MODULE_DEVICE_TABLE(of, amd_ai_engine_of_match);

static struct platform_driver amd_ai_engine_driver = {
	.probe			= amd_ai_engine_probe,
	.driver			= {
		.name		= "amd-ai-engine",
		.of_match_table	= amd_ai_engine_of_match,
	},
};

static int __init amd_ai_engine_init(void)
{
	int ret;

	ret = platform_driver_register(&amd_ai_engine_driver);
	if (ret)
		return ret;

	ret = platform_driver_register(&amd_aie_aperture_driver);
	if (ret) {
		platform_driver_unregister(&amd_ai_engine_driver);
		return ret;
	}

	return 0;
}
module_init(amd_ai_engine_init);

static void __exit amd_ai_engine_exit(void)
{
	platform_driver_unregister(&amd_aie_aperture_driver);
	platform_driver_unregister(&amd_ai_engine_driver);
}
module_exit(amd_ai_engine_exit);

MODULE_AUTHOR("Advanced Micro Devices, Inc.");
MODULE_LICENSE("GPL");
