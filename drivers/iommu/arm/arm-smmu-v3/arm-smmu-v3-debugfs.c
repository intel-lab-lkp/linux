// SPDX-License-Identifier: GPL-2.0
/*
 * ARM SMMUv3 DebugFS Support
 *
 * Directory Structure:
 * /sys/kernel/debug/iommu/arm_smmu_v3/
 * └── smmu<ioaddr>/
 *     ├── capabilities    # SMMU feature capabilities and configuration
 *
 * The capabilities file provides detailed information about:
 * - translation stage support (Stage1/Stage2)
 * - System coherency, ATS, and PRI feature availability
 * - Stream table size and command/event queue depths
 *
 * Copyright (C) 2026 HiSilicon Limited.
 * Author: Qinxin Xia <xiaqinxin@huawei.com>
 */

#include <linux/cleanup.h>
#include <linux/debugfs.h>
#include <linux/slab.h>
#include "arm-smmu-v3.h"

static struct dentry *smmu_debugfs_root;
static DEFINE_MUTEX(arm_smmu_debugfs_lock);

/**
 * smmu_debugfs_capabilities_show() - Display SMMU capabilities
 * @seq: seq_file to write to
 * @unused: unused parameter
 *
 * Errors are reported via seq_puts, the function always returns 0
 */
static int smmu_debugfs_capabilities_show(struct seq_file *seq, void *unused)
{
	struct arm_smmu_device *smmu = seq->private;

	if (!smmu) {
		seq_puts(seq, "SMMU not available\n");
		return 0;
	}

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
	seq_printf(seq, "  Stream Table Size: %llu\n", 1ULL << smmu->sid_bits);
	seq_printf(seq, "  Command Queue Depth: %d\n",
		   1 << smmu->cmdq.q.llq.max_n_shift);
	seq_printf(seq, "  Event Queue Depth: %d\n",
		   1 << smmu->evtq.q.llq.max_n_shift);

	return 0;
}

static int smmu_debugfs_capabilities_open(struct inode *inode, struct file *file)
{
	struct arm_smmu_device *smmu = inode->i_private;
	int ret;

	if (!smmu || !get_device(smmu->dev))
		return -ENODEV;

	ret = single_open(file, smmu_debugfs_capabilities_show, smmu);
	if (ret)
		put_device(smmu->dev);

	return ret;
}

static int smmu_debugfs_capabilities_release(struct inode *inode, struct file *file)
{
	struct seq_file *seq = file->private_data;
	struct arm_smmu_device *smmu = seq->private;

	single_release(inode, file);
	if (smmu)
		put_device(smmu->dev);

	return 0;
}

static const struct file_operations smmu_debugfs_capabilities_fops = {
	.owner   = THIS_MODULE,
	.open    = smmu_debugfs_capabilities_open,
	.read    = seq_read,
	.llseek  = seq_lseek,
	.release = smmu_debugfs_capabilities_release,
};

/**
 * arm_smmu_debugfs_setup() - Initialize debugfs for SMMU device
 * @smmu: SMMU device to setup debugfs for
 * @name: SMMU device name
 *
 * This function creates the basic debugfs directory structure for an SMMU device.
 *
 * Return: 0 on success, negative error code on failure
 */
int arm_smmu_debugfs_setup(struct arm_smmu_device *smmu, const char *name)
{
	struct arm_smmu_debugfs *debugfs;
	struct dentry *smmu_dir;

	/* Create root directory if it doesn't exist */
	scoped_guard(mutex, &arm_smmu_debugfs_lock) {
		if (!smmu_debugfs_root) {
			/* Once created, it will not be removed */
			smmu_debugfs_root = debugfs_create_dir("arm_smmu_v3",
							       iommu_debugfs_dir);
			if (IS_ERR(smmu_debugfs_root)) {
				smmu_debugfs_root = NULL;
				return -ENOMEM;
			}
		}
	}

	/* Allocate debugfs structure */
	debugfs = kzalloc_obj(*debugfs);
	if (!debugfs)
		return -ENOMEM;

	/* Create SMMU instance directory */
	smmu_dir = debugfs_create_dir(name, smmu_debugfs_root);
	if (IS_ERR(smmu_dir)) {
		kfree(debugfs);
		smmu->debugfs = NULL;
		return PTR_ERR(smmu_dir);
	}

	debugfs->smmu_dir = smmu_dir;
	smmu->debugfs = debugfs;

	/* Create capabilities file */
	debugfs_create_file("capabilities", 0444, smmu_dir, smmu,
			    &smmu_debugfs_capabilities_fops);

	dev_dbg(smmu->dev, "debugfs initialized for %s\n", name);
	return 0;
}

/**
 * arm_smmu_debugfs_remove() - Clean up debugfs entries for an SMMU device
 * @smmu: SMMU device
 *
 * This function removes the debugfs directories created by setup.
 */
void arm_smmu_debugfs_remove(struct arm_smmu_device *smmu)
{
	struct arm_smmu_debugfs *debugfs;
	struct dentry *smmu_dir;

	scoped_guard(mutex, &arm_smmu_debugfs_lock) {
		debugfs = smmu->debugfs;
		if (!debugfs)
			return;

		smmu_dir = debugfs->smmu_dir;
		kfree(debugfs);
		smmu->debugfs = NULL;
	}

	/* Remove outside lock to avoid blocking on active VFS operations */
	debugfs_remove_recursive(smmu_dir);
}
