/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * RAS2 ACPI driver header file
 *
 * (C) Copyright 2014, 2015 Hewlett-Packard Enterprises
 *
 * Copyright (c) 2024 HiSilicon Limited
 */

#ifndef _RAS2_ACPI_H
#define _RAS2_ACPI_H

#include <linux/acpi.h>
#include <linux/mailbox_client.h>
#include <linux/mutex.h>
#include <linux/types.h>

#define RAS2_PCC_CMD_COMPLETE	BIT(0)
#define RAS2_PCC_CMD_ERROR	BIT(2)

/* RAS2 specific PCC commands */
#define RAS2_PCC_CMD_EXEC 0x01

struct device;

/* Data structures for PCC communication and RAS2 table */
struct pcc_mbox_chan;

struct ras2_pcc_subspace {
	int pcc_subspace_id;
	struct mbox_client mbox_client;
	struct pcc_mbox_chan *pcc_chan;
	struct acpi_ras2_shared_memory __iomem *pcc_comm_addr;
	u64 comm_base_addr;
	bool pcc_channel_acquired;
	ktime_t deadline;
	unsigned int pcc_mpar;
	unsigned int pcc_mrtt;
	struct list_head elem;
	u16 ref_count;
};

struct ras2_scrub_ctx {
	struct device *dev;
	struct ras2_pcc_subspace *pcc_subspace;
	int id;
	struct device *scrub_dev;
	bool bg;
	u64 base, size;
	u8 scrub_cycle_hrs, min_scrub_cycle, max_scrub_cycle;
	/* Lock to provide mutually exclusive access to PCC channel */
	struct mutex lock;
};

int ras2_send_pcc_cmd(struct ras2_scrub_ctx *ras2_ctx, u16 cmd);
int devm_ras2_register_pcc_channel(struct device *dev, struct ras2_scrub_ctx *ras2_ctx,
				   int pcc_subspace_id);

#endif /* _RAS2_ACPI_H */
