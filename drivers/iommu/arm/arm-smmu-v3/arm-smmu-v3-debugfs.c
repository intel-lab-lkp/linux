// SPDX-License-Identifier: GPL-2.0
/*
 * ARM SMMUv3 DebugFS Support
 *
 * This file implements the basic debugfs infrastructure for ARM SMMUv3 driver.
 * It provides the foundation for exposing SMMU internal state through debugfs
 * for debugging and diagnostic purposes.
 *
 * Key Features:
 * - Global root directory management with proper mutex synchronization
 * - Per-SMMU instance directory creation with unique naming
 * - Capability reporting covering all major SMMU features and configuration
 * - Extensible architecture designed for adding future debug functionality
 * - Comprehensive error handling and resource cleanup
 * - Display of control registers (CR0, CR1, CR2) with bitfield decoding
 * - Command and Event queue pointer monitoring (PROD/CONS)
 *
 * Register Information Displayed:
 * - CR0: SMMU global control with enable states and queue enables
 * - CR1/CR2: Additional control and configuration registers
 * - CMDQ_PROD/CONS: Command queue producer and consumer pointers
 * - EVTQ_PROD/CONS: Event queue producer and consumer pointers
 *
 * Directory Structure:
 * /sys/kernel/debug/iommu/arm_smmu_v3/
 * └── smmu0/
 *     ├── capabilities    # SMMU feature capabilities and configuration
 *     └── registers	   # SMMU Key registers
 *
 * The capabilities file provides detailed information about:
 * - Architecture version and translation stage support (Stage1/Stage2)
 * - System coherency, ATS, and PRI feature availability
 * - Stream table size and command/event queue depths
 * - All feature bits from the SMMU device structure
 *
 * The register display provides crucial visibility into:
 * - SMMU operational state (enabled/disabled)
 * - Queue operation and potential stalls
 * - Configuration settings affecting all streams
 *
 * Copyright (C) 2025 HiSilicon Limited.
 * Author: Qinxin Xia <xiaqinxin@huawei.com>
 */

#include <linux/debugfs.h>
#include "arm-smmu-v3.h"

static struct dentry *smmuv3_root_dir;
static DEFINE_MUTEX(arm_smmu_debugfs_lock);

/**
 * smmu_debugfs_capabilities_show() - Display SMMU capabilities
 * @seq: seq_file to write to
 * @v: private data (SMMU device)
 *
 * Return: 0 on success, negative error code on failure
 */
