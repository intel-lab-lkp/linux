// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 ARM Limited.
 */

#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/io.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/reset.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <media/v4l2-dev.h>
#include <media/v4l2-device.h>
#include <media/v4l2-mem2mem.h>

#include "mali-vpu-device.h"
#include "mali-vpu-firmware-cache.h"
#include "mali-vpu-registers.h"
#include "mali-vpu-variant.h"
#include "mali-vpu-vidioc.h"

static void mali_vpu_read_version(struct mali_vpu_dev *vpu_dev, u32 *product_id, u32 *rev,
				  u32 *patch)
{
	u32 pid_prefix, pid_suffix;

	u32 hwver = mali_vpu_read_reg(vpu_dev->regs_base, MALI_VPU_REG_VERSION);

	pid_prefix = FIELD_GET(MALI_VPU_VERSION_PRODUCT_ID_MASK, hwver);
	pid_suffix = FIELD_GET(MALI_VPU_PID_SUFFIX_MASK, hwver);

	*product_id	= (pid_prefix << 4) | pid_suffix;
	*rev		= FIELD_GET(MALI_VPU_VERSION_REVISION_MASK, hwver);
	*patch		= FIELD_GET(MALI_VPU_VERSION_PATCH_MASK, hwver);
}

static int mali_vpu_get_clocks(struct mali_vpu_dev *vpu_dev)
{
	int ret;

	ret = devm_clk_bulk_get_all(vpu_dev->dev, &vpu_dev->clk_list);
	if (ret < 0)
		return dev_err_probe(vpu_dev->dev, ret, "failed to get clocks\n");
	if (!ret)
		return dev_err_probe(vpu_dev->dev, -EINVAL, "no clocks found\n");

	vpu_dev->clk_count = ret;

	return 0;
}

static int mali_vpu_get_resets(struct mali_vpu_dev *vpu_dev)
{
	unsigned int i;
	int ret;

	vpu_dev->rst_count = vpu_dev->variant->num_rsts;
	if (vpu_dev->rst_count == 0)
		return 0;

	vpu_dev->rst_list = devm_kcalloc(vpu_dev->dev, vpu_dev->rst_count,
					 sizeof(*vpu_dev->rst_list), GFP_KERNEL);
	if (!vpu_dev->rst_list)
		return -ENOMEM;

	for (i = 0; i < vpu_dev->rst_count; i++)
		vpu_dev->rst_list[i].id = vpu_dev->variant->rst_names[i];

	ret = devm_reset_control_bulk_get_optional_exclusive(vpu_dev->dev,
							     vpu_dev->rst_count,
							     vpu_dev->rst_list);
	if (ret)
		return dev_err_probe(vpu_dev->dev, ret, "failed to get reset controls\n");

	return 0;
}

static int mali_vpu_enable_clk(struct mali_vpu_dev *vpu_dev)
{
	return clk_bulk_prepare_enable(vpu_dev->clk_count, vpu_dev->clk_list);
}

static void mali_vpu_disable_clk(struct mali_vpu_dev *vpu_dev)
{
	clk_bulk_disable_unprepare(vpu_dev->clk_count, vpu_dev->clk_list);
}

static void mali_vpu_assert_reset(struct mali_vpu_dev *vpu_dev)
{
	if (!vpu_dev->rst_count)
		return;

	reset_control_bulk_assert(vpu_dev->rst_count, vpu_dev->rst_list);
}

static int mali_vpu_deassert_reset(struct mali_vpu_dev *vpu_dev)
{
	if (!vpu_dev->rst_count)
		return 0;

	return reset_control_bulk_deassert(vpu_dev->rst_count, vpu_dev->rst_list);
}

static int mali_vpu_get_resources(struct mali_vpu_dev *vpu_dev)
{
	int ret;

	ret = mali_vpu_get_clocks(vpu_dev);
	if (ret)
		return ret;

	return mali_vpu_get_resets(vpu_dev);
}

static int mali_vpu_register_video_device(struct mali_vpu_dev *vpu_dev,
					  enum mali_vpu_session_role role)
{
	struct video_device *vdev;
	int ret;

	vdev = video_device_alloc();
	if (!vdev)
		return -ENOMEM;

	vdev->release		= video_device_release;
	vdev->fops		= &mali_vpu_fops;
	vdev->vfl_dir		= VFL_DIR_M2M;
	vdev->v4l2_dev		= &vpu_dev->v4l2_dev;
	vdev->device_caps	= V4L2_CAP_VIDEO_M2M_MPLANE | V4L2_CAP_STREAMING;

	if (role == MALI_VPU_DECODER) {
		strscpy(vdev->name, "mali-vpu-decoder", sizeof(vdev->name));
		vdev->ioctl_ops		= &mali_vpu_ioctl_ops_dec;
		vpu_dev->vdev_dec	= vdev;
	} else {
		ret = -EINVAL;
		goto err_vdev_release;
	}

	video_set_drvdata(vdev, vpu_dev);

	ret = video_register_device(vdev, VFL_TYPE_VIDEO, -1);
	if (ret)
		goto err_vdev_release;

