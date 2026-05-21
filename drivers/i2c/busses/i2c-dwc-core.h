/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Synopsys DWC I2C adapter driver.
 *
 * Based on the TI DAVINCI I2C adapter driver.
 *
 * Copyright (C) 2006 Texas Instruments.
 * Copyright (C) 2007 MontaVista Software Inc.
 * Copyright (C) 2009 Provigent Ltd.
 */

#include <linux/bits.h>
#include <linux/compiler_types.h>
#include <linux/completion.h>
#include <linux/dev_printk.h>
#include <linux/errno.h>
#include <linux/i2c.h>
#include <linux/regmap.h>
#include <linux/types.h>

#define IC_DFLT_OPERATION_REG_OFFSET		0x00
#define IC_DFLT_I2C_REG_OFFSET			0x20
#define IC_DFLT_SMBUS_REG_OFFSET		0xFC

#define DW_IC_DEFAULT_FUNCTIONALITY		(I2C_FUNC_I2C | \
						 I2C_FUNC_SMBUS_BYTE | \
						 I2C_FUNC_SMBUS_BYTE_DATA | \
						 I2C_FUNC_SMBUS_WORD_DATA | \
						 I2C_FUNC_SMBUS_BLOCK_DATA | \
						 I2C_FUNC_SMBUS_I2C_BLOCK)

#define DWC_IC_CTRL_OP_MODE			BIT(0)
#define DWC_IC_CTRL_SPEED_STD			BIT(4)
#define DWC_IC_CTRL_SPEED_FAST			(2 << 4)
#define DWC_IC_CTRL_SPEED_HIGH			(3 << 4)
#define DWC_IC_CTRL_SPEED_MASK			GENMASK(5, 4)
#define DWC_IC_CTRL_10BITADDR_TGT		BIT(8)
#define DWC_IC_CTRL_10BITADDR_CTRLR		BIT(9)
#define DWC_IC_CTRL_STOP_DET_IFADDRESSED	BIT(10)
#define DWC_IC_CTRL_TX_EMPTY_CTRL		BIT(11)
#define DWC_IC_CTRL_RX_FIFO_FULL_HLD_CTRL	BIT(12)
#define DWC_IC_CTRL_BUS_CLEAR_CTRL		BIT(14)

#define DWC_IC_DATA_CMD_DAT			GENMASK(7, 0)
#define DWC_IC_DATA_CMD_FIRST_DATA_BYTE		BIT(11)
#define DWC_IC_DATA_CMD_STOP			BIT(9)
#define DWC_IC_DATA_CMD_CMD			BIT(8)

/*
 * Registers offset
 */
#undef DW_IC_ENABLE
#define DW_IC_ENABLE				(IC_DFLT_OPERATION_REG_OFFSET + 0x04)
#define DWC_IC_CAPABILITIES			(IC_DFLT_OPERATION_REG_OFFSET + 0x0c)
#define DWC_IC_SMBUS_CAPABILITIES		(IC_DFLT_OPERATION_REG_OFFSET + 0x18)

