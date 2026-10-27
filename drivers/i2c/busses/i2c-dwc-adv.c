// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Synopsys Advanced I2C (DWC_i2c) controller driver
 *
 * Based on the Synopsys DesignWare I2C adapter driver (i2c-designware-*),
 * which is based on the TI DAVINCI I2C adapter driver.
 *
 * Copyright (C) 2006 Texas Instruments.
 * Copyright (C) 2007 MontaVista Software Inc.
 * Copyright (C) 2009 Provigent Ltd.
 * Copyright (C) 2026 Texas Instruments Incorporated - https://www.ti.com/
 */

#include <linux/array_size.h>
#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/clk.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/math.h>
#include <linux/math64.h>
#include <linux/minmax.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/pinctrl/consumer.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/property.h>
#include <linux/reset.h>
#include <linux/units.h>

#define DWC_ADV_I2C_OP_ENABLE			0x04
#define DWC_ADV_I2C_OP_I2C_CAPABILITIES		0x10
#define DWC_ADV_I2C_OP_I2C_CAPABILITIES_LOADING_400	BIT(3)
#define DWC_ADV_I2C_OP_I2C_BLOCK_OFFSET		0x14

#define DWC_ADV_I2C_HEADER_ID			GENMASK(7, 0)
#define DWC_ADV_I2C_HEADER_LENGTH		GENMASK(23, 8)
#define DWC_ADV_I2C_HEADER_ID_CORE		0xa1
#define DWC_ADV_I2C_HEADER_ID_TIMING		0xa2
#define DWC_ADV_I2C_HEADER_ID_FIFO		0xa3
#define DWC_ADV_I2C_HEADER_ID_INTR_DEBUG	0xa4
#define DWC_ADV_I2C_HEADER_ID_HWID		0xa5

#define DWC_ADV_I2C_CORE_CON			0x04
#define DWC_ADV_I2C_CORE_TAR			0x08

#define DWC_ADV_I2C_TIMING_HCNT			0x08
#define DWC_ADV_I2C_TIMING_LCNT			0x0c
#define DWC_ADV_I2C_TIMING_HS_HCNT		0x10
#define DWC_ADV_I2C_TIMING_HS_LCNT		0x14
#define DWC_ADV_I2C_TIMING_SDA_HOLD		0x18

#define DWC_ADV_I2C_FIFO_DATA_CMD		0x04
#define DWC_ADV_I2C_FIFO_RX_TL			0x08
#define DWC_ADV_I2C_FIFO_TX_TL			0x0c

#define DWC_ADV_I2C_INTR_INTR_STAT		0x04
#define DWC_ADV_I2C_INTR_INTR_MASK		0x08
#define DWC_ADV_I2C_INTR_RAW_INTR_STAT		0x0c
#define DWC_ADV_I2C_INTR_CLR_INTR		0x10
#define DWC_ADV_I2C_INTR_ENABLE_STATUS		0x14
#define DWC_ADV_I2C_INTR_TX_ABRT_SOURCE		0x18
#define DWC_ADV_I2C_INTR_STATUS			0x1c
#define DWC_ADV_I2C_INTR_TXFLR			0x20
#define DWC_ADV_I2C_INTR_RXFLR			0x24

#define DWC_ADV_I2C_HWID_COMP_TYPE		0x0c
#define DWC_ADV_I2C_COMP_TYPE_VALUE		0x44570140

#define DWC_ADV_I2C_CON_CONTROLLER		BIT(0)
#define DWC_ADV_I2C_CON_SPEED_MASK		GENMASK(5, 4)
#define DWC_ADV_I2C_CON_SPEED_STD		BIT(4)
#define DWC_ADV_I2C_CON_SPEED_FAST		BIT(5)
#define DWC_ADV_I2C_CON_SPEED_HIGH		(BIT(4) | BIT(5))
#define DWC_ADV_I2C_CON_10BITADDR_CTRLR		BIT(9)

#define DWC_ADV_I2C_TAR_10BITADDR_CTRLR		BIT(12)

#define DWC_ADV_I2C_DATA_CMD_DAT		GENMASK(7, 0)
#define DWC_ADV_I2C_DATA_CMD_READ		BIT(8)
#define DWC_ADV_I2C_DATA_CMD_STOP		BIT(9)
#define DWC_ADV_I2C_DATA_CMD_RESTART		BIT(10)

#define DWC_ADV_I2C_INTR_RX_UNDER		BIT(0)
#define DWC_ADV_I2C_INTR_RX_OVER		BIT(1)
#define DWC_ADV_I2C_INTR_RX_FULL		BIT(2)
#define DWC_ADV_I2C_INTR_TX_OVER		BIT(3)
#define DWC_ADV_I2C_INTR_TX_EMPTY		BIT(4)
#define DWC_ADV_I2C_INTR_RD_REQ			BIT(5)
#define DWC_ADV_I2C_INTR_TX_ABRT		BIT(6)
#define DWC_ADV_I2C_INTR_RX_DONE		BIT(7)
#define DWC_ADV_I2C_INTR_ACTIVITY		BIT(8)
#define DWC_ADV_I2C_INTR_STOP_DET		BIT(9)
#define DWC_ADV_I2C_INTR_START_DET		BIT(10)
#define DWC_ADV_I2C_INTR_GEN_CALL		BIT(11)
#define DWC_ADV_I2C_INTR_CTRLR_ON_HOLD		BIT(13)

#define DWC_ADV_I2C_INTR_CLR_ALL		BIT(0)
#define DWC_ADV_I2C_INTR_CLR_RX_UNDER		BIT(1)
#define DWC_ADV_I2C_INTR_CLR_RX_OVER		BIT(2)
#define DWC_ADV_I2C_INTR_CLR_TX_OVER		BIT(3)
#define DWC_ADV_I2C_INTR_CLR_RD_REQ		BIT(4)
#define DWC_ADV_I2C_INTR_CLR_TX_ABRT		BIT(5)
#define DWC_ADV_I2C_INTR_CLR_RX_DONE		BIT(6)
#define DWC_ADV_I2C_INTR_CLR_ACTIVITY		BIT(7)
#define DWC_ADV_I2C_INTR_CLR_STOP_DET		BIT(8)
#define DWC_ADV_I2C_INTR_CLR_START_DET		BIT(9)
#define DWC_ADV_I2C_INTR_CLR_GEN_CALL		BIT(10)

#define DWC_ADV_I2C_CTRLR_INTR_MASK \
	(DWC_ADV_I2C_INTR_RX_FULL | DWC_ADV_I2C_INTR_TX_ABRT | \
	 DWC_ADV_I2C_INTR_STOP_DET | DWC_ADV_I2C_INTR_TX_EMPTY)

#define DWC_ADV_I2C_ENABLE_ENABLE		BIT(0)
#define DWC_ADV_I2C_ENABLE_ABORT		BIT(1)

#define DWC_ADV_I2C_STATUS_ACTIVITY		BIT(0)
#define DWC_ADV_I2C_STATUS_CTRLR_ACTIVITY	BIT(5)
#define DWC_ADV_I2C_STATUS_CTRLR_HOLD_TX_FIFO_EMPTY BIT(7)

