// SPDX-License-Identifier: (GPL-2.0 OR BSD-3-Clause)
/*
 * Wave6 series multi-standard codec IP - wave6 driver
 *
 * Copyright (C) 2025 CHIPS&MEDIA INC
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/clk.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/dma-mapping.h>
#include <linux/pm_runtime.h>
#include "wave6-vpu.h"

#define VPU_PLATFORM_DEVICE_NAME "wave6-vpu"
#define VPU_CLK_NAME "vpu"

#define WAVE6_VPU_FLAG_SLEEP	BIT(0)
#define WAVE6_VPU_FLAG_WAKEUP	BIT(1)

/**
 * wave6_alloc_dma - Allocate DMA memory
 * @dev: device pointer
 * @vb: VPU buffer structure
 *
 * Allocates a contiguous DMA memory region for VPU usage.
 * The allocated memory information is stored in the given
 * @vb structure.
 *
 * Return: 0 on success, -EINVAL for invalid arguments, -ENOMEM on failure
 */
int wave6_alloc_dma(struct device *dev, struct vpu_buf *vb)
{
	void *vaddr;
	dma_addr_t daddr;

	if (!vb || !vb->size)
		return -EINVAL;

	vaddr = dma_alloc_coherent(dev, vb->size, &daddr, GFP_KERNEL);
	if (!vaddr)
		return -ENOMEM;

	vb->vaddr = vaddr;
	vb->daddr = daddr;
	vb->dev = dev;

	return 0;
}
EXPORT_SYMBOL_GPL(wave6_alloc_dma);

/**
 * wave6_free_dma - Free DMA memory
 * @vb: VPU buffer structure
 *
 * Frees the DMA memory previously allocated by wave6_alloc_dma().
 * @vb structure is also cleared to zero.
 */
void wave6_free_dma(struct vpu_buf *vb)
{
	if (!vb || !vb->size || !vb->vaddr)
		return;

	dma_free_coherent(vb->dev, vb->size, vb->vaddr, vb->daddr);
	memset(vb, 0, sizeof(*vb));
}
EXPORT_SYMBOL_GPL(wave6_free_dma);

static int wave6_check_entity(struct wave6_vpu_device *vpu,
			      struct wave6_vpu_entity *entity)
{
	if (!entity || !entity->vpu || !vpu || entity->vpu != vpu->dev)
		return -EINVAL;
	if (entity->index < 0 || entity->index >= WAVE6_VPU_MAXIMUM_ENTITY_CNT)
		return -EINVAL;
	if (entity != vpu->entities[entity->index])
		return -EINVAL;

	return 0;
}

static unsigned long wave6_vpu_get_clk_rate(struct wave6_vpu_device *vpu)
{
	unsigned long rate = 0;
	int i;

	mutex_lock(&vpu->lock);

	for (i = 0; i < vpu->num_clks; i++) {
		if (vpu->clks[i].id && !strcmp(vpu->clks[i].id, VPU_CLK_NAME))
			rate = clk_get_rate(vpu->clks[i].clk);
	}

	mutex_unlock(&vpu->lock);
	return rate;
}

static int __wave6_vpu_get(struct wave6_vpu_device *vpu,
			   struct wave6_vpu_entity *entity)
{
	int ret;

	if (atomic_inc_return(&vpu->ref_count) > 1)
		return 0;

	ret = pm_runtime_resume_and_get(vpu->dev);
	if (ret) {
		dev_err(vpu->dev, "pm runtime resume fail, ret = %d\n", ret);
		atomic_dec(&vpu->ref_count);
		return -EINVAL;
	}

	if (vpu->ctrl && vpu->ctrl_ops) {
		ret = vpu->ctrl_ops->get_ctrl(vpu->ctrl, entity);
		if (ret) {
			dev_err(vpu->dev, "get ctrl fail, ret = %d\n", ret);
			pm_runtime_put_sync(vpu->dev);
			atomic_dec(&vpu->ref_count);
			return ret;
		}
	}

	return 0;
}

static int wave6_vpu_get(struct wave6_vpu_device *vpu,
			 struct wave6_vpu_entity *entity)
{
	int ret = 0;

	mutex_lock(&vpu->lock);

	if (wave6_check_entity(vpu, entity)) {
		ret = -EINVAL;
		goto unlock;
	}

	if (!entity->active)
		goto unlock;

	ret = __wave6_vpu_get(vpu, entity);

unlock:
	mutex_unlock(&vpu->lock);
	return ret;
}

static void __wave6_vpu_put(struct wave6_vpu_device *vpu,
			    struct wave6_vpu_entity *entity)
{
	if (atomic_dec_return(&vpu->ref_count) > 0)
		return;

