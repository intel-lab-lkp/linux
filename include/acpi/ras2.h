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

struct device;

/*
 * ACPI spec 6.5 Table 5.82: PCC command codes used by
 * RAS2 platform communication channel.
 */
#define PCC_CMD_EXEC_RAS2 0x01

#define RAS2_AUX_DEV_NAME "ras2"
#define RAS2_MEM_DEV_ID_NAME "acpi_ras2_mem"

/**
 * struct ras2_mem_ctx - Context for RAS2 memory features
 * @id:			Unique identifier for the RAS2 feature
 * @adev:		Auxiliary device object
 * @comm_addr:		Pointer to RAS2 PCC shared memory region
 * @dev:		Pointer to device backing struct mbox_controller for PCC
 * @pcc_subspace:	Pointer to local data structure for PCC communication
 * @pcc_lock:		Pointer to PCC lock to provide mutually exclusive access
 *			to PCC channel subspace
 */
struct ras2_mem_ctx {
	int				id;
	struct auxiliary_device		adev;
	struct acpi_ras2_shmem __iomem	*comm_addr;
	struct device			*dev;
	void				*pcc_subspace;
	struct mutex			*pcc_lock;
};

#ifdef CONFIG_ACPI_RAS2
void __init acpi_ras2_init(void);
int ras2_send_pcc_cmd(struct ras2_mem_ctx *ras2_ctx, u16 cmd);
#else
static inline void acpi_ras2_init(void) { }
#endif

#endif /* _ACPI_RAS2_H */