enum dwc_adv_i2c_abrt_bit {
	DWC_ADV_I2C_ABRT_7B_ADDR_NOACK = 0,
	DWC_ADV_I2C_ABRT_10ADDR1_NOACK = 1,
	DWC_ADV_I2C_ABRT_10ADDR2_NOACK = 2,
	DWC_ADV_I2C_ABRT_TXDATA_NOACK = 3,
	DWC_ADV_I2C_ABRT_GCALL_NOACK = 4,
	DWC_ADV_I2C_ABRT_GCALL_READ = 5,
	DWC_ADV_I2C_ABRT_HS_ACKDET = 6,
	DWC_ADV_I2C_ABRT_SBYTE_ACKDET = 7,
	DWC_ADV_I2C_ABRT_CTRLR_DIS = 11,
	DWC_ADV_I2C_ARB_LOST = 12,
	DWC_ADV_I2C_ABRT_USER_TRMNT = 16,
	DWC_ADV_I2C_ABRT_SDA_STUCK_AT_LOW = 17,
};

#define TX_ABRT_7B_ADDR_NOACK	BIT(DWC_ADV_I2C_ABRT_7B_ADDR_NOACK)
#define TX_ABRT_10ADDR1_NOACK	BIT(DWC_ADV_I2C_ABRT_10ADDR1_NOACK)
#define TX_ABRT_10ADDR2_NOACK	BIT(DWC_ADV_I2C_ABRT_10ADDR2_NOACK)
#define TX_ABRT_TXDATA_NOACK	BIT(DWC_ADV_I2C_ABRT_TXDATA_NOACK)
#define TX_ABRT_GCALL_NOACK	BIT(DWC_ADV_I2C_ABRT_GCALL_NOACK)
#define TX_ABRT_GCALL_READ	BIT(DWC_ADV_I2C_ABRT_GCALL_READ)
#define TX_ARB_LOST		BIT(DWC_ADV_I2C_ARB_LOST)
#define TX_ABRT_USER_TRMNT	BIT(DWC_ADV_I2C_ABRT_USER_TRMNT)
#define TX_ABRT_NOACK \
	(TX_ABRT_7B_ADDR_NOACK | \
	 TX_ABRT_10ADDR1_NOACK | \
	 TX_ABRT_10ADDR2_NOACK | \
	 TX_ABRT_TXDATA_NOACK | \
	 TX_ABRT_GCALL_NOACK)

#define DWC_ADV_I2C_STATUS_SW_ACTIVE		BIT(0)
#define DWC_ADV_I2C_STATUS_SW_WRITE_IN_PROGRESS	BIT(1)
#define DWC_ADV_I2C_STATUS_SW_READ_IN_PROGRESS	BIT(2)
#define DWC_ADV_I2C_STATUS_SW_MASK		GENMASK(2, 0)

#define DWC_ADV_I2C_ABORT_POLL_US		10
#define DWC_ADV_I2C_ABORT_TIMEOUT_US		100
#define DWC_ADV_I2C_DISABLE_POLL_US		250
#define DWC_ADV_I2C_DISABLE_TIMEOUT_US		25000

#define DWC_ADV_I2C_XFER_TIMEOUT_MS		1000
#define DWC_ADV_I2C_AUTOSUSPEND_DELAY_MS	1000

static const char * const dwc_adv_i2c_abort_sources[] = {
	[DWC_ADV_I2C_ABRT_7B_ADDR_NOACK] =
		"target address not acknowledged",
	[DWC_ADV_I2C_ABRT_10ADDR1_NOACK] =
		"10-bit first address byte not acknowledged",
	[DWC_ADV_I2C_ABRT_10ADDR2_NOACK] =
		"10-bit second address byte not acknowledged",
	[DWC_ADV_I2C_ABRT_TXDATA_NOACK] =
		"data not acknowledged",
	[DWC_ADV_I2C_ABRT_GCALL_NOACK] =
		"general call not acknowledged",
	[DWC_ADV_I2C_ABRT_GCALL_READ] =
		"general call read",
	[DWC_ADV_I2C_ABRT_HS_ACKDET] =
		"high speed controller code acknowledged",
	[DWC_ADV_I2C_ABRT_SBYTE_ACKDET] =
		"start byte acknowledged",
	[DWC_ADV_I2C_ABRT_CTRLR_DIS] =
		"disabled while active",
	[DWC_ADV_I2C_ARB_LOST] =
		"arbitration lost",
	[DWC_ADV_I2C_ABRT_USER_TRMNT] =
		"terminated by user request",
	[DWC_ADV_I2C_ABRT_SDA_STUCK_AT_LOW] =
		"SDA stuck at low",
};

struct dwc_adv_i2c_regs {
	u32 enable;
	u32 con;
	u32 tar;
	u32 ss_fs_hcnt;
	u32 ss_fs_lcnt;
	u32 hs_hcnt;
	u32 hs_lcnt;
	u32 sda_hold;
	u32 data_cmd;
	u32 rx_tl;
	u32 tx_tl;
	u32 intr_stat;
	u32 intr_mask;
	u32 raw_intr_stat;
	u32 clr_intr;
	u32 enable_status;
	u32 tx_abrt_source;
	u32 status;
	u32 txflr;
	u32 rxflr;
};

struct dwc_adv_i2c_dev {
	struct device *dev;
	void __iomem *base;
	struct clk *clk;
	struct clk *pclk;
	struct reset_control *rst;
	struct i2c_adapter adap;
	struct completion cmd_complete;
	struct i2c_bus_recovery_info rinfo;

	struct dwc_adv_i2c_regs regs;
	u32 tx_fifo_depth;
	u32 rx_fifo_depth;

	struct i2c_timings timings;
	bool hs_loading_400pf;
	u32 sda_hold_time;

	u32 ctrlr_cfg;
	u32 ss_hcnt;
	u32 ss_lcnt;
	u32 fs_hcnt;
	u32 fs_lcnt;
	u32 hs_hcnt;
	u32 hs_lcnt;

	struct i2c_msg *msgs;
	int msgs_num;
	int msg_write_idx;
	int msg_read_idx;
	int msg_err;
	int cmd_err;
	unsigned int status;
	unsigned int abort_source;
	int rx_outstanding;
	bool need_restart;

	u8 *tx_buf;
	size_t tx_buf_len;
	u8 *rx_buf;
	size_t rx_buf_len;
};

struct dwc_adv_i2c_match_data {
	u32 tx_fifo_depth;
	u32 rx_fifo_depth;
};

static const struct dwc_adv_i2c_match_data tda54_i2c_match_data = {
	.tx_fifo_depth = 32,
	.rx_fifo_depth = 32,
};

static inline u32 dwc_adv_reg_read(struct dwc_adv_i2c_dev *dev, u32 offset)
{
	return readl(dev->base + offset);
}

static inline void dwc_adv_reg_write(struct dwc_adv_i2c_dev *dev, u32 offset, u32 val)
{
	writel(val, dev->base + offset);
}

static int dwc_adv_i2c_next_header(struct dwc_adv_i2c_dev *dev, u32 hdr_offset,
				   u32 expected_id, u32 *next_offset)
{
	u32 hdr = dwc_adv_reg_read(dev, hdr_offset);
	u32 id = FIELD_GET(DWC_ADV_I2C_HEADER_ID, hdr);
	u32 length_words = FIELD_GET(DWC_ADV_I2C_HEADER_LENGTH, hdr);

	if (id != expected_id) {
		return dev_err_probe(dev->dev, -ENODEV,
				     "header at offset 0x%x has ID 0x%x, expected 0x%x\n",
				     hdr_offset, id, expected_id);
	}

	if (!length_words) {
		return dev_err_probe(dev->dev, -ENODEV,
				     "zero length header at offset 0x%x\n", hdr_offset);
	}

	if (next_offset)
		*next_offset = hdr_offset + length_words * 4;

	return 0;
}

