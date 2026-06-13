// SPDX-License-Identifier: GPL-2.0-only
/* Copyright 2024-2025 Tomeu Vizoso <tomeu@tomeuvizoso.net> */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/dev_printk.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/iommu.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/reset.h>

#include "rocket_core.h"
#include "rocket_drv.h"
#include "rocket_job.h"

int rocket_core_init(struct rocket_core *core)
{
	struct device *dev = core->dev;
	struct platform_device *pdev = to_platform_device(dev);
	u32 version;
	int err = 0;

	core->soc_data = of_device_get_match_data(dev);
	if (!core->soc_data)
		return dev_err_probe(dev, -EINVAL, "missing SoC match data\n");

	core->resets[0].id = "srst_a";
	core->resets[1].id = "srst_h";
	err = devm_reset_control_bulk_get_exclusive(&pdev->dev, ARRAY_SIZE(core->resets),
						    core->resets);
	if (err)
		return dev_err_probe(dev, err, "failed to get resets for core %d\n", core->index);

	err = devm_clk_bulk_get(dev, ARRAY_SIZE(core->clks), core->clks);
	if (err)
		return dev_err_probe(dev, err, "failed to get clocks for core %d\n", core->index);

	core->pc_iomem = devm_platform_ioremap_resource_byname(pdev, "pc");
	if (IS_ERR(core->pc_iomem)) {
		dev_err(dev, "couldn't find PC registers %ld\n", PTR_ERR(core->pc_iomem));
		return PTR_ERR(core->pc_iomem);
	}

	core->cna_iomem = devm_platform_ioremap_resource_byname(pdev, "cna");
	if (IS_ERR(core->cna_iomem)) {
		dev_err(dev, "couldn't find CNA registers %ld\n", PTR_ERR(core->cna_iomem));
		return PTR_ERR(core->cna_iomem);
	}

	core->core_iomem = devm_platform_ioremap_resource_byname(pdev, "core");
	if (IS_ERR(core->core_iomem)) {
		dev_err(dev, "couldn't find CORE registers %ld\n", PTR_ERR(core->core_iomem));
		return PTR_ERR(core->core_iomem);
	}

	dma_set_max_seg_size(dev, UINT_MAX);

	err = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(core->soc_data->dma_bits));
	if (err)
		return err;

	core->iommu_group = iommu_group_get(dev);

	err = rocket_job_init(core);
	if (err) {
		iommu_group_put(core->iommu_group);
		core->iommu_group = NULL;
		return err;
	}

	pm_runtime_use_autosuspend(dev);

	/*
	 * As this NPU will be most often used as part of a media pipeline that
	 * ends presenting in a display, choose 50 ms (~3 frames at 60Hz) as an
	 * autosuspend delay as that will keep the device powered up while the
	 * pipeline is running.
	 */
	pm_runtime_set_autosuspend_delay(dev, 50);

	pm_runtime_enable(dev);

	err = pm_runtime_resume_and_get(dev);
	if (err) {
		rocket_core_fini(core);
		return err;
	}

	if (core->soc_data->noc_init) {
		err = core->soc_data->noc_init(core);
		if (err) {
			pm_runtime_put_sync(dev);
			rocket_core_fini(core);
			return err;
		}
	}

	version = rocket_pc_readl(core, VERSION);
	version += rocket_pc_readl(core, VERSION_NUM) & 0xffff;

	pm_runtime_mark_last_busy(dev);
	pm_runtime_put_autosuspend(dev);

	dev_info(dev, "Rockchip NPU core %d version: %d\n", core->index, version);

	return 0;
}

void rocket_core_fini(struct rocket_core *core)
{
	pm_runtime_dont_use_autosuspend(core->dev);
	pm_runtime_disable(core->dev);

	/*
	 * Stop the scheduler before tearing down the IOMMU so an in-flight
	 * job can no longer touch the (about to be detached) domain.
	 */
	rocket_job_fini(core);

	if (core->attached_domain) {
		iommu_detach_group(NULL, core->iommu_group);
		rocket_iommu_domain_put(core->attached_domain);
		core->attached_domain = NULL;
	}
	iommu_group_put(core->iommu_group);
	core->iommu_group = NULL;
}

void rocket_core_reset(struct rocket_core *core)
{
	reset_control_bulk_assert(ARRAY_SIZE(core->resets), core->resets);

	udelay(10);

	reset_control_bulk_deassert(ARRAY_SIZE(core->resets), core->resets);
}