	if (vpu->ctrl && vpu->ctrl_ops)
		vpu->ctrl_ops->put_ctrl(vpu->ctrl, entity);

	pm_runtime_put_sync(vpu->dev);
}

static void wave6_vpu_put(struct wave6_vpu_device *vpu,
			  struct wave6_vpu_entity *entity)
{
	mutex_lock(&vpu->lock);

	if (wave6_check_entity(vpu, entity))
		goto unlock;

	if (!entity->active)
		goto unlock;

	__wave6_vpu_put(vpu, entity);

unlock:
	mutex_unlock(&vpu->lock);
}

static void wave6_support_follower(struct wave6_vpu_device *vpu,
				   struct wave6_vpu_entity *entity, u32 flag)
{
	struct wave6_vpu_entity *target = NULL;
	int ret;
	int i;

	if (!vpu->support_follower)
		return;
	if (!vpu->ctrl)
		return;

	if (entity)
		target = entity;

	ret = pm_runtime_resume_and_get(vpu->dev);
	if (ret) {
		dev_warn(vpu->dev, "pm runtime resume fail, ret = %d\n", ret);
		return;
	}

	for (i = 0; i < ARRAY_SIZE(vpu->entities); i++) {
		if (!vpu->entities[i])
			continue;
		if (target && vpu->entities[i] != target)
			continue;
		if (flag & WAVE6_VPU_FLAG_WAKEUP)
			__wave6_vpu_get(vpu, vpu->entities[i]);
		if (flag & WAVE6_VPU_FLAG_SLEEP)
			__wave6_vpu_put(vpu, vpu->entities[i]);
	}

	pm_runtime_put_sync(vpu->dev);
}

static int wave6_find_unused_index(struct wave6_vpu_device *vpu)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(vpu->entities); i++) {
		if (!vpu->entities[i])
			return i;
	}

	return -1;
}

static int wave6_register_vpu_core(struct wave6_vpu_device *vpu,
				   struct wave6_vpu_entity *entity)
{
	int ret = 0;
	int index;

	mutex_lock(&vpu->lock);

	if (!entity || !entity->dev) {
		ret = -EINVAL;
		goto unlock;
	}

	index = wave6_find_unused_index(vpu);
	if (index < 0 || index >= ARRAY_SIZE(vpu->entities)) {
		ret = -1;
		goto unlock;
	}

	entity->vpu = vpu->dev;
	entity->index = index;
	vpu->entities[index] = entity;
	wave6_support_follower(vpu, entity, WAVE6_VPU_FLAG_WAKEUP);

unlock:
	mutex_unlock(&vpu->lock);
	return ret;
}

static void wave6_unregister_vpu_core(struct wave6_vpu_device *vpu,
				      struct wave6_vpu_entity *entity)
{
	mutex_lock(&vpu->lock);

	if (wave6_check_entity(vpu, entity))
		goto unlock;

	wave6_support_follower(vpu, entity, WAVE6_VPU_FLAG_SLEEP);
	vpu->entities[entity->index] = NULL;
	entity->vpu = NULL;
	entity->index = -1;

unlock:
	mutex_unlock(&vpu->lock);
}

static int wave6_register_vpu_ctrl(struct wave6_vpu_device *vpu,
				   struct device *ctrl,
				   const struct wave6_vpu_ctrl_ops *ops)
{
	int ret = 0;

	mutex_lock(&vpu->lock);

	if (!ctrl || !ops) {
		ret = -EINVAL;
		goto unlock;
	}

	if (vpu->ctrl) {
		if (vpu->ctrl != ctrl)
			ret = -EINVAL;

		goto unlock;
	}

	vpu->ctrl = ctrl;
	vpu->ctrl_ops = ops;
	wave6_support_follower(vpu, NULL, WAVE6_VPU_FLAG_WAKEUP);

unlock:
	mutex_unlock(&vpu->lock);
	return ret;
}

static void wave6_unregister_vpu_ctrl(struct wave6_vpu_device *vpu,
				      struct device *ctrl)
{
	mutex_lock(&vpu->lock);

	if (vpu->ctrl != ctrl)
		goto unlock;

	wave6_support_follower(vpu, NULL, WAVE6_VPU_FLAG_SLEEP);
	vpu->ctrl = NULL;

unlock:
	mutex_unlock(&vpu->lock);
}

static void wave6_require_work_buffer(struct wave6_vpu_device *vpu,
				      struct wave6_vpu_entity *entity)
{
	int ret = 0;

	mutex_lock(&vpu->lock);

	if (wave6_check_entity(vpu, entity))
		goto unlock;

	if (!vpu->ctrl || !vpu->ctrl_ops)
		goto unlock;

	ret = vpu->ctrl_ops->require_work_buffer(vpu->ctrl, entity);
	if (ret)
		dev_warn(vpu->dev, "require_work_buffer fail %d\n", ret);

unlock:
	mutex_unlock(&vpu->lock);
}