/*
 * Register offsets are relative to per-block header registers. Walk the
 * header chain to find each block, starting from the core block offset.
 */
static int dwc_adv_i2c_discover_regs(struct dwc_adv_i2c_dev *dev)
{
	struct dwc_adv_i2c_regs *regs = &dev->regs;
	u32 core_header, timing_header, fifo_header, intr_header, hwid_header;
	u32 caps, comp_type;
	int ret;

	regs->enable = DWC_ADV_I2C_OP_ENABLE;
	caps = dwc_adv_reg_read(dev, DWC_ADV_I2C_OP_I2C_CAPABILITIES);
	dev->hs_loading_400pf = caps & DWC_ADV_I2C_OP_I2C_CAPABILITIES_LOADING_400;
	core_header = dwc_adv_reg_read(dev, DWC_ADV_I2C_OP_I2C_BLOCK_OFFSET);
	if (!core_header)
		return dev_err_probe(dev->dev, -ENODEV, "no I2C block offset\n");

	regs->con = core_header + DWC_ADV_I2C_CORE_CON;
	regs->tar = core_header + DWC_ADV_I2C_CORE_TAR;

	ret = dwc_adv_i2c_next_header(dev, core_header,
				      DWC_ADV_I2C_HEADER_ID_CORE, &timing_header);
	if (ret)
		return ret;

	regs->ss_fs_hcnt = timing_header + DWC_ADV_I2C_TIMING_HCNT;
	regs->ss_fs_lcnt = timing_header + DWC_ADV_I2C_TIMING_LCNT;
	regs->hs_hcnt = timing_header + DWC_ADV_I2C_TIMING_HS_HCNT;
	regs->hs_lcnt = timing_header + DWC_ADV_I2C_TIMING_HS_LCNT;
	regs->sda_hold = timing_header + DWC_ADV_I2C_TIMING_SDA_HOLD;

	ret = dwc_adv_i2c_next_header(dev, timing_header,
				      DWC_ADV_I2C_HEADER_ID_TIMING, &fifo_header);
	if (ret)
		return ret;

	regs->data_cmd = fifo_header + DWC_ADV_I2C_FIFO_DATA_CMD;
	regs->rx_tl = fifo_header + DWC_ADV_I2C_FIFO_RX_TL;
	regs->tx_tl = fifo_header + DWC_ADV_I2C_FIFO_TX_TL;

	ret = dwc_adv_i2c_next_header(dev, fifo_header,
				      DWC_ADV_I2C_HEADER_ID_FIFO, &intr_header);
	if (ret)
		return ret;

	regs->intr_stat = intr_header + DWC_ADV_I2C_INTR_INTR_STAT;
	regs->intr_mask = intr_header + DWC_ADV_I2C_INTR_INTR_MASK;
	regs->raw_intr_stat = intr_header + DWC_ADV_I2C_INTR_RAW_INTR_STAT;
	regs->clr_intr = intr_header + DWC_ADV_I2C_INTR_CLR_INTR;
	regs->enable_status = intr_header + DWC_ADV_I2C_INTR_ENABLE_STATUS;
	regs->tx_abrt_source = intr_header + DWC_ADV_I2C_INTR_TX_ABRT_SOURCE;
	regs->status = intr_header + DWC_ADV_I2C_INTR_STATUS;
	regs->txflr = intr_header + DWC_ADV_I2C_INTR_TXFLR;
	regs->rxflr = intr_header + DWC_ADV_I2C_INTR_RXFLR;

	ret = dwc_adv_i2c_next_header(dev, intr_header,
				      DWC_ADV_I2C_HEADER_ID_INTR_DEBUG, &hwid_header);
	if (ret)
		return ret;

	ret = dwc_adv_i2c_next_header(dev, hwid_header,
				      DWC_ADV_I2C_HEADER_ID_HWID, NULL);
	if (ret)
		return ret;

	comp_type = dwc_adv_reg_read(dev, hwid_header + DWC_ADV_I2C_HWID_COMP_TYPE);
	if (comp_type != DWC_ADV_I2C_COMP_TYPE_VALUE)
		return dev_err_probe(dev->dev, -ENODEV,
				     "unknown Synopsys component type: 0x%08x\n",
				     comp_type);

	return 0;
}

static void dwc_adv_i2c_ack_intr(struct dwc_adv_i2c_dev *dev, u32 clr_bit)
{
	dwc_adv_reg_write(dev, dev->regs.clr_intr, clr_bit);
}

static u32 dwc_adv_i2c_scl_hcnt(u32 ic_clk, u32 tsymbol, u32 tf, int offset)
{
	return DIV_ROUND_CLOSEST_ULL((u64)ic_clk * (tsymbol + tf), MICRO) - 3 + offset;
}

static u32 dwc_adv_i2c_scl_lcnt(u32 ic_clk, u32 tlow, u32 tf, int offset)
{
	return DIV_ROUND_CLOSEST_ULL((u64)ic_clk * (tlow + tf), MICRO) - 1 + offset;
}

static void dwc_adv_i2c_set_timings(struct dwc_adv_i2c_dev *dev)
{
	struct i2c_timings *t = &dev->timings;
	u32 ic_clk = clk_get_rate(dev->clk) / 1000;
	u32 sda_falling_time = t->sda_fall_ns ?: 300;
	u32 scl_falling_time = t->scl_fall_ns ?: 300;
	u32 t_high, t_low;

	dev->ss_hcnt = dwc_adv_i2c_scl_hcnt(ic_clk, 4000, sda_falling_time, 0);
	dev->ss_lcnt = dwc_adv_i2c_scl_lcnt(ic_clk, 4700, scl_falling_time, 0);

	if (t->bus_freq_hz == I2C_MAX_FAST_MODE_PLUS_FREQ) {
		dev->fs_hcnt = dwc_adv_i2c_scl_hcnt(ic_clk, 260, sda_falling_time, 0);
		dev->fs_lcnt = dwc_adv_i2c_scl_lcnt(ic_clk, 500, scl_falling_time, 0);
	} else {
		dev->fs_hcnt = dwc_adv_i2c_scl_hcnt(ic_clk, 600, sda_falling_time, 0);
		dev->fs_lcnt = dwc_adv_i2c_scl_lcnt(ic_clk, 1300, scl_falling_time, 0);
	}

	if (dev->hs_loading_400pf) {
		t_high = 120;
		t_low = 320;
	} else {
		t_high = 60;
		t_low = 160;
	}
	dev->hs_hcnt = dwc_adv_i2c_scl_hcnt(ic_clk, t_high, sda_falling_time, 0);
	dev->hs_lcnt = dwc_adv_i2c_scl_lcnt(ic_clk, t_low, scl_falling_time, 0);
}

