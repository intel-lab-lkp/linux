// SPDX-License-Identifier: GPL-2.0
/*
 * ARM SMMUv3 DebugFS Support
 *
 * Directory Structure:
 * /sys/kernel/debug/iommu/arm_smmu_v3/
 * └── smmu<ioaddr>/
 *     ├── capabilities    # SMMU feature capabilities and configuration
 *     ├── registers       # SMMU Key registers
 *     ├── stream_table
 *         ├── <sid>/                                # Stream ID
 *             ├── ste                               # Stream Table Entry
 *
 * The capabilities file provides detailed information about:
 * - translation stage support (Stage1/Stage2)
 * - System coherency, ATS, and PRI feature availability
 * - Stream table size and command/event queue depths
 *
 * The registers display provides crucial visibility into:
 * - CR0, CR1, CR2 control registers
 * - Command and Event queue pointers
 *
 * The STE Information Displayed:
 * - STE validity and configuration
 * - Stage 1 and Stage 2 context pointers
 * - Raw STE data
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
 * smmu_debugfs_registers_show() - Display SMMU register values
 * @seq: seq_file to write to
 * @unused: unused parameter
 *
 * Errors are reported via seq_puts, the function always returns 0
 */
static int smmu_debugfs_registers_show(struct seq_file *seq, void *unused)
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
	seq_printf(seq, "CR0: 0x%08x\n", readl_relaxed(base + ARM_SMMU_CR0));
	seq_printf(seq, "CR1: 0x%08x\n", readl_relaxed(base + ARM_SMMU_CR1));
	seq_printf(seq, "CR2: 0x%08x\n", readl_relaxed(base + ARM_SMMU_CR2));

	/* 32-bit queue pointer registers */
	seq_printf(seq, "CMDQ_PROD: 0x%08x\n",
		   readl_relaxed(base + ARM_SMMU_CMDQ_PROD));
	seq_printf(seq, "CMDQ_CONS: 0x%08x\n",
		   readl_relaxed(base + ARM_SMMU_CMDQ_CONS));
	seq_printf(seq, "EVTQ_PROD: 0x%08x\n",
		   smmu->page1 ? readl_relaxed(smmu->page1 + ARM_SMMU_EVTQ_PROD) : 0);
	seq_printf(seq, "EVTQ_CONS: 0x%08x\n",
		   smmu->page1 ? readl_relaxed(smmu->page1 + ARM_SMMU_EVTQ_CONS) : 0);

	return 0;
}

static int smmu_debugfs_registers_open(struct inode *inode, struct file *file)
{
	struct arm_smmu_device *smmu = inode->i_private;
	int ret;

	if (!smmu || !get_device(smmu->dev))
		return -ENODEV;

	ret = single_open(file, smmu_debugfs_registers_show, smmu);
	if (ret)
		put_device(smmu->dev);

	return ret;
}

static int smmu_debugfs_registers_release(struct inode *inode, struct file *file)
{
	struct seq_file *seq = file->private_data;
	struct arm_smmu_device *smmu = seq->private;

	single_release(inode, file);
	if (smmu)
		put_device(smmu->dev);

	return 0;
}

static const struct file_operations smmu_debugfs_registers_fops = {
	.owner   = THIS_MODULE,
	.open    = smmu_debugfs_registers_open,
	.read    = seq_read,
	.llseek  = seq_lseek,
	.release = smmu_debugfs_registers_release,
};

/**
 * smmu_debugfs_ste_show() - Dump STE details to seq_file
 * @seq: seq_file to write to
 * @unused: unused parameter
 *
 * Errors are reported via seq_puts, the function always returns 0
 */
static int smmu_debugfs_ste_show(struct seq_file *seq, void *unused)
{
	struct ste_context *ctx = seq->private;
	struct arm_smmu_master *master = ctx->master;
	struct arm_smmu_device *smmu;
	struct arm_smmu_ste *ste;
	u32 sid, cfg;
	int i;

	if (!master) {
		seq_puts(seq, "No SMMU master data\n");
		return 0;
	}

	smmu = master->smmu;
	guard(mutex)(&smmu->streams_mutex);

	sid = ctx->sid;

	if (!arm_smmu_sid_in_range(smmu, sid)) {
		seq_printf(seq, "Invalid Stream ID: %u (max %u)\n",
			   sid, (1 << smmu->sid_bits) - 1);
		return 0;
	}

	ste = arm_smmu_get_step_for_sid(smmu, sid);
	if (!ste) {
		seq_printf(seq, "STE not available for SID %u\n", sid);
		return 0;
	}

	seq_printf(seq, "STE for Stream ID %u\n", sid);
	seq_printf(seq, "  Valid: %s\n",
		   le64_to_cpu(ste->data[0]) & STRTAB_STE_0_V ? "Yes" : "No");

	seq_puts(seq, "  Config: ");

	cfg = FIELD_GET(STRTAB_STE_0_CFG, le64_to_cpu(ste->data[0]));

	switch (cfg) {
	case STRTAB_STE_0_CFG_BYPASS:
		seq_puts(seq, "BYPASS\n");
		break;
	case STRTAB_STE_0_CFG_S1_TRANS:
		seq_puts(seq, "only S1_TRANS\n");
		break;
	case STRTAB_STE_0_CFG_S2_TRANS:
		seq_puts(seq, "only S2_TRANS\n");
		break;
	case STRTAB_STE_0_CFG_NESTED:
		seq_puts(seq, "S1+S2_TRANS\n");
		break;
	case STRTAB_STE_0_CFG_ABORT:
		seq_puts(seq, "ABORT\n");
		break;
	default:
		seq_puts(seq, "UNKNOWN\n");
	}

	if (cfg == STRTAB_STE_0_CFG_S1_TRANS || cfg == STRTAB_STE_0_CFG_NESTED) {
		seq_printf(seq, "  S1ContextPtr: 0x%016llx\n",
			   le64_to_cpu(ste->data[0]) & STRTAB_STE_0_S1CTXPTR_MASK);
	}

	if (cfg == STRTAB_STE_0_CFG_S2_TRANS || cfg == STRTAB_STE_0_CFG_NESTED) {
		seq_printf(seq, "  S2TTB: 0x%016llx\n",
			   le64_to_cpu(ste->data[3]) & STRTAB_STE_3_S2TTB_MASK);
	}

	/* Display raw STE data */
	seq_puts(seq, "  Raw Data:\n");
	for (i = 0; i < STRTAB_STE_DWORDS; i++)
		seq_printf(seq, "    STE[%d]: 0x%016llx\n", i,
			   le64_to_cpu(ste->data[i]));

	return 0;
}