#define DWC_IC_CTRL				(IC_DFLT_I2C_REG_OFFSET + 0x04)
#undef DW_IC_TAR
#define DW_IC_TAR				(IC_DFLT_I2C_REG_OFFSET + 0x08)
#define DWC_IC_DAR				(IC_DFLT_I2C_REG_OFFSET + 0x0C)
#define DWC_IC_SCL_HCNT				(IC_DFLT_I2C_REG_OFFSET + 0x24)
#define DWC_IC_SCL_LCNT				(IC_DFLT_I2C_REG_OFFSET + 0x28)
#define DWC_IC_HS_SCL_HCNT			(IC_DFLT_I2C_REG_OFFSET + 0x2c)
#define DWC_IC_HS_SCL_LCNT			(IC_DFLT_I2C_REG_OFFSET + 0x30)
#undef DW_IC_SDA_HOLD
#define DW_IC_SDA_HOLD				(IC_DFLT_I2C_REG_OFFSET + 0x34)
#define DWC_IC_SPKLEN				(IC_DFLT_I2C_REG_OFFSET + 0x3c)
#define DWC_IC_HS_SPKLEN			(IC_DFLT_I2C_REG_OFFSET + 0x40)
#define DWC_IC_SCL_STUCK_AT_LOW_TIMEOUT		(IC_DFLT_I2C_REG_OFFSET + 0x44)
#define DWC_IC_SCL_STUCK_AT_LOW_TIMEOUT_MAX	(IC_DFLT_I2C_REG_OFFSET + 0x48)
#define DWC_IC_SDA_STUCK_AT_LOW_TIMEOUT		(IC_DFLT_I2C_REG_OFFSET + 0x4c)
#undef DW_IC_DATA_CMD
#define DW_IC_DATA_CMD				(IC_DFLT_I2C_REG_OFFSET + 0x58)
#define DWC_IC_RX_TL				(IC_DFLT_I2C_REG_OFFSET + 0x5c)
#define DWC_IC_TX_TL				(IC_DFLT_I2C_REG_OFFSET + 0x60)
#undef DW_IC_INTR_STAT
#define DW_IC_INTR_STAT				(IC_DFLT_I2C_REG_OFFSET + 0x74)
#undef DW_IC_INTR_MASK
#define DW_IC_INTR_MASK				(IC_DFLT_I2C_REG_OFFSET + 0x78)
#undef DW_IC_RAW_INTR_STAT
#define DW_IC_RAW_INTR_STAT			(IC_DFLT_I2C_REG_OFFSET + 0x7c)
#define DWC_IC_INTR_CLR				(IC_DFLT_I2C_REG_OFFSET + 0x80)
#define DWC_CLR_INTR				BIT(0)
#define DWC_IC_CLR_RX_UNDER			BIT(1)
#define DWC_IC_CLR_RX_OVER			BIT(2)
#define DWC_IC_CLR_TX_OVER			BIT(3)
#define DWC_IC_CLR_RD_REQ			BIT(4)
#define DWC_IC_CLR_TX_ABRT			BIT(5)
#define DWC_IC_CLR_RX_DONE			BIT(6)
#define DWC_IC_CLR_ACTIVITY			BIT(7)
#define DWC_IC_CLR_STOP_DET			BIT(8)
#define DWC_IC_CLR_START_DET			BIT(9)
#define DWC_IC_CLR_GEN_CALL			BIT(10)
#define DWC_IC_CLR_RESTART_DET			BIT(11)
#define DWC_IC_CLR_SCL_STUCK_DET		BIT(12)
#undef DW_IC_ENABLE_STATUS
#define DW_IC_ENABLE_STATUS			(IC_DFLT_I2C_REG_OFFSET + 0x84)
#define DWC_IC_TX_TRMNT_SOURCE			(IC_DFLT_I2C_REG_OFFSET + 0x88)
#undef DW_IC_STATUS
#define DW_IC_STATUS				(IC_DFLT_I2C_REG_OFFSET + 0x8c)
#undef DW_IC_TXFLR
#define DW_IC_TXFLR				(IC_DFLT_I2C_REG_OFFSET + 0x90)
#undef DW_IC_RXFLR
#define DW_IC_RXFLR				(IC_DFLT_I2C_REG_OFFSET + 0x94)
#define DW_IC_COMP_PARAM_1			0xf4
#undef DW_IC_COMP_VERSION
#define DW_IC_COMP_VERSION			(IC_DFLT_I2C_REG_OFFSET + 0xa4)
#define DWC_IC_SDA_HOLD_MIN_VERS		0x3131312A /* "111*" == v1.11* */

#undef DW_IC_COMP_TYPE
#define DW_IC_COMP_TYPE				(IC_DFLT_I2C_REG_OFFSET + 0xa8)
#define DWC_IC_INTR_SCL_STUCK_AT_LOW		BIT(14)