static void dwc_adv_i2c_write_timings(struct dwc_adv_i2c_dev *dev)
{
	u32 speed = dev->ctrlr_cfg & DWC_ADV_I2C_CON_SPEED_MASK;

	if (speed == DWC_ADV_I2C_CON_SPEED_HIGH) {
		dwc_adv_reg_write(dev, dev->regs.hs_hcnt, dev->hs_hcnt);
		dwc_adv_reg_write(dev, dev->regs.hs_lcnt, dev->hs_lcnt);
		dwc_adv_reg_write(dev, dev->regs.ss_fs_hcnt, dev->fs_hcnt);
		dwc_adv_reg_write(dev, dev->regs.ss_fs_lcnt, dev->fs_lcnt);
	} else if (speed == DWC_ADV_I2C_CON_SPEED_STD) {
		dwc_adv_reg_write(dev, dev->regs.ss_fs_hcnt, dev->ss_hcnt);
		dwc_adv_reg_write(dev, dev->regs.ss_fs_lcnt, dev->ss_lcnt);
	} else {
		dwc_adv_reg_write(dev, dev->regs.ss_fs_hcnt, dev->fs_hcnt);
		dwc_adv_reg_write(dev, dev->regs.ss_fs_lcnt, dev->fs_lcnt);
	}
}

static void __dwc_adv_i2c_enable(struct dwc_adv_i2c_dev *dev)
{
	dev->status |= DWC_ADV_I2C_STATUS_SW_ACTIVE;
	dwc_adv_reg_write(dev, dev->regs.enable, DWC_ADV_I2C_ENABLE_ENABLE);
}

static void __dwc_adv_i2c_disable_nowait(struct dwc_adv_i2c_dev *dev)
{
	u32 enable = dwc_adv_reg_read(dev, dev->regs.enable);

	dwc_adv_reg_write(dev, dev->regs.enable, enable & ~DWC_ADV_I2C_ENABLE_ENABLE);
	dev->status &= ~DWC_ADV_I2C_STATUS_SW_ACTIVE;
}

static u32 dwc_adv_i2c_try_disable(struct dwc_adv_i2c_dev *dev)
{
	__dwc_adv_i2c_disable_nowait(dev);

	return dwc_adv_reg_read(dev, dev->regs.enable_status);
}

/*
 * If the controller is holding the bus with an empty TX FIFO, terminate the
 * transfer first. IC_ENABLE.TERMINATE is only honoured while ENABLE is set.
 */
static void __dwc_adv_i2c_disable(struct dwc_adv_i2c_dev *dev)
{
	u32 raw_intr_stat = dwc_adv_reg_read(dev, dev->regs.raw_intr_stat);
	u32 ic_status = dwc_adv_reg_read(dev, dev->regs.status);
	u32 enable = dwc_adv_reg_read(dev, dev->regs.enable);
	bool abort_needed = (raw_intr_stat & DWC_ADV_I2C_INTR_CTRLR_ON_HOLD) ||
			     (ic_status & DWC_ADV_I2C_STATUS_CTRLR_HOLD_TX_FIFO_EMPTY);
	u32 status;
	int ret;

	if (abort_needed) {
		if (!(enable & DWC_ADV_I2C_ENABLE_ENABLE)) {
			dwc_adv_reg_write(dev, dev->regs.enable, DWC_ADV_I2C_ENABLE_ENABLE);
			/* Let ENABLE take effect: 10 signaling periods */
			fsleep(DIV_ROUND_CLOSEST_ULL(10 * MICRO, dev->timings.bus_freq_hz));
			enable |= DWC_ADV_I2C_ENABLE_ENABLE;
		}
		dwc_adv_reg_write(dev, dev->regs.enable, enable | DWC_ADV_I2C_ENABLE_ABORT);

		ret = readl_poll_timeout(dev->base + dev->regs.enable, enable,
					 !(enable & DWC_ADV_I2C_ENABLE_ABORT),
					 DWC_ADV_I2C_ABORT_POLL_US,
					 DWC_ADV_I2C_ABORT_TIMEOUT_US);
		if (ret)
			dev_err(dev->dev, "timeout while trying to abort current transfer\n");
	}

	ret = read_poll_timeout(dwc_adv_i2c_try_disable, status,
				!(status & DWC_ADV_I2C_ENABLE_ENABLE),
				DWC_ADV_I2C_DISABLE_POLL_US,
				DWC_ADV_I2C_DISABLE_TIMEOUT_US, false, dev);
	if (ret)
		dev_warn(dev->dev, "timeout in disabling adapter\n");
}

static void dwc_adv_i2c_init(struct dwc_adv_i2c_dev *dev)
{
	__dwc_adv_i2c_disable(dev);

	dwc_adv_i2c_write_timings(dev);

	if (dev->sda_hold_time)
		dwc_adv_reg_write(dev, dev->regs.sda_hold, dev->sda_hold_time);

	dwc_adv_reg_write(dev, dev->regs.tx_tl, dev->tx_fifo_depth / 2);
	dwc_adv_reg_write(dev, dev->regs.rx_tl, 0);
	dwc_adv_reg_write(dev, dev->regs.con, dev->ctrlr_cfg);
}

static int dwc_adv_i2c_wait_bus_not_busy(struct dwc_adv_i2c_dev *dev)
{
	u32 status;
	int ret;

	ret = readl_poll_timeout(dev->base + dev->regs.status, status,
				 !(status & DWC_ADV_I2C_STATUS_ACTIVITY),
				 1000, 20000);
	if (ret) {
		dev_warn(dev->dev, "timeout waiting for bus ready\n");
		i2c_recover_bus(&dev->adap);

		status = dwc_adv_reg_read(dev, dev->regs.status);
		if (!(status & DWC_ADV_I2C_STATUS_ACTIVITY))
			ret = 0;
	}

	return ret;
}

/*
 * Wait for the controller to go idle once the transfer has completed (STOP_DET
 * or TX_ABRT), so that disabling it does not cut the transfer short.
 * Return: true if it is still active after the timeout.
 */
static bool dwc_adv_i2c_is_controller_active(struct dwc_adv_i2c_dev *dev)
{
	u32 status;

	status = dwc_adv_reg_read(dev, dev->regs.status);
	if (!(status & DWC_ADV_I2C_STATUS_CTRLR_ACTIVITY))
		return false;

	return readl_poll_timeout(dev->base + dev->regs.status, status,
				  !(status & DWC_ADV_I2C_STATUS_CTRLR_ACTIVITY),
				  1100, 20000) != 0;
}

static void dwc_adv_i2c_xfer_init(struct dwc_adv_i2c_dev *dev)
{
	struct i2c_msg *msg = &dev->msgs[dev->msg_write_idx];
	u32 con;

	__dwc_adv_i2c_disable(dev);

	/* If the target address is ten bit address, enable 10BITADDR */
	con = dev->ctrlr_cfg;
	if (msg->flags & I2C_M_TEN)
		con |= DWC_ADV_I2C_CON_10BITADDR_CTRLR;
	else
		con &= ~DWC_ADV_I2C_CON_10BITADDR_CTRLR;
	dwc_adv_reg_write(dev, dev->regs.con, con);

	/*
	 * With IC_DYNAMIC_TAR_UPDATE the 10-bit addressing mode is controlled
	 * by IC_TAR bit 12 and the IC_CTRL bit is read-only. This cannot be
	 * detected from the registers, so always set it.
	 */
	dwc_adv_reg_write(dev, dev->regs.tar, msg->addr |
			   ((msg->flags & I2C_M_TEN) ? DWC_ADV_I2C_TAR_10BITADDR_CTRLR : 0));

	/*
	 * Enforce disabled interrupts: IC_INTR_MASK resets with TX_EMPTY
	 * unmasked, which would fire as soon as the controller is enabled.
	 */
	dwc_adv_reg_write(dev, dev->regs.intr_mask, 0);

	/* Enable the adapter, then clear and enable interrupts */
	__dwc_adv_i2c_enable(dev);
	dwc_adv_i2c_ack_intr(dev, DWC_ADV_I2C_INTR_CLR_ALL);
	dwc_adv_reg_write(dev, dev->regs.intr_mask, DWC_ADV_I2C_CTRLR_INTR_MASK);
}

