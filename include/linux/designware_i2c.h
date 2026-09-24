/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Synopsys DesignWare I2C register definitions
 *
 * Copyright (C) 2026, Intel Corporation
 */

#ifndef __LINUX_DESIGNWARE_I2C_H
#define __LINUX_DESIGNWARE_I2C_H

#include <linux/bits.h>

/*
 * Logical register IDs. Each ID is resolved to a physical offset via a
 * per-variant table (see dw_i2c_reg_offsets[] below); regmap backends
 * (dw_reg_read()/dw_reg_write() and the Xe I2C_REG() macro) perform that
 * lookup so callers never need to know the real offset.
 */
enum dw_ic_reg_id {
	DW_IC_CON,
	DW_IC_TAR,
	DW_IC_SAR,
	DW_IC_DATA_CMD,
	DW_IC_SS_SCL_HCNT,
	DW_IC_SS_SCL_LCNT,
	DW_IC_FS_SCL_HCNT,
	DW_IC_FS_SCL_LCNT,
	DW_IC_HS_SCL_HCNT,
	DW_IC_HS_SCL_LCNT,
	DW_IC_INTR_STAT,
	DW_IC_INTR_MASK,
	DW_IC_RAW_INTR_STAT,
	DW_IC_RX_TL,
	DW_IC_TX_TL,
	DW_IC_CLR_INTR,
	DW_IC_CLR_RX_UNDER,
	DW_IC_CLR_RX_OVER,
	DW_IC_CLR_TX_OVER,
	DW_IC_CLR_RD_REQ,
	DW_IC_CLR_TX_ABRT,
	DW_IC_CLR_RX_DONE,
	DW_IC_CLR_ACTIVITY,
	DW_IC_CLR_STOP_DET,
	DW_IC_CLR_START_DET,
	DW_IC_CLR_GEN_CALL,
	DW_IC_ENABLE,
	DW_IC_STATUS,
	DW_IC_TXFLR,
	DW_IC_RXFLR,
	DW_IC_SDA_HOLD,
	DW_IC_TX_ABRT_SOURCE,
	DW_IC_ENABLE_STATUS,
	DW_IC_CLR_RESTART_DET,
	DW_IC_SMBUS_INTR_STAT,
	DW_IC_SMBUS_INTR_MASK,
	DW_IC_CLR_SMBUS_INTR,
	DW_IC_COMP_PARAM_1,
	DW_IC_COMP_VERSION,
	DW_IC_COMP_TYPE,
	AMD_UCSI_INTR_REG,
	DW_IC_REG_MAX
};

/*
 * Default "snps,designware-i2c" register offsets, indexed by
 * enum dw_ic_reg_id.
 */
static const __maybe_unused u32 dw_i2c_reg_offsets[DW_IC_REG_MAX] = {
	[DW_IC_CON]			= 0x00,
	[DW_IC_TAR]			= 0x04,
	[DW_IC_SAR]			= 0x08,
	[DW_IC_DATA_CMD]		= 0x10,
	[DW_IC_SS_SCL_HCNT]		= 0x14,
	[DW_IC_SS_SCL_LCNT]		= 0x18,
	[DW_IC_FS_SCL_HCNT]		= 0x1c,
	[DW_IC_FS_SCL_LCNT]		= 0x20,
	[DW_IC_HS_SCL_HCNT]		= 0x24,
	[DW_IC_HS_SCL_LCNT]		= 0x28,
	[DW_IC_INTR_STAT]		= 0x2c,
	[DW_IC_INTR_MASK]		= 0x30,
	[DW_IC_RAW_INTR_STAT]		= 0x34,
	[DW_IC_RX_TL]			= 0x38,
	[DW_IC_TX_TL]			= 0x3c,
	[DW_IC_CLR_INTR]		= 0x40,
	[DW_IC_CLR_RX_UNDER]		= 0x44,
	[DW_IC_CLR_RX_OVER]		= 0x48,
	[DW_IC_CLR_TX_OVER]		= 0x4c,
	[DW_IC_CLR_RD_REQ]		= 0x50,
	[DW_IC_CLR_TX_ABRT]		= 0x54,
	[DW_IC_CLR_RX_DONE]		= 0x58,
	[DW_IC_CLR_ACTIVITY]		= 0x5c,
	[DW_IC_CLR_STOP_DET]		= 0x60,
	[DW_IC_CLR_START_DET]		= 0x64,
	[DW_IC_CLR_GEN_CALL]		= 0x68,
	[DW_IC_ENABLE]			= 0x6c,
	[DW_IC_STATUS]			= 0x70,
	[DW_IC_TXFLR]			= 0x74,
	[DW_IC_RXFLR]			= 0x78,
	[DW_IC_SDA_HOLD]		= 0x7c,
	[DW_IC_TX_ABRT_SOURCE]		= 0x80,
	[DW_IC_ENABLE_STATUS]		= 0x9c,
	[DW_IC_CLR_RESTART_DET]		= 0xa8,
	[DW_IC_SMBUS_INTR_STAT]		= 0xc8,
	[DW_IC_SMBUS_INTR_MASK]		= 0xcc,
	[DW_IC_CLR_SMBUS_INTR]		= 0xd4,
	[DW_IC_COMP_PARAM_1]		= 0xf4,
	[DW_IC_COMP_VERSION]		= 0xf8,
	[DW_IC_COMP_TYPE]		= 0xfc,
	[AMD_UCSI_INTR_REG]		= 0x474,
};