static const struct wave6_vpu_ops wave6_vpu_ops = {
	.get_vpu = wave6_vpu_get,
	.put_vpu = wave6_vpu_put,
	.reg_core = wave6_register_vpu_core,
	.unreg_core = wave6_unregister_vpu_core,
	.reg_ctrl = wave6_register_vpu_ctrl,
	.unreg_ctrl = wave6_unregister_vpu_ctrl,
	.req_work_buffer = wave6_require_work_buffer,
	.get_clk_rate = wave6_vpu_get_clk_rate,
};

static int wave6_vpu_probe(struct platform_device *pdev)
{
	struct wave6_vpu_device *vpu;
	int ret;

	vpu = devm_kzalloc(&pdev->dev, sizeof(*vpu), GFP_KERNEL);
	if (!vpu)
		return -ENOMEM;

	dev_set_drvdata(&pdev->dev, vpu);
	vpu->dev = &pdev->dev;
	vpu->ops = &wave6_vpu_ops;

	mutex_init(&vpu->lock);
	atomic_set(&vpu->ref_count, 0);

	ret = devm_clk_bulk_get_all(&pdev->dev, &vpu->clks);
	if (ret < 0) {
		dev_warn(&pdev->dev, "unable to get clocks: %d\n", ret);
		ret = 0;
	}
	vpu->num_clks = ret;

	pm_runtime_enable(&pdev->dev);

#if IS_ENABLED(CONFIG_VIDEO_WAVE6_VPU_SUPPORT_FOLLOWER)
	vpu->support_follower = true;
#endif
	if (vpu->support_follower) {
		ret = pm_runtime_resume_and_get(&pdev->dev);
		if (ret) {
			dev_warn(&pdev->dev, "pm resume fail %d\n", ret);
			vpu->support_follower = false;
		}
	}

	of_platform_populate(pdev->dev.of_node, NULL, NULL, &pdev->dev);

	return 0;
}

static void wave6_vpu_remove(struct platform_device *pdev)
{
	struct wave6_vpu_device *vpu = dev_get_drvdata(&pdev->dev);

	if (vpu->support_follower) {
		if (!pm_runtime_suspended(&pdev->dev))
			pm_runtime_put_sync(&pdev->dev);

		wave6_support_follower(vpu, NULL, WAVE6_VPU_FLAG_SLEEP);
	}

	pm_runtime_disable(&pdev->dev);
	mutex_destroy(&vpu->lock);
}

static int __maybe_unused wave6_vpu_runtime_suspend(struct device *dev)
{
	struct wave6_vpu_device *vpu = dev_get_drvdata(dev);

	if (!vpu->num_clks)
		return 0;

	clk_bulk_disable_unprepare(vpu->num_clks, vpu->clks);
	return 0;
}

static int __maybe_unused wave6_vpu_runtime_resume(struct device *dev)
{
	struct wave6_vpu_device *vpu = dev_get_drvdata(dev);

	if (!vpu->num_clks)
		return 0;

	return clk_bulk_prepare_enable(vpu->num_clks, vpu->clks);
}

static int __maybe_unused wave6_vpu_suspend(struct device *dev)
{
	struct wave6_vpu_device *vpu = dev_get_drvdata(dev);

	wave6_support_follower(vpu, NULL, WAVE6_VPU_FLAG_SLEEP);

	return 0;
}

static int __maybe_unused wave6_vpu_resume(struct device *dev)
{
	struct wave6_vpu_device *vpu = dev_get_drvdata(dev);

	wave6_support_follower(vpu, NULL, WAVE6_VPU_FLAG_WAKEUP);

	return 0;
}

static const struct dev_pm_ops wave6_vpu_pm_ops = {
	SET_RUNTIME_PM_OPS(wave6_vpu_runtime_suspend,
			   wave6_vpu_runtime_resume, NULL)
	SET_SYSTEM_SLEEP_PM_OPS(wave6_vpu_suspend,
				wave6_vpu_resume)
};

static const struct of_device_id wave6_vpu_ids[] = {
	{ .compatible = "nxp,imx95-vpu" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, wave6_vpu_ids);

static struct platform_driver wave6_vpu_driver = {
	.driver = {
		.name = VPU_PLATFORM_DEVICE_NAME,
		.of_match_table = wave6_vpu_ids,
		.pm = &wave6_vpu_pm_ops,
	},
	.probe = wave6_vpu_probe,
	.remove = wave6_vpu_remove,
};

module_platform_driver(wave6_vpu_driver);
MODULE_DESCRIPTION("chips&media Wave6 VPU driver");
MODULE_LICENSE("Dual BSD/GPL");