/*
 * Initiate (and continue) low level controller read/write transaction.
 * This function is only called from dwc_adv_i2c_isr(), and pumps i2c_msg
 * messages into the TX FIFO. Even if the size of i2c_msg data is longer
 * than the TX FIFO, it handles everything.
 */
static void dwc_adv_i2c_xfer_msg(struct dwc_adv_i2c_dev *dev)
{
	struct i2c_msg *msgs = dev->msgs;
	u32 intr_mask = DWC_ADV_I2C_CTRLR_INTR_MASK;
	int tx_limit, rx_limit;
	u32 buf_len = dev->tx_buf_len;
	u8 *buf = dev->tx_buf;
	u32 cmd;
	int rx_tl;

	for (; dev->msg_write_idx < dev->msgs_num; dev->msg_write_idx++) {
		u32 flags = msgs[dev->msg_write_idx].flags;

		if (!(dev->status & DWC_ADV_I2C_STATUS_SW_WRITE_IN_PROGRESS)) {
			buf = msgs[dev->msg_write_idx].buf;
			buf_len = msgs[dev->msg_write_idx].len;
		}

		tx_limit = dev->tx_fifo_depth - dwc_adv_reg_read(dev, dev->regs.txflr);
		rx_limit = dev->rx_fifo_depth - dwc_adv_reg_read(dev, dev->regs.rxflr);

		while (buf_len > 0 && tx_limit > 0 && rx_limit > 0) {
			cmd = 0;

			/*
			 * The bus is held while the TX FIFO is empty unless
			 * STOP is requested in IC_DATA_CMD, so always set it
			 * on the last byte of the last message.
			 *
			 * i2c-core always sets the buffer length of
			 * I2C_FUNC_SMBUS_BLOCK_DATA to 1. The length will be
			 * adjusted when receiving the first byte. Thus we
			 * can't stop the transaction here.
			 */
			if (dev->msg_write_idx == dev->msgs_num - 1 &&
			    buf_len == 1 && !(flags & I2C_M_RECV_LEN))
				cmd |= DWC_ADV_I2C_DATA_CMD_STOP;

			/*
			 * A RESTART is only issued automatically when the
			 * direction changes. Request it explicitly at the
			 * start of each following message.
			 */
			if (dev->need_restart) {
				cmd |= DWC_ADV_I2C_DATA_CMD_RESTART;
				dev->need_restart = false;
			}

			if (msgs[dev->msg_write_idx].flags & I2C_M_RD) {
				/* Avoid rx buffer overrun */
				if (dev->rx_outstanding >= dev->rx_fifo_depth)
					break;
				dwc_adv_reg_write(dev, dev->regs.data_cmd,
						  cmd | DWC_ADV_I2C_DATA_CMD_READ);
				rx_limit--;
				dev->rx_outstanding++;
			} else {
				dwc_adv_reg_write(dev, dev->regs.data_cmd, cmd | *buf++);
			}
			tx_limit--;
			buf_len--;
		}

		dev->tx_buf = buf;
		dev->tx_buf_len = buf_len;

		/*
		 * Because we don't know the buffer length in the
		 * I2C_FUNC_SMBUS_BLOCK_DATA case, we can't stop the
		 * transaction here. Also disable the TX_EMPTY IRQ while
		 * waiting for the data length byte to avoid a flood of
		 * bogus interrupts.
		 */
		if (flags & I2C_M_RECV_LEN) {
			dev->status |= DWC_ADV_I2C_STATUS_SW_WRITE_IN_PROGRESS;
			intr_mask &= ~DWC_ADV_I2C_INTR_TX_EMPTY;
			break;
		}

		if (buf_len > 0) {
			dev->status |= DWC_ADV_I2C_STATUS_SW_WRITE_IN_PROGRESS;
			break;
		}
		dev->status &= ~DWC_ADV_I2C_STATUS_SW_WRITE_IN_PROGRESS;
		dev->need_restart = true;
	}

	/*
	 * If i2c_msg index search is completed, we don't need TX_EMPTY
	 * interrupt any more.
	 */
	if (dev->msg_write_idx == dev->msgs_num)
		intr_mask &= ~DWC_ADV_I2C_INTR_TX_EMPTY;

	if (dev->msg_err)
		intr_mask = 0;

	/*
	 * Size the RX FIFO threshold to the reads already queued, so the
	 * controller raises one RX_FULL for the whole burst instead of one per
	 * received byte. While messages remain to be queued, cap it so RX_FULL
	 * still arrives in time to drain the FIFO and let the next TX_EMPTY
	 * queue the rest.
	 */
	rx_tl = dev->rx_outstanding;
	if (dev->msg_write_idx < dev->msgs_num)
		rx_tl = min_t(int, rx_tl, dev->rx_fifo_depth / 2);
	dwc_adv_reg_write(dev, dev->regs.rx_tl, rx_tl ? rx_tl - 1 : 0);

	dwc_adv_reg_write(dev, dev->regs.intr_mask, intr_mask);
}

static u8 dwc_adv_i2c_recv_len(struct dwc_adv_i2c_dev *dev, u8 len)
{
	struct i2c_msg *msgs = dev->msgs;
	u32 flags = msgs[dev->msg_read_idx].flags;
	u32 intr_mask;

	/*
	 * Adjust the buffer length and mask the flag
	 * after receiving the first byte.
	 */
	len += (flags & I2C_CLIENT_PEC) ? 2 : 1;
	dev->tx_buf_len = len - min_t(int, len, dev->rx_outstanding);
	msgs[dev->msg_read_idx].len = len;
	msgs[dev->msg_read_idx].flags &= ~I2C_M_RECV_LEN;

	/*
	 * Received buffer length, re-enable TX_EMPTY interrupt
	 * to resume the SMBus transaction.
	 */
	intr_mask = dwc_adv_reg_read(dev, dev->regs.intr_mask);
	intr_mask |= DWC_ADV_I2C_INTR_TX_EMPTY;
	dwc_adv_reg_write(dev, dev->regs.intr_mask, intr_mask);

	return len;
}

static void dwc_adv_i2c_read(struct dwc_adv_i2c_dev *dev)
{
	struct i2c_msg *msgs = dev->msgs;
	int rx_valid;

	for (; dev->msg_read_idx < dev->msgs_num; dev->msg_read_idx++) {
		u32 len;
		u8 *buf;

		if (!(msgs[dev->msg_read_idx].flags & I2C_M_RD))
			continue;

		if (!(dev->status & DWC_ADV_I2C_STATUS_SW_READ_IN_PROGRESS)) {
			len = msgs[dev->msg_read_idx].len;
			buf = msgs[dev->msg_read_idx].buf;
		} else {
			len = dev->rx_buf_len;
			buf = dev->rx_buf;
		}

		rx_valid = dwc_adv_reg_read(dev, dev->regs.rxflr);

		for (; len > 0 && rx_valid > 0; len--, rx_valid--) {
			u32 data = dwc_adv_reg_read(dev, dev->regs.data_cmd) &
				   DWC_ADV_I2C_DATA_CMD_DAT;

			if (msgs[dev->msg_read_idx].flags & I2C_M_RECV_LEN) {
				/*
				 * STOP is only set after receiving the block
				 * data response length. When that length is
				 * invalid, read one more byte with STOP set to
				 * complete the transaction.
				 */
				if (!data || data > I2C_SMBUS_BLOCK_MAX)
					data = 1;

				len = dwc_adv_i2c_recv_len(dev, data);
			}
			*buf++ = data;
			dev->rx_outstanding--;
		}

		if (len > 0) {
			dev->status |= DWC_ADV_I2C_STATUS_SW_READ_IN_PROGRESS;
			dev->rx_buf_len = len;
			dev->rx_buf = buf;
			return;
		}
		dev->status &= ~DWC_ADV_I2C_STATUS_SW_READ_IN_PROGRESS;
	}
}