/* DW_IC_CON bits */
#define DW_IC_CON_MASTER			BIT(0)
#define DW_IC_CON_SPEED_STD			(1 << 1)
#define DW_IC_CON_SPEED_FAST			(2 << 1)
#define DW_IC_CON_SPEED_HIGH			(3 << 1)
#define DW_IC_CON_SPEED_MASK			GENMASK(2, 1)
#define DW_IC_CON_10BITADDR_SLAVE		BIT(3)
#define DW_IC_CON_10BITADDR_MASTER		BIT(4)
#define DW_IC_CON_RESTART_EN			BIT(5)
#define DW_IC_CON_SLAVE_DISABLE			BIT(6)
#define DW_IC_CON_STOP_DET_IFADDRESSED		BIT(7)
#define DW_IC_CON_TX_EMPTY_CTRL			BIT(8)
#define DW_IC_CON_RX_FIFO_FULL_HLD_CTRL		BIT(9)
#define DW_IC_CON_BUS_CLEAR_CTRL		BIT(11)

/* DW_IC_DATA_CMD bits */
#define DW_IC_DATA_CMD_DAT			GENMASK(7, 0)
#define DW_IC_DATA_CMD_FIRST_DATA_BYTE		BIT(11)

/* DW_IC_INTR_* bits */
#define DW_IC_INTR_RX_UNDER			BIT(0)
#define DW_IC_INTR_RX_OVER			BIT(1)
#define DW_IC_INTR_RX_FULL			BIT(2)
#define DW_IC_INTR_TX_OVER			BIT(3)
#define DW_IC_INTR_TX_EMPTY			BIT(4)
#define DW_IC_INTR_RD_REQ			BIT(5)
#define DW_IC_INTR_TX_ABRT			BIT(6)
#define DW_IC_INTR_RX_DONE			BIT(7)
#define DW_IC_INTR_ACTIVITY			BIT(8)
#define DW_IC_INTR_STOP_DET			BIT(9)
#define DW_IC_INTR_START_DET			BIT(10)
#define DW_IC_INTR_GEN_CALL			BIT(11)
#define DW_IC_INTR_RESTART_DET			BIT(12)
#define DW_IC_INTR_MST_ON_HOLD			BIT(13)

/* DW_IC_ENABLE bits */
#define DW_IC_ENABLE_ENABLE			BIT(0)
#define DW_IC_ENABLE_ABORT			BIT(1)

/* DW_IC_STATUS bits */
#define DW_IC_STATUS_ACTIVITY			BIT(0)
#define DW_IC_STATUS_TFE			BIT(2)
#define DW_IC_STATUS_RFNE			BIT(3)
#define DW_IC_STATUS_MASTER_ACTIVITY		BIT(5)
#define DW_IC_STATUS_SLAVE_ACTIVITY		BIT(6)
#define DW_IC_STATUS_MASTER_HOLD_TX_FIFO_EMPTY	BIT(7)

/* DW_IC_SMBUS_INTR_* bits */
#define DW_IC_SMBUS_INTR_ALERT			BIT(10)

#endif /* __LINUX_DESIGNWARE_I2C_H */