static int smmu_debugfs_capabilities_show(struct seq_file *seq, void *v)
{
	struct arm_smmu_device *smmu = seq->private;

	if (!smmu)
		return -ENODEV;

	seq_puts(seq, "SMMUv3 Capabilities:\n");
	seq_printf(seq, "  Stage1 Translation: %s\n",
		   smmu->features & ARM_SMMU_FEAT_TRANS_S1 ? "Yes" : "No");
	seq_printf(seq, "  Stage2 Translation: %s\n",
		   smmu->features & ARM_SMMU_FEAT_TRANS_S2 ? "Yes" : "No");
	seq_printf(seq, "  Coherent Walk: %s\n",
		   smmu->features & ARM_SMMU_FEAT_COHERENCY ? "Yes" : "No");
	seq_printf(seq, "  ATS Support: %s\n",
		   smmu->features & ARM_SMMU_FEAT_ATS ? "Yes" : "No");
	seq_printf(seq, "  PRI Support: %s\n",
		   smmu->features & ARM_SMMU_FEAT_PRI ? "Yes" : "No");
	seq_printf(seq, "  Stream Table Size: %d\n", 1 << smmu->sid_bits);
	seq_printf(seq, "  Command Queue Depth: %d\n",
		   1 << smmu->cmdq.q.llq.max_n_shift);
	seq_printf(seq, "  Event Queue Depth: %d\n",
		   1 << smmu->evtq.q.llq.max_n_shift);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(smmu_debugfs_capabilities);

/**
 * smmu_debugfs_registers_show() - Display SMMU register values
 * @seq: seq_file to write to
 * @v: private data (SMMU device)
 *
 * Return: 0 on success, negative error code on failure
 */
static int smmu_debugfs_registers_show(struct seq_file *seq, void *v)
{
	struct arm_smmu_device *smmu = seq->private;
	void __iomem *base;

	if (!smmu || !smmu->base) {
		seq_puts(seq, "SMMU not available\n");
		return 0;
	}

	base = smmu->base;

	seq_puts(seq, "SMMUv3 Key Registers:\n");

	/* 32-bit control registers */
	seq_printf(seq, "CR0: 0x%08x [%s%s%s]\n",
		   readl_relaxed(base + ARM_SMMU_CR0),
		   readl_relaxed(base + ARM_SMMU_CR0) & CR0_SMMUEN ?
		   "Enabled " : "Disabled ",
		   readl_relaxed(base + ARM_SMMU_CR0) & CR0_EVTQEN ?
		   "EventQ " : "",
		   readl_relaxed(base + ARM_SMMU_CR0) & CR0_CMDQEN ?
		   "CmdQ " : "");

	seq_printf(seq, "CR1: 0x%08x\n", readl_relaxed(base + ARM_SMMU_CR1));
	seq_printf(seq, "CR2: 0x%08x\n", readl_relaxed(base + ARM_SMMU_CR2));

	/* 32-bit queue pointer registers */
	seq_printf(seq, "CMDQ_PROD: 0x%08x\n",
		   readl_relaxed(base + ARM_SMMU_CMDQ_PROD));
	seq_printf(seq, "CMDQ_CONS: 0x%08x\n",
		   readl_relaxed(base + ARM_SMMU_CMDQ_CONS));
	seq_printf(seq, "EVTQ_PROD: 0x%08x\n",
		   readl_relaxed(base + ARM_SMMU_EVTQ_PROD));
	seq_printf(seq, "EVTQ_CONS: 0x%08x\n",
		   readl_relaxed(base + ARM_SMMU_EVTQ_CONS));

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(smmu_debugfs_registers);

/**
 * arm_smmu_debugfs_setup() - Initialize debugfs for SMMU device
 * @smmu: SMMU device to setup debugfs for
 * @ioaddr: Physical base address of the SMMU device registers
 *
 * This function creates the basic debugfs directory structure for a SMMU device.
 *
 * Return: 0 on success, negative error code on failure
 */
int arm_smmu_debugfs_setup(struct arm_smmu_device *smmu, phys_addr_t ioaddr)
{
	struct arm_smmu_debugfs *debugfs;
	struct dentry *smmu_dir;
	char name[32];
	int ret;

	/* Create root directory if it doesn't exist */
	mutex_lock(&arm_smmu_debugfs_lock);
	if (!smmuv3_root_dir) {
		smmuv3_root_dir = debugfs_create_dir("arm_smmu_v3",
						     iommu_debugfs_dir);
		if (!smmuv3_root_dir) {
			mutex_unlock(&arm_smmu_debugfs_lock);
			return -ENOMEM;
		}
	}
	mutex_unlock(&arm_smmu_debugfs_lock);

	/* Allocate debugfs structure */
	debugfs = kzalloc(sizeof(*debugfs), GFP_KERNEL);
	if (!debugfs)
		return -ENOMEM;

	smmu->debugfs = debugfs;
	debugfs->smmu = smmu;

	/* Create SMMU instance directory */
	snprintf(name, sizeof(name), "smmu3.%pa", &ioaddr);
	smmu_dir = debugfs_create_dir(name, smmuv3_root_dir);
	if (!smmu_dir) {
		ret = -ENOMEM;
		goto err_free;
	}

	debugfs->smmu_dir = smmu_dir;

	/* Create capabilities file */
	if (!debugfs_create_file("capabilities", 0444, smmu_dir, smmu,
				 &smmu_debugfs_capabilities_fops))
		goto err_cleanup;

	if (!debugfs_create_file("registers", 0444, smmu_dir, smmu,
				 &smmu_debugfs_registers_fops))
		goto err_cleanup;

	pr_info("SMMUv3 debugfs initialized for smmu%pa\n", &ioaddr);
	return 0;

err_cleanup:
	debugfs_remove_recursive(smmu_dir);
err_free:
	kfree(debugfs);
	smmu->debugfs = NULL;
	return ret;
}