/*
 * Clear the sources individually: IC_CLR_INTR could clear interrupts raised
 * between reading IC_INTR_STAT and the clear. The abort source is saved
 * before TX_ABRT is cleared, as clearing also resets IC_TX_TRMNT_SOURCE.
 */
static u32 dwc_adv_i2c_read_clear_intrbits(struct dwc_adv_i2c_dev *dev)
{
	u32 stat = dwc_adv_reg_read(dev, dev->regs.intr_stat);

	if (stat & DWC_ADV_I2C_INTR_RX_UNDER)
		dwc_adv_i2c_ack_intr(dev, DWC_ADV_I2C_INTR_CLR_RX_UNDER);
	if (stat & DWC_ADV_I2C_INTR_RX_OVER)
		dwc_adv_i2c_ack_intr(dev, DWC_ADV_I2C_INTR_CLR_RX_OVER);
	if (stat & DWC_ADV_I2C_INTR_TX_OVER)
		dwc_adv_i2c_ack_intr(dev, DWC_ADV_I2C_INTR_CLR_TX_OVER);
	if (stat & DWC_ADV_I2C_INTR_RD_REQ)
		dwc_adv_i2c_ack_intr(dev, DWC_ADV_I2C_INTR_CLR_RD_REQ);
	if (stat & DWC_ADV_I2C_INTR_TX_ABRT) {
		dev->abort_source = dwc_adv_reg_read(dev, dev->regs.tx_abrt_source);
		dwc_adv_i2c_ack_intr(dev, DWC_ADV_I2C_INTR_CLR_TX_ABRT);
	}
	if (stat & DWC_ADV_I2C_INTR_RX_DONE)
		dwc_adv_i2c_ack_intr(dev, DWC_ADV_I2C_INTR_CLR_RX_DONE);
	if (stat & DWC_ADV_I2C_INTR_ACTIVITY)
		dwc_adv_i2c_ack_intr(dev, DWC_ADV_I2C_INTR_CLR_ACTIVITY);
	if (stat & DWC_ADV_I2C_INTR_STOP_DET &&
	    (dev->rx_outstanding == 0 || (stat & DWC_ADV_I2C_INTR_RX_FULL)))
		dwc_adv_i2c_ack_intr(dev, DWC_ADV_I2C_INTR_CLR_STOP_DET);
	if (stat & DWC_ADV_I2C_INTR_START_DET)
		dwc_adv_i2c_ack_intr(dev, DWC_ADV_I2C_INTR_CLR_START_DET);
	if (stat & DWC_ADV_I2C_INTR_GEN_CALL)
		dwc_adv_i2c_ack_intr(dev, DWC_ADV_I2C_INTR_CLR_GEN_CALL);

	return stat;
}

static void dwc_adv_i2c_process_transfer(struct dwc_adv_i2c_dev *dev, u32 stat)
{
	if (stat & DWC_ADV_I2C_INTR_TX_ABRT) {
		/* TX_ABRT flushes the TX/RX FIFOs, nothing left to process */
		dev->cmd_err |= DWC_ADV_I2C_INTR_TX_ABRT;
		dev->status &= ~DWC_ADV_I2C_STATUS_SW_MASK;
		dev->rx_outstanding = 0;
		dwc_adv_reg_write(dev, dev->regs.intr_mask, 0);
		goto tx_aborted;
	}

	if (stat & DWC_ADV_I2C_INTR_RX_FULL)
		dwc_adv_i2c_read(dev);

	if (stat & DWC_ADV_I2C_INTR_TX_EMPTY)
		dwc_adv_i2c_xfer_msg(dev);

	/* A STOP in the middle of a read or write is an error */
	if ((stat & DWC_ADV_I2C_INTR_STOP_DET) &&
	    (dev->status & (DWC_ADV_I2C_STATUS_SW_WRITE_IN_PROGRESS |
			     DWC_ADV_I2C_STATUS_SW_READ_IN_PROGRESS))) {
		dev_err(dev->dev, "spurious STOP, status 0x%x\n", dev->status);
		dev->rx_outstanding = 0;
		dev->msg_err = -EIO;
	}

tx_aborted:
	if (((stat & (DWC_ADV_I2C_INTR_TX_ABRT | DWC_ADV_I2C_INTR_STOP_DET)) ||
	     dev->msg_err) && dev->rx_outstanding == 0)
		complete(&dev->cmd_complete);
}

static irqreturn_t dwc_adv_i2c_handle_irq(struct dwc_adv_i2c_dev *dev)
{
	u32 enabled, stat;

	enabled = dwc_adv_reg_read(dev, dev->regs.enable);
	stat = dwc_adv_reg_read(dev, dev->regs.raw_intr_stat);

	if (!enabled || !(stat & ~DWC_ADV_I2C_INTR_ACTIVITY))
		return IRQ_NONE;

	if (stat == GENMASK(31, 0))
		return IRQ_NONE;

	stat = dwc_adv_i2c_read_clear_intrbits(dev);

	if (!(dev->status & DWC_ADV_I2C_STATUS_SW_ACTIVE)) {
		dwc_adv_reg_write(dev, dev->regs.intr_mask, 0);
		return IRQ_HANDLED;
	}

	dwc_adv_i2c_process_transfer(dev, stat);

	return IRQ_HANDLED;
}

static irqreturn_t dwc_adv_i2c_isr(int irq, void *dev_id)
{
	struct dwc_adv_i2c_dev *dev = dev_id;
	irqreturn_t ret;
	int pm;

	pm = pm_runtime_get_if_active(dev->dev);
	if (!pm)
		return IRQ_NONE;

	ret = dwc_adv_i2c_handle_irq(dev);

	if (pm > 0)
		pm_runtime_put_autosuspend(dev->dev);

	return ret;
}

static int dwc_adv_i2c_handle_tx_abort(struct dwc_adv_i2c_dev *dev)
{
	u32 abort_source = dev->abort_source;
	int i;

	for (i = 0; i < ARRAY_SIZE(dwc_adv_i2c_abort_sources); i++) {
		if (!(abort_source & BIT(i)) || !dwc_adv_i2c_abort_sources[i])
			continue;
		if (BIT(i) & (TX_ABRT_NOACK | TX_ABRT_USER_TRMNT))
			dev_dbg(dev->dev, "%s\n", dwc_adv_i2c_abort_sources[i]);
		else
			dev_err(dev->dev, "%s\n", dwc_adv_i2c_abort_sources[i]);
	}

	if (abort_source & TX_ABRT_NOACK)
		return -EREMOTEIO;
	if (abort_source & TX_ARB_LOST)
		return -EAGAIN;
	if (abort_source & TX_ABRT_GCALL_READ)
		return -EINVAL;

	return -EIO;
}

