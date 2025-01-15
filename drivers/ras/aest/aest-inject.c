// SPDX-License-Identifier: GPL-2.0
/*
 * ARM Error Source Table Support
 *
 * Copyright (c) 2024, Alibaba Group.
 */

#include "aest.h"

static struct ras_ext_regs regs_inj;
static u64 hard_inject_val;

struct inj_attr {
	struct attribute attr;
	ssize_t (*show)(struct aest_node *n, struct inj_attr *a, char *b);
	ssize_t (*store)(struct aest_node *n, struct inj_attr *a, const char *b,
				size_t c);
};

struct aest_inject {
	struct aest_node *node;
	struct kobject kobj;
};

#define to_inj(k)	container_of(k, struct aest_inject, kobj)
#define to_inj_attr(a)	container_of(a, struct inj_attr, attr)

static u64 aest_sysreg_read_inject(void *__unused, u32 offset)
{
	u64 *p = (u64 *)&regs_inj;

	return p[offset/8];
}

static void aest_sysreg_write_inject(void *base, u32 offset, u64 val)
{
	u64 *p = (u64 *)&regs_inj;

	p[offset/8] = val;
}

static u64 aest_iomem_read_inject(void *base, u32 offset)
{
	u64 *p = (u64 *)&regs_inj;

	return p[offset/8];
}

static void aest_iomem_write_inject(void *base, u32 offset, u64 val)
{
	u64 *p = (u64 *)&regs_inj;

	p[offset/8] = val;
}

static struct aest_access aest_access_inject[] = {
	[ACPI_AEST_NODE_SYSTEM_REGISTER] = {
		.read = aest_sysreg_read_inject,
		.write = aest_sysreg_write_inject,
	},

	[ACPI_AEST_NODE_MEMORY_MAPPED] = {
		.read = aest_iomem_read_inject,
		.write = aest_iomem_write_inject,
	},
	[ACPI_AEST_NODE_SINGLE_RECORD_MEMORY_MAPPED] = {
		.read = aest_iomem_read_inject,
		.write = aest_iomem_write_inject,
	},
	{ }
};

static int inject_store(void *data, u64 val)
{
	int i = val, count = 0;
	struct aest_record record_inj, *record;
	struct aest_node node_inj, *node = data;

	if (i > (int)node->info->interface_hdr->error_record_count)
		return -EINVAL;

	memcpy(&node_inj, node, sizeof(*node));
	node_inj.name = "AEST-injection";

	record_inj.access = &aest_access_inject[node->info->interface_hdr->type];
	record_inj.node = &node_inj;
	record_inj.index = i;
	if (i >= 0) {
		record = &node->records[i];
		regs_inj.err_fr = record_read(record, ERXFR);
		regs_inj.err_ctlr = record_read(record, ERXCTLR);
		regs_inj.err_status = record_read(record, ERXSTATUS);
		regs_inj.err_addr = record_read(record, ERXADDR);
		regs_inj.err_misc[0] = record_read(record, ERXMISC0);
		regs_inj.err_misc[1] = record_read(record, ERXMISC1);
		regs_inj.err_misc[2] = record_read(record, ERXMISC2);
		regs_inj.err_misc[3] = record_read(record, ERXMISC3);
	}

	regs_inj.err_status |= ERR_STATUS_V;

	aest_proc_record(&record_inj, &count);

	if (count != 1)
		return -EIO;

	return 0;
}
DEFINE_DEBUGFS_ATTRIBUTE(inject_ops, NULL, inject_store, "%llu\n");

static int hard_inject_store(void *data, u64 val)
{
	struct aest_node *node = data;

	if (!node->inj)
		return -EPERM;

	if (val > node->record_count)
		return -ENODEV;

	if (node->type == ACPI_AEST_PROCESSOR_ERROR_NODE) {
		aest_select_record(node, val);
		write_sysreg_s(hard_inject_val, SYS_ERXPFGCTL_EL1);
		write_sysreg_s(0x100, SYS_ERXPFGCDN_EL1);
		aest_sync(node);
	} else
		writeq_relaxed(hard_inject_val, node->inj + val * 8);

	return 0;
}
DEFINE_DEBUGFS_ATTRIBUTE(hard_inject_ops, NULL, hard_inject_store, "%llu\n");

void aest_inject_init_debugfs(struct aest_node *node)
{
	struct dentry *inj;

	inj = debugfs_create_dir("inject", node->debugfs);

	debugfs_create_u64("err_fr", 0400, inj, &regs_inj.err_fr);
	debugfs_create_u64("err_ctrl", 0400, inj, &regs_inj.err_ctlr);
	debugfs_create_u64("err_status", 0400, inj, &regs_inj.err_status);
	debugfs_create_u64("err_addr", 0400, inj, &regs_inj.err_addr);
	debugfs_create_u64("err_misc0", 0400, inj, &regs_inj.err_misc[0]);
	debugfs_create_u64("err_misc1", 0400, inj, &regs_inj.err_misc[1]);
	debugfs_create_u64("err_misc2", 0400, inj, &regs_inj.err_misc[2]);
	debugfs_create_u64("err_misc3", 0400, inj, &regs_inj.err_misc[3]);
	debugfs_create_file("inject", 0400, inj, node, &inject_ops);

	debugfs_create_file("hard_inject", 0600, inj, node, &hard_inject_ops);
	debugfs_create_u64("hard_inject_val", 0600, inj, &hard_inject_val);
}
