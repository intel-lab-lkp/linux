/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2022, Google LLC.
 */

#ifndef SELFTEST_KVM_MERCURY_DEVICE_H
#define SELFTEST_KVM_MERCURY_DEVICE_H

#include "processor.h"
#include "test_util.h"

#define MERCURY_VENDOR_ID	0x1ae0
#define MERCURY_DEVICE_ID	0x0050

/* The base registers of the mercury device begin at the below offset from BAR0 */
#define MERCURY_BASE_OFFSET	(768 * 1024)

#define MERCURY_MSIX_VECTOR	0
#define MERCURY_MSIX_COUNT	1 /* Currently, only 1 vector is assigned to mercury */

#define MERCURY_DMA_MAX_BUF_SIZE_BYTES		SZ_8K
#define MERCURY_DMA_MEMCPY_MAX_BUF_SIZE_BYTES	SZ_1G

/* Mercury device accepts the DMA size as double-word (4-bytes) */
#define MERCURY_DMA_SIZE_STRIDE			4

#define MERCURY_ABI_VERSION	0

/* Register Offsets relative to MERCURY_BASE_OFFSET */
/* Unless otherwise specified, all the registers are 32-bits */
#define MERCURY_REG_VERSION		0x0	/* Read-only */
#define MERCURY_REG_COMMAND		0x04	/* Write-only */
#define MERCURY_REG_STATUS		0x08	/* Read-only, 64-bit register */
#define MERCURY_REG_DMA_SRC_ADDR	0x10	/* Read/Write, 64-bit register */
#define MERCURY_REG_DMA_DEST_ADDR	0x18	/* Read/Write, 64-bit register */
#define MERCURY_REG_DMA_DW_LEN		0x20	/* Read/Write */
#define MERCURY_REG_SCRATCH_REG0	0x24	/* Read/Write */
#define MERCURY_REG_SCRATCH_REG1	0x1000	/* Read/Write */

/* Bit positions of the STATUS register */
enum mercury_status_bit {
	MERCURY_STATUS_BIT_READY = 0,
	MERCURY_STATUS_BIT_DMA_FROM_DEV_COMPLETE = 1,
	MERCURY_STATUS_BIT_DMA_TO_DEV_COMPLETE = 2,
	MERCURY_STATUS_BIT_DMA_MEMCPY_COMPLETE = 3,
	MERCURY_STATUS_BIT_FORCE_INTERRUPT = 4,
	MERCURY_STATUS_BIT_INVAL_DMA_SIZE = 5,
	MERCURY_STATUS_BIT_DMA_ERROR = 6,
	MERCURY_STATUS_BIT_CMD_ERR_INVAL_CMD = 7,
	MERCURY_STATUS_BIT_CMD_ERR_DEV_NOT_READY = 8,
};

/* List of mercury commands that can be written into MERCURY_REG_COMMAND register */
enum mercury_command {
	MERCURY_COMMAND_RESET = 0,
	MERCURY_COMMAND_TRIGGER_DMA_FROM_DEV = 1,
	MERCURY_COMMAND_TRIGGER_DMA_TO_DEV = 2,
	MERCURY_COMMAND_TRIGGER_DMA_MEMCPY = 3,
	MERCURY_COMMAND_FORCE_INTERRUPT = 4,
};

static inline void mercury_write_reg64(void *bar0, uint32_t reg_off, uint64_t val)
{
	void *reg = bar0 + MERCURY_BASE_OFFSET + reg_off;

	writeq(val, reg);
}

static inline void mercury_write_reg32(void *bar0, uint32_t reg_off, uint32_t val)
{
	void *reg = bar0 + MERCURY_BASE_OFFSET + reg_off;

	writel(val, reg);
}

static inline uint32_t mercury_read_reg32(void *bar0, uint32_t reg_off)
{
	void *reg = bar0 + MERCURY_BASE_OFFSET + reg_off;

	return readl(reg);
}

static inline uint64_t mercury_read_reg64(void *bar0, uint32_t reg_off)
{
	void *reg = bar0 + MERCURY_BASE_OFFSET + reg_off;

	return readq(reg);
}

static inline uint64_t mercury_get_status(void *bar0)
{
	return mercury_read_reg64(bar0, MERCURY_REG_STATUS);
}

static inline void mercury_issue_command(void *bar0, enum mercury_command cmd)
{
	mercury_write_reg32(bar0, MERCURY_REG_COMMAND, cmd);
}

static inline void mercury_issue_reset(void *bar0)
{
	mercury_issue_command(bar0, MERCURY_COMMAND_RESET);
}

static inline void mercury_force_irq(void *bar0)
{
	mercury_issue_command(bar0, MERCURY_COMMAND_FORCE_INTERRUPT);
}

static inline void mercury_set_dma_size(void *bar0, size_t sz_bytes)
{
	/* Convert the DMA size from bytes to DWORDS, as accepted by the device */
	size_t sz_dwords = sz_bytes / MERCURY_DMA_SIZE_STRIDE;

	mercury_write_reg32(bar0, MERCURY_REG_DMA_DW_LEN, sz_dwords);
}

#endif /* SELFTEST_KVM_MERCURY_DEVICE_H */