static bool dwc_adv_i2c_msg_is_valid(struct dwc_adv_i2c_dev *dev,
				     struct i2c_msg *msgs, int idx)
{
	if (idx == 0)
		return true;

	if (msgs[idx - 1].addr != msgs[idx].addr) {
		dev_err(dev->dev, "invalid target address\n");
		return false;
	}

	return true;
}

static int dwc_adv_i2c_xfer_one_part(struct dwc_adv_i2c_dev *dev)
{
	int ret;

	reinit_completion(&dev->cmd_complete);
	dev->cmd_err = 0;
	dev->msg_write_idx = 0;
	dev->msg_read_idx = 0;
	dev->msg_err = 0;
	dev->status = 0;
	dev->abort_source = 0;
	dev->rx_outstanding = 0;
	dev->need_restart = false;

	ret = dwc_adv_i2c_wait_bus_not_busy(dev);
	if (ret)
		return ret;

	dwc_adv_i2c_xfer_init(dev);

	if (!wait_for_completion_timeout(&dev->cmd_complete, dev->adap.timeout)) {
		dev_err(dev->dev, "controller timed out\n");
		i2c_recover_bus(&dev->adap);
		dwc_adv_i2c_init(dev);
		return -ETIMEDOUT;
	}

	if (dwc_adv_i2c_is_controller_active(dev)) {
		/*
		 * If the controller is still active after the timeout, attempt a
		 * bus recovery to clear any potentially locked state.
		 */
		dev_err(dev->dev, "controller active after xfer, recovering\n");
		i2c_recover_bus(&dev->adap);
		dwc_adv_i2c_init(dev);
	} else {
		/*
		 * Disable the controller before returning, so that late
		 * interrupts do not race with the following transfer.
		 */
		__dwc_adv_i2c_disable_nowait(dev);
	}

	if (dev->msg_err)
		return dev->msg_err;

	if (!dev->cmd_err && !dev->status)
		return 0;

	if (dev->cmd_err & DWC_ADV_I2C_INTR_TX_ABRT)
		return dwc_adv_i2c_handle_tx_abort(dev);

	if (dev->status)
		dev_err(dev->dev, "transfer terminated early, status 0x%x\n", dev->status);

	return -EIO;
}

static int dwc_adv_i2c_xfer(struct i2c_adapter *adap, struct i2c_msg msgs[], int num)
{
	struct dwc_adv_i2c_dev *dev = i2c_get_adapdata(adap);
	int ret, idx, start = 0;

	PM_RUNTIME_ACQUIRE_AUTOSUSPEND(dev->dev, pm);
	if (PM_RUNTIME_ACQUIRE_ERR(&pm))
		return -ENXIO;

	for (idx = 0; idx < num; idx++) {
		if (!dwc_adv_i2c_msg_is_valid(dev, &msgs[start], idx - start))
			return -EINVAL;

		if (!(msgs[idx].flags & I2C_M_STOP) && idx != num - 1)
			continue;

		dev->msgs = &msgs[start];
		dev->msgs_num = idx - start + 1;

		ret = dwc_adv_i2c_xfer_one_part(dev);
		if (ret)
			return ret;

		start = idx + 1;
	}

	return num;
}

static u32 dwc_adv_i2c_func(struct i2c_adapter *adap)
{
	return I2C_FUNC_I2C | I2C_FUNC_SMBUS_BYTE | I2C_FUNC_SMBUS_BYTE_DATA |
	       I2C_FUNC_SMBUS_WORD_DATA | I2C_FUNC_SMBUS_BLOCK_DATA |
	       I2C_FUNC_SMBUS_I2C_BLOCK | I2C_FUNC_10BIT_ADDR | I2C_FUNC_PROTOCOL_MANGLING;
}

static const struct i2c_algorithm dwc_adv_i2c_algo = {
	.xfer = dwc_adv_i2c_xfer,
	.functionality = dwc_adv_i2c_func,
};

static const struct i2c_adapter_quirks dwc_adv_i2c_quirks = {
	.flags = I2C_AQ_NO_ZERO_LEN,
};

static int dwc_adv_i2c_prepare_clk(struct dwc_adv_i2c_dev *dev, bool prepare)
{
	int ret;

	if (!prepare) {
		clk_disable_unprepare(dev->clk);
		clk_disable_unprepare(dev->pclk);
		return 0;
	}

	ret = clk_prepare_enable(dev->pclk);
	if (ret)
		return ret;

	ret = clk_prepare_enable(dev->clk);
	if (ret)
		clk_disable_unprepare(dev->pclk);

	return ret;
}

static void dwc_adv_i2c_prepare_recovery(struct i2c_adapter *adap)
{
	struct dwc_adv_i2c_dev *dev = i2c_get_adapdata(adap);

	__dwc_adv_i2c_disable(dev);
	reset_control_assert(dev->rst);
	dwc_adv_i2c_prepare_clk(dev, false);
}

static void dwc_adv_i2c_unprepare_recovery(struct i2c_adapter *adap)
{
	struct dwc_adv_i2c_dev *dev = i2c_get_adapdata(adap);
	int ret;

	ret = dwc_adv_i2c_prepare_clk(dev, true);
	if (ret) {
		dev_err(dev->dev, "failed to enable clock for recovery: %d\n", ret);
		reset_control_deassert(dev->rst);
		return;
	}

	reset_control_deassert(dev->rst);
	dwc_adv_i2c_init(dev);
}

static int dwc_adv_i2c_init_recovery_info(struct dwc_adv_i2c_dev *dev)
{
	struct i2c_bus_recovery_info *rinfo = &dev->rinfo;

	rinfo->scl_gpiod = devm_gpiod_get_optional(dev->dev, "scl", GPIOD_OUT_HIGH);
	if (IS_ERR_OR_NULL(rinfo->scl_gpiod))
		return PTR_ERR_OR_ZERO(rinfo->scl_gpiod);

	rinfo->sda_gpiod = devm_gpiod_get_optional(dev->dev, "sda", GPIOD_IN);
	if (IS_ERR(rinfo->sda_gpiod))
		return PTR_ERR(rinfo->sda_gpiod);

	rinfo->pinctrl = devm_pinctrl_get(dev->dev);
	if (IS_ERR(rinfo->pinctrl)) {
		if (PTR_ERR(rinfo->pinctrl) == -EPROBE_DEFER)
			return -EPROBE_DEFER;
		rinfo->pinctrl = NULL;
	}

	rinfo->recover_bus = i2c_generic_scl_recovery;
	rinfo->prepare_recovery = dwc_adv_i2c_prepare_recovery;
	rinfo->unprepare_recovery = dwc_adv_i2c_unprepare_recovery;
	dev->adap.bus_recovery_info = rinfo;

	return 0;
}

static void dwc_adv_i2c_configure_controller(struct dwc_adv_i2c_dev *dev)
{
	dev->ctrlr_cfg = DWC_ADV_I2C_CON_CONTROLLER;

	switch (dev->timings.bus_freq_hz) {
	case I2C_MAX_STANDARD_MODE_FREQ:
		dev->ctrlr_cfg |= DWC_ADV_I2C_CON_SPEED_STD;
		break;
	case I2C_MAX_HIGH_SPEED_MODE_FREQ:
		dev->ctrlr_cfg |= DWC_ADV_I2C_CON_SPEED_HIGH;
		break;
	default:
		dev->ctrlr_cfg |= DWC_ADV_I2C_CON_SPEED_FAST;
		break;
	}
}