static int smmu_debugfs_ste_open(struct inode *inode, struct file *file)
{
	struct ste_context *ctx = inode->i_private;
	int ret;

	if (!ctx || !get_device(ctx->master->dev))
		return -ENODEV;

	ret = single_open(file, smmu_debugfs_ste_show, ctx);
	if (ret)
		put_device(ctx->master->dev);

	return ret;
}

static int smmu_debugfs_ste_release(struct inode *inode, struct file *file)
{
	struct seq_file *seq = file->private_data;
	struct ste_context *ctx = seq->private;

	single_release(inode, file);
	if (ctx)
		put_device(ctx->master->dev);
	return 0;
}

static const struct file_operations smmu_debugfs_ste_fops = {
	.owner   = THIS_MODULE,
	.open    = smmu_debugfs_ste_open,
	.read    = seq_read,
	.llseek  = seq_lseek,
	.release = smmu_debugfs_ste_release,
};

/**
 * arm_smmu_debugfs_create_stream_table() - Create debugfs entries for stream table
 * @smmu: SMMU device
 * @dev: device to create entries for
 *
 * Return: 0 on success, negative error code on failure
 */
int arm_smmu_debugfs_create_stream_table(struct arm_smmu_device *smmu,
					 struct device *dev)
{
	struct dentry *stream_dir, *dev_dir;
	struct arm_smmu_master *master;
	struct ste_context *ctx;
	char name[64];
	u32 sid;
	int i;

	if (!smmu->debugfs)
		return -ENODEV;

	scoped_guard(mutex, &arm_smmu_debugfs_lock) {
		if (!smmu->debugfs->stream_dir) {
			stream_dir = debugfs_create_dir("stream_table",
							smmu->debugfs->smmu_dir);
			if (IS_ERR(stream_dir))
				return PTR_ERR(stream_dir);

			smmu->debugfs->stream_dir = stream_dir;
		} else {
			stream_dir = smmu->debugfs->stream_dir;
		}
	}

	master = dev_iommu_priv_get(dev);
	if (!master || !master->num_streams)
		return -ENODEV;

	for (i = 0; i < master->num_streams; i++) {
		sid = master->streams[i].id;
		snprintf(name, sizeof(name), "%u", sid);
		dev_dir = debugfs_create_dir(name, stream_dir);
		if (IS_ERR(dev_dir))
			continue;

		/* Create STE file */
		ctx = kzalloc_obj(*ctx);
		if (!ctx)
			continue;

		ctx->master = master;
		ctx->sid = sid;
		spin_lock(&smmu->debugfs->stream_lock);
		list_add_tail(&ctx->node, &smmu->debugfs->stream_list);
		spin_unlock(&smmu->debugfs->stream_lock);
		debugfs_create_file("ste", 0444, dev_dir, ctx,
				    &smmu_debugfs_ste_fops);
	}

	return 0;
}

/**
 * arm_smmu_debugfs_remove_stream_table() - Remove debugfs entries for stream table
 * @smmu: SMMU device
 * @dev: device to remove entries for
 *
 * This function removes the debugfs directories created by
 * arm_smmu_debugfs_create_stream_table().
 */
void arm_smmu_debugfs_remove_stream_table(struct arm_smmu_device *smmu,
					  struct device *dev)
{
	struct dentry *stream_dir, *dev_dir;
	struct arm_smmu_master *master;
	struct ste_context *ctx, *tmp;
	char name[64];
	int i;

	/* Check if stream_table directory exists */
	if (!smmu->debugfs || !smmu->debugfs->stream_dir)
		return;

	stream_dir = smmu->debugfs->stream_dir;
	master = dev_iommu_priv_get(dev);
	if (!master)
		return;

	/* Remove directories for each stream ID */
	for (i = 0; i < master->num_streams; i++) {
		snprintf(name, sizeof(name), "%u", master->streams[i].id);
		dev_dir = debugfs_lookup(name, stream_dir);
		debugfs_remove_recursive(dev_dir);
		dput(dev_dir);
	}

	/* Free stream context */
	spin_lock(&smmu->debugfs->stream_lock);
	list_for_each_entry_safe(ctx, tmp, &smmu->debugfs->stream_list, node) {
		if (ctx->master->dev == dev) {
			list_del(&ctx->node);
			kfree(ctx);
		}
	}
	spin_unlock(&smmu->debugfs->stream_lock);
}

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
	INIT_LIST_HEAD(&debugfs->stream_list);
	spin_lock_init(&debugfs->stream_lock);
	smmu->debugfs = debugfs;

	/* Create capabilities file */
	debugfs_create_file("capabilities", 0444, smmu_dir, smmu,
			    &smmu_debugfs_capabilities_fops);

	debugfs_create_file("registers", 0444, smmu_dir, smmu,
			    &smmu_debugfs_registers_fops);

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
