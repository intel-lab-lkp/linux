/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright 2024-2025 Tomeu Vizoso <tomeu@tomeuvizoso.net> */

#ifndef __ROCKET_CORE_H__
#define __ROCKET_CORE_H__

#include <drm/gpu_scheduler.h>
#include <linux/clk.h>
#include <linux/hrtimer.h>
#include <linux/io.h>
#include <linux/mutex_types.h>
#include <linux/reset.h>

#include "rocket_registers.h"

#define rocket_pc_readl(core, reg) \
	readl((core)->pc_iomem + (REG_PC_##reg))
#define rocket_pc_writel(core, reg, value) \
	writel(value, (core)->pc_iomem + (REG_PC_##reg))

#define rocket_cna_readl(core, reg) \
	readl((core)->cna_iomem + (REG_CNA_##reg) - REG_CNA_S_STATUS)
#define rocket_cna_writel(core, reg, value) \
	writel(value, (core)->cna_iomem + (REG_CNA_##reg) - REG_CNA_S_STATUS)

#define rocket_core_readl(core, reg) \
	readl((core)->core_iomem + (REG_CORE_##reg) - REG_CORE_S_STATUS)
#define rocket_core_writel(core, reg, value) \
	writel(value, (core)->core_iomem + (REG_CORE_##reg) - REG_CORE_S_STATUS)

#define rocket_dpu_readl(core, reg) \
	readl((core)->dpu_iomem + (REG_DPU_##reg) - REG_DPU_S_STATUS)
#define rocket_dpu_writel(core, reg, value) \
	writel(value, (core)->dpu_iomem + (REG_DPU_##reg) - REG_DPU_S_STATUS)

#define rocket_dpu_rdma_readl(core, reg) \
	readl((core)->dpu_rdma_iomem + (REG_DPU_RDMA_##reg) - REG_DPU_RDMA_RDMA_S_STATUS)
#define rocket_dpu_rdma_writel(core, reg, value) \
	writel(value, (core)->dpu_rdma_iomem + (REG_DPU_RDMA_##reg) - REG_DPU_RDMA_RDMA_S_STATUS)

struct rocket_core {
	struct device *dev;
	struct rocket_device *rdev;
	unsigned int index;

	int irq;
	void __iomem *pc_iomem;
	void __iomem *cna_iomem;
	void __iomem *core_iomem;
	void __iomem *dpu_iomem;
	void __iomem *dpu_rdma_iomem;
	struct clk_bulk_data clks[6];
	struct reset_control_bulk_data resets[1];

	struct iommu_group *iommu_group;

	struct mutex job_lock;
	struct rocket_job *in_flight_job;

	spinlock_t fence_lock;

	struct {
		struct workqueue_struct *wq;
		struct work_struct work;
		atomic_t pending;
	} reset;

	/* RK3576 has no completion IRQ; poll for PC_DONE via hrtimer. */
	struct hrtimer poll_timer;
	struct work_struct poll_work;
	atomic_t poll_active;

	struct drm_gpu_scheduler sched;
	u64 fence_context;
	u64 emit_seqno;
};

int rocket_core_init(struct rocket_core *core);
void rocket_core_fini(struct rocket_core *core);
void rocket_core_reset(struct rocket_core *core);

#endif
