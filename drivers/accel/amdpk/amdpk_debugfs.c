// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2025 Advanced Micro Devices, Inc.
 */

#include <linux/seq_file.h>
#include <drm/drm_debugfs.h>
#include "amdpk_drv.h"

static int amdpk_hw_info(struct seq_file *s, void *unused)
{
	struct drm_debugfs_entry *entry = s->private;
	u64 maxtotalreqs, rqmaxpending, mults;
	struct amdpk_dev *pkdev;
	u64 v, hwv, cnt;

	pkdev = to_amdpk_dev(entry->dev);

	v = pk_rdreg(pkdev->regs, REG_SEMVER);
	seq_printf(s, "Hardware interface version: %lld.%lld.%lld\n",
		   AMDPK_SEMVER_MAJOR(v), AMDPK_SEMVER_MINOR(v), AMDPK_SEMVER_PATCH(v));

	hwv = pk_rdreg(pkdev->regs, REG_HW_VERSION);
	seq_printf(s, "Hardware implementation version: %lld.%lld.%lld\n",
		   AMDPK_HWVER_MAJOR(hwv), AMDPK_HWVER_MINOR(hwv), AMDPK_HWVER_SVN(hwv));

	cnt = pk_rdreg(pkdev->regs, REG_CFG_REQ_QUEUES_CNT);
	seq_printf(s, "Count request queues: %lld\n", cnt);

	maxtotalreqs = pk_rdreg(pkdev->regs, REG_CFG_MAX_PENDING_REQ);
	seq_printf(s, "Total max pending requests: %lld\n", maxtotalreqs);

	rqmaxpending = pk_rdreg(pkdev->regs, REG_CFG_MAX_REQ_QUEUE_ENTRIES);
	seq_printf(s, "Total max pending requests: %lld\n", rqmaxpending);

	mults = pk_rdreg(pkdev->regs, REG_CFG_PK_INST);
	seq_printf(s, "Pkcores 64 multipliers: %lld\n", mults >> 16);
	seq_printf(s, "Pkcores 256 multipliers: %lld\n", mults & 0xFFFF);

	return 0;
}

static int amdpk_hw_config(struct seq_file *s, void *unused)
{
	struct drm_debugfs_entry *entry = s->private;
	struct amdpk_dev *pkdev;
	u64 addr, size, depth;
	int i, j;

	pkdev = to_amdpk_dev(entry->dev);
	for (i = 0; i < pkdev->max_queues; i++) {
		seq_printf(s, "Queue-%d:\n", i);
		for (j = 0; j < MAX_RQMEM_PER_QUEUE; j++) {
			addr = pk_rdreg(pkdev->regs, REG_RQ_CFG_PAGE(i, j));
			seq_printf(s, "    page_addr[%d]: %llx\n", j, addr);
		}
		size = pk_rdreg(pkdev->regs, REG_RQ_CFG_PAGE_SIZE(i));
		seq_printf(s, "    page_size: %lld\n", size);
		depth = pk_rdreg(pkdev->regs, REG_RQ_CFG_DEPTH(i));
		seq_printf(s, "    page_depth: %lld\n", depth);
	}

	return 0;
}

static int amdpk_cycle_count(struct seq_file *s, void *unused)
{
	struct drm_debugfs_entry *entry = s->private;
	u64 busy_cycles, idle_cycles;
	struct amdpk_dev *pkdev;

	pkdev = to_amdpk_dev(entry->dev);
	busy_cycles = pk_rdreg(pkdev->regs, REG_PK_BUSY_CYCLES);
	seq_printf(s, "PK busy cycles: %lld\n", busy_cycles);
	idle_cycles = pk_rdreg(pkdev->regs, REG_PK_IDLE_CYCLES);
	seq_printf(s, "PK idle cycles: %lld\n", idle_cycles);

	return 0;
}

static int amdpk_pending_reqs(struct seq_file *s, void *unused)
{
	struct drm_debugfs_entry *entry = s->private;
	struct amdpk_dev *pkdev;
	u64 pending_reqs;
	int i;

	pkdev = to_amdpk_dev(entry->dev);
	for (i = 0; i < pkdev->max_queues; i++) {
		pending_reqs = pk_rdreg(pkdev->regs, REG_CTL_BASE(i) + REG_CTL_PENDING_REQS);
		seq_printf(s, "Queue-%d pending requests: %lld\n", i, pending_reqs);
	}

	return 0;
}

static const struct drm_debugfs_info amdpk_debugfs_list[] = {
	{"hw_info", amdpk_hw_info, 0},
	{"hw_config", amdpk_hw_config, 0},
	{"cycle_count", amdpk_cycle_count, 0},
	{"pending_reqs", amdpk_pending_reqs, 0},
};

void amdpk_debugfs_init(struct amdpk_dev *pkdev)
{
	drm_debugfs_add_files(&pkdev->ddev, amdpk_debugfs_list, ARRAY_SIZE(amdpk_debugfs_list));
}