	return 0;

err_vdev_release:
	video_device_release(vdev);
	return ret;
}

static int mali_vpu_probe(struct platform_device *pdev)
{
	struct mali_vpu_dev *vpu_dev;
	u32 pid, rev, patch;
	int ret;

	vpu_dev = devm_kzalloc(&pdev->dev, sizeof(*vpu_dev), GFP_KERNEL);
	if (!vpu_dev)
		return -ENOMEM;
	vpu_dev->dev = &pdev->dev;

	vpu_dev->variant = of_device_get_match_data(&pdev->dev);
	if (!vpu_dev->variant)
		return -ENODEV;

	vpu_dev->regs_base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(vpu_dev->regs_base))
		return PTR_ERR(vpu_dev->regs_base);

	ret = platform_get_irq(pdev, 0);
	if (ret < 0)
		return ret;
	vpu_dev->irq = ret;

	ret = mali_vpu_get_resources(vpu_dev);
	if (ret)
		return ret;

	ret = mali_vpu_enable_clk(vpu_dev);
	if (ret) {
		dev_err(vpu_dev->dev, "failed to enable core clock\n");
		return ret;
	}

	ret = mali_vpu_deassert_reset(vpu_dev);
	if (ret) {
		dev_err(vpu_dev->dev, "failed to deassert reset\n");
		goto err_clk;
	}

	mali_vpu_read_version(vpu_dev, &pid, &rev, &patch);
	dev_info(&pdev->dev, "Mali VPU device product ID: %#x, revision: %#x, patch: %#x\n",
		 pid, rev, patch);

	if (pid != vpu_dev->variant->expected_hw_pid) {
		dev_err(vpu_dev->dev, "unexpected hardware product ID: %#x (expected %#x)\n",
			pid, vpu_dev->variant->expected_hw_pid);
		ret = -ENODEV;
		goto err_rst_clk;
	}

	vpu_dev->dma_mask = vpu_dev->variant->dma_mask;
	ret = dma_set_mask_and_coherent(vpu_dev->dev, vpu_dev->dma_mask);
	if (ret)
		goto err_rst_clk;

	dma_set_max_seg_size(vpu_dev->dev, SZ_2G);

	ret = v4l2_device_register(&pdev->dev, &vpu_dev->v4l2_dev);
	if (ret) {
		dev_err(vpu_dev->dev, "failed to register V4L2 device\n");
		goto err_rst_clk;
	}

	vpu_dev->m2m_dev = v4l2_m2m_init(&mali_vpu_m2m_ops);
	if (IS_ERR(vpu_dev->m2m_dev)) {
		ret = PTR_ERR(vpu_dev->m2m_dev);
		goto err_unreg_v4l2;
	}

	vpu_dev->fw_cache = devm_kzalloc(&pdev->dev, sizeof(*vpu_dev->fw_cache), GFP_KERNEL);
	if (!vpu_dev->fw_cache) {
		ret = -ENOMEM;
		goto err_release_m2m;
	}

	ret = mali_vpu_fw_cache_init(vpu_dev->fw_cache, vpu_dev->dev);
	if (ret)
		goto err_release_m2m;

	ret = mali_vpu_register_video_device(vpu_dev, MALI_VPU_DECODER);
	if (ret)
		goto err_cleanup_fw_cache;

	platform_set_drvdata(pdev, vpu_dev);

	return 0;

err_cleanup_fw_cache:
	mali_vpu_fw_cache_cleanup(vpu_dev->fw_cache);
err_release_m2m:
	v4l2_m2m_release(vpu_dev->m2m_dev);
err_unreg_v4l2:
	v4l2_device_unregister(&vpu_dev->v4l2_dev);
err_rst_clk:
	mali_vpu_assert_reset(vpu_dev);
err_clk:
	mali_vpu_disable_clk(vpu_dev);

	return ret;
}

static void mali_vpu_remove(struct platform_device *pdev)
{
	struct mali_vpu_dev *vpu_dev = platform_get_drvdata(pdev);

	video_unregister_device(vpu_dev->vdev_dec);
	mali_vpu_fw_cache_cleanup(vpu_dev->fw_cache);
	v4l2_m2m_release(vpu_dev->m2m_dev);
	v4l2_device_unregister(&vpu_dev->v4l2_dev);

	mali_vpu_assert_reset(vpu_dev);
	mali_vpu_disable_clk(vpu_dev);

	platform_set_drvdata(pdev, NULL);
}

static const struct of_device_id mali_vpu_dt_match[] = {
	{
		.compatible = "arm,mali-v76",
		.data = &mali_vpu_v76_variant,
	},
	{},
};
MODULE_DEVICE_TABLE(of, mali_vpu_dt_match);

static struct platform_driver mali_vpu_driver = {
	.probe = mali_vpu_probe,
	.remove = mali_vpu_remove,
	.driver = {
		.name = MALI_VPU_DRV_NAME,
		.of_match_table = mali_vpu_dt_match,
	},
};

module_platform_driver(mali_vpu_driver);
MODULE_DESCRIPTION("Arm Mali VPU driver");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("DMA_BUF");