static int dwc_adv_i2c_parse_fw(struct dwc_adv_i2c_dev *dev)
{
	struct device *device = dev->dev;

	i2c_parse_fw_timings(device, &dev->timings, false);
	if (!dev->timings.bus_freq_hz)
		dev->timings.bus_freq_hz = I2C_MAX_FAST_MODE_FREQ;

	switch (dev->timings.bus_freq_hz) {
	case I2C_MAX_STANDARD_MODE_FREQ:
	case I2C_MAX_FAST_MODE_FREQ:
	case I2C_MAX_FAST_MODE_PLUS_FREQ:
	case I2C_MAX_HIGH_SPEED_MODE_FREQ:
		break;
	default:
		return dev_err_probe(device, -EINVAL, "invalid clock-frequency %u\n",
				     dev->timings.bus_freq_hz);
	}

	return 0;
}

static int dwc_adv_i2c_runtime_suspend(struct device *device)
{
	struct dwc_adv_i2c_dev *dev = dev_get_drvdata(device);

	__dwc_adv_i2c_disable(dev);
	dwc_adv_i2c_prepare_clk(dev, false);

	return 0;
}

static int dwc_adv_i2c_runtime_resume(struct device *device)
{
	struct dwc_adv_i2c_dev *dev = dev_get_drvdata(device);
	int ret;

	ret = dwc_adv_i2c_prepare_clk(dev, true);
	if (ret)
		return ret;

	dwc_adv_i2c_init(dev);

	return 0;
}

static int dwc_adv_i2c_suspend(struct device *device)
{
	struct dwc_adv_i2c_dev *dev = dev_get_drvdata(device);

	i2c_mark_adapter_suspended(&dev->adap);

	return pm_runtime_force_suspend(device);
}

static int dwc_adv_i2c_resume(struct device *device)
{
	struct dwc_adv_i2c_dev *dev = dev_get_drvdata(device);
	int ret;

	ret = pm_runtime_force_resume(device);
	if (ret)
		return ret;

	i2c_mark_adapter_resumed(&dev->adap);

	return 0;
}

static const struct dev_pm_ops dwc_adv_i2c_pm_ops = {
	LATE_SYSTEM_SLEEP_PM_OPS(dwc_adv_i2c_suspend, dwc_adv_i2c_resume)
	RUNTIME_PM_OPS(dwc_adv_i2c_runtime_suspend, dwc_adv_i2c_runtime_resume, NULL)
};

static void dwc_adv_i2c_pm_teardown(void *data)
{
	struct dwc_adv_i2c_dev *dev = data;
	int ret;

	ret = pm_runtime_resume_and_get(dev->dev);
	if (!ret)
		__dwc_adv_i2c_disable(dev);
	pm_runtime_dont_use_autosuspend(dev->dev);
	if (!ret)
		pm_runtime_put_noidle(dev->dev);
	pm_runtime_disable(dev->dev);
}

static int dwc_adv_i2c_probe(struct platform_device *pdev)
{
	struct device *device = &pdev->dev;
	const struct dwc_adv_i2c_match_data *match_data;
	struct dwc_adv_i2c_dev *dev;
	int irq, ret;

	match_data = device_get_match_data(device);
	if (!match_data)
		return -ENODEV;

	dev = devm_kzalloc(device, sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;

	dev->dev = device;
	dev->tx_fifo_depth = match_data->tx_fifo_depth;
	dev->rx_fifo_depth = match_data->rx_fifo_depth;

	dev->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(dev->base))
		return PTR_ERR(dev->base);

	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;

	dev->rst = devm_reset_control_get_optional_exclusive_deasserted(device, NULL);
	if (IS_ERR(dev->rst))
		return dev_err_probe(device, PTR_ERR(dev->rst), "failed to get reset\n");

	ret = dwc_adv_i2c_parse_fw(dev);
	if (ret)
		return ret;

	dwc_adv_i2c_configure_controller(dev);

	dev->pclk = devm_clk_get_enabled(device, "pclk");
	if (IS_ERR(dev->pclk))
		return dev_err_probe(device, PTR_ERR(dev->pclk), "failed to get pclk\n");

	dev->clk = devm_clk_get_enabled(device, "ref");
	if (IS_ERR(dev->clk))
		return dev_err_probe(device, PTR_ERR(dev->clk), "failed to get ref clock\n");

	ret = dwc_adv_i2c_discover_regs(dev);
	if (ret)
		return ret;

	if (dev->timings.sda_hold_ns)
		dev->sda_hold_time =
			DIV_S64_ROUND_CLOSEST((s64)clk_get_rate(dev->clk) / 1000 *
					      dev->timings.sda_hold_ns, MICRO);

	dwc_adv_i2c_set_timings(dev);

	init_completion(&dev->cmd_complete);

	platform_set_drvdata(pdev, dev);

	ret = devm_request_irq(device, irq, dwc_adv_i2c_isr, 0, dev_name(device), dev);
	if (ret)
		return dev_err_probe(device, ret, "failed to request irq %d\n", irq);

	ret = dwc_adv_i2c_init_recovery_info(dev);
	if (ret)
		return dev_err_probe(device, ret, "failed to init bus recovery\n");

	i2c_set_adapdata(&dev->adap, dev);
	dev->adap.owner = THIS_MODULE;
	strscpy(dev->adap.name, "Synopsys Advanced I2C adapter",
		sizeof(dev->adap.name));
	dev->adap.algo = &dwc_adv_i2c_algo;
	dev->adap.quirks = &dwc_adv_i2c_quirks;
	dev->adap.dev.parent = device;
	dev->adap.timeout = msecs_to_jiffies(DWC_ADV_I2C_XFER_TIMEOUT_MS);
	dev->adap.retries = 3;
	device_set_node(&dev->adap.dev, dev_fwnode(device));

	dwc_adv_i2c_init(dev);

	pm_runtime_set_autosuspend_delay(device, DWC_ADV_I2C_AUTOSUSPEND_DELAY_MS);
	pm_runtime_use_autosuspend(device);
	ret = pm_runtime_set_active(device);
	if (ret)
		return dev_err_probe(device, ret, "failed to set runtime PM active\n");

	pm_runtime_enable(device);

	ret = devm_add_action_or_reset(device, dwc_adv_i2c_pm_teardown, dev);
	if (ret)
		return ret;

	return devm_i2c_add_adapter(device, &dev->adap);
}

static const struct of_device_id dwc_adv_i2c_of_match[] = {
	{ .compatible = "ti,tda54-i2c", .data = &tda54_i2c_match_data },
	{ }
};
MODULE_DEVICE_TABLE(of, dwc_adv_i2c_of_match);

static struct platform_driver dwc_adv_i2c_driver = {
	.probe = dwc_adv_i2c_probe,
	.driver = {
		.name = "i2c-dwc-adv",
		.of_match_table = dwc_adv_i2c_of_match,
		.pm = pm_ptr(&dwc_adv_i2c_pm_ops),
	},
};
module_platform_driver(dwc_adv_i2c_driver);

MODULE_AUTHOR("Aniket Limaye <a-limaye@ti.com>");
MODULE_DESCRIPTION("Synopsys Advanced I2C (DWC_i2c) driver");
MODULE_LICENSE("GPL");
