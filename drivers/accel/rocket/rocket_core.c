// SPDX-License-Identifier: GPL-2.0-only
/* Copyright 2024-2025 Tomeu Vizoso <tomeu@tomeuvizoso.net> */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/dev_printk.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/iommu.h>
#include <linux/platform_device.h>
#include <linux/pm_domain.h>
#include <linux/pm_runtime.h>
#include <linux/reset.h>

#include "rocket_core.h"
#include "rocket_job.h"

int rocket_core_init(struct rocket_core *core)
{
	struct device *dev = core->dev;
	struct platform_device *pdev = to_platform_device(dev);
	u32 version;
	int err = 0;

	core->resets[0].id = "srst_a";
	err = devm_reset_control_bulk_get_exclusive(&pdev->dev, ARRAY_SIZE(core->resets),
						    core->resets);
	if (err)
		return dev_err_probe(dev, err, "failed to get resets for core %d\n", core->index);

	core->clks[0].id = "aclk";
	core->clks[1].id = "hclk";
	core->clks[2].id = "npu";
	core->clks[3].id = "pclk";
	/*
	 * RK3576: the CBUF (convolution buffer) has its own clock domain. The CNA
	 * fills the CBUF and CORE reads from it; without these the compute path
	 * stalls after loading one slice (RDMA, which bypasses the CBUF, still
	 * runs). The vendor keeps all NPU clocks on whenever powered.
	 */
	core->clks[4].id = "aclk_cbuf";
	core->clks[5].id = "hclk_cbuf";
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

	core->dpu_iomem = devm_platform_ioremap_resource_byname(pdev, "dpu");
	if (IS_ERR(core->dpu_iomem)) {
		dev_warn(dev, "no DPU registers; DPU S_POINTER won't be pre-armed\n");
		core->dpu_iomem = NULL;
	}

	core->dpu_rdma_iomem = devm_platform_ioremap_resource_byname(pdev, "dpu_rdma");
	if (IS_ERR(core->dpu_rdma_iomem)) {
		dev_warn(dev, "no DPU_RDMA registers; DPU_RDMA S_POINTER won't be pre-armed\n");
		core->dpu_rdma_iomem = NULL;
	}

	dma_set_max_seg_size(dev, UINT_MAX);

	err = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(40));
	if (err)
		return err;

	core->iommu_group = iommu_group_get(dev);

	err = rocket_job_init(core);
	if (err) {
		iommu_group_put(core->iommu_group);
		core->iommu_group = NULL;
		return err;
	}

	/*
	 * RK3576: the NPU spans TWO power domains (PD_NPU0 + PD_NPU1). The vendor
	 * powers BOTH from its single NPU node even when computing on one core --
	 * the CBUF->CMAC read path only works fully with NPU1 powered. The board DT
	 * lists both power-domains on rknn_core_0; a multi-PD device skips the
	 * driver-core single-PD auto-attach, so attach the list explicitly. With
	 * one PD in DT this is a no-op (returns 1) and behaves as before.
	 */
	{
		struct dev_pm_domain_list *pd_list;

		err = devm_pm_domain_attach_list(dev, NULL, &pd_list);
		if (err < 0)
			return dev_err_probe(dev, err,
					     "failed to attach NPU power domains\n");
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
	iommu_group_put(core->iommu_group);
	core->iommu_group = NULL;
	rocket_job_fini(core);
}

void rocket_core_reset(struct rocket_core *core)
{
	reset_control_bulk_assert(ARRAY_SIZE(core->resets), core->resets);

	udelay(10);

	reset_control_bulk_deassert(ARRAY_SIZE(core->resets), core->resets);
}