#define DWC_IC_STATUS_ACTIVITY			BIT(0)
#define DWC_IC_STATUS_TFE			BIT(2)
#define DWC_IC_STATUS_RFNE			BIT(3)
#define DWC_IC_STATUS_CTRLR_ACTIVITY		BIT(5)
#define DWC_IC_STATUS_TGT_ACTIVITY		BIT(6)

#define DW_IC_SDA_HOLD_TX_SHIFT			0
#define DW_IC_SDA_HOLD_TX_MASK			GENMASK(15, 0)

#define DW_IC_ERR_TX_ABRT			0x1

#define DWC_IC_TAR_SMBUS_QUICK_CMD		BIT(16)
#define DWC_IC_TAR_SPECIAL			BIT(11)

#define DWC_IC_COMP_PARAM_1_SPEED_MODE_HIGH	(BIT(4) | BIT(5))
#define DWC_IC_COMP_PARAM_1_SPEED_MODE_MASK	GENMASK(5, 4)

#define DWC_IC_SMBUS_ARP_CTRL			(IC_DFLT_SMBUS_REG_OFFSET + 0x8)
#define DWC_IC_SMBUS_INTR_STAT			(IC_DFLT_SMBUS_REG_OFFSET + 0x28)
#define DWC_IC_SMBUS_INTR_CLR			(IC_DFLT_SMBUS_REG_OFFSET + 0x34)
#define DWC_CLR_SMBUS_ALERT_DET			BIT(10)
#define DWC_R_SMBUS_ALERT_DET			BIT(10)

#define DWC_IC_DAR4_EN				19
#define DWC_IC_DAR3_EN				18
#define DWC_IC_DAR2_EN				17
#define DWC_IC_DAR_EN				16

#define DWC_IC_ENABLE_DAR_EN			BIT(DWC_IC_DAR_EN)
#define DWC_IC_ENABLE_DAR2_EN			BIT(DWC_IC_DAR2_EN)
#define DWC_IC_ENABLE_DAR3_EN			BIT(DWC_IC_DAR3_EN)
#define DWC_IC_ENABLE_DAR4_EN			BIT(DWC_IC_DAR4_EN)

#define DW_IC_ENABLE_TX_CMD_BLOCK		BIT(2)

#define DWC_IC_SMBUS				10

#define DWC_IC_CAPABILITIES_IC_SMBUS		BIT(DWC_IC_SMBUS)

#define DWC_IC_SMBUS_ARP			2

#define DWC_IC_SMBUS_CAPABILITIES_SMBUS_ARP	BIT(DWC_IC_SMBUS_ARP)

#define DWC_IC_SMBUS_ARP_CTRL_NARP_DEVICE_TYPE	0

#undef TXGBE_RX_FIFO_DEPTH
#define TXGBE_RX_FIFO_DEPTH			0

extern const struct i2c_algorithm i2c_dw_slave_algo;
int i2c_dw_probe_slave(struct dw_i2c_dev *dev);
int i2c_dw_probe_master(struct dw_i2c_dev *dev);
void i2c_dw_read_clear_intrbits_common(struct dw_i2c_dev *dev);

static inline void __i2c_dw_enable(struct dw_i2c_dev *dev)
{
	int val;

	dev->status |= STATUS_ACTIVE;
	regmap_read(dev->map, DW_IC_ENABLE, &val);
	regmap_write(dev->map, DW_IC_ENABLE,
		     ((val & ~DW_IC_ENABLE_TX_CMD_BLOCK) | DW_IC_ENABLE_ENABLE));
}

static inline void __i2c_dw_disable_nowait(struct dw_i2c_dev *dev)
{
	int val;

	regmap_read(dev->map, DW_IC_ENABLE, &val);
	regmap_write(dev->map, DW_IC_ENABLE, val & ~DW_IC_ENABLE_ENABLE);
	dev->status &= ~STATUS_ACTIVE;
}
