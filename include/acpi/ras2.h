/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * ACPI RAS2 driver header file
 *
 * Copyright (c) 2024-2025 HiSilicon Limited
 */

#ifndef _ACPI_RAS2_H
#define _ACPI_RAS2_H

#include <linux/acpi.h>
#include <linux/auxiliary_bus.h>
#include <linux/mailbox_client.h>
#include <linux/mutex.h>
#include <linux/types.h>

#define RAS2_PCC_CMD_COMPLETE	BIT(0)
#define RAS2_PCC_CMD_ERROR	BIT(2)

/* RAS2 specific PCC commands */
#define RAS2_PCC_CMD_EXEC 0x01

#define RAS2_AUX_DEV_NAME "ras2"
#define RAS2_MEM_DEV_ID_NAME "acpi_ras2_mem"

/* Data structure RAS2 table */
struct ras2_mem_ctx {
	struct auxiliary_device adev;
	/* Lock to provide mutually exclusive access to PCC channel */
	struct mutex lock;
	struct device *dev;
	struct acpi_ras2_shmem __iomem *comm_addr;
	void *pcc_subspace;
	u64 base, size;
	int id;
	u8 instance;
	u8 scrub_cycle_hrs;
	u8 min_scrub_cycle;
	u8 max_scrub_cycle;
	bool bg_scrub;
};

#ifdef CONFIG_ACPI_RAS2
void __init acpi_ras2_init(void);
int ras2_send_pcc_cmd(struct ras2_mem_ctx *ras2_ctx, u16 cmd);
#else
static inline void acpi_ras2_init(void) { }

static inline int ras2_send_pcc_cmd(struct ras2_mem_ctx *ras2_ctx, u16 cmd)
{
	return -EOPNOTSUPP;
}
#endif
#endif /* _ACPI_RAS2_H */
