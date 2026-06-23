// SPDX-License-Identifier: GPL-2.0-only
/*
 * ASPEED AST2600 new register set I2C controller driver
 *
 * Copyright (C) 2026 ASPEED Technology Inc.
 */
#include <linux/array_size.h>
#include <linux/bits.h>
#include <linux/clk.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/i2c.h>
#include <linux/i2c-smbus.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/minmax.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/regmap.h>
#include <linux/reset.h>
#include <linux/slab.h>
#include <linux/unaligned.h>

#define AST2600_I2CG_ISR			0x00
#define AST2600_I2CG_SLAVE_ISR		0x04
#define AST2600_I2CG_OWNER		0x08
#define AST2600_I2CG_CTRL		0x0C
#define AST2600_I2CG_CLK_DIV_CTRL	0x10

#define AST2600_I2CG_SLAVE_PKT_NAK	BIT(4)
#define AST2600_I2CG_M_S_SEPARATE_INTR	BIT(3)
#define AST2600_I2CG_CTRL_NEW_REG	BIT(2)
#define AST2600_I2CG_CTRL_NEW_CLK_DIV	BIT(1)
#define AST2600_GLOBAL_INIT	\
	(AST2600_I2CG_CTRL_NEW_REG | AST2600_I2CG_CTRL_NEW_CLK_DIV)
/*
 * APB clk : 100Mhz
 * div	: scl		: baseclk [APB/((div/2) + 1)] : tBuf [1/bclk * 16]
 * I2CG10[31:24] base clk4 for i2c auto recovery timeout counter (0xC6)
 * I2CG10[23:16] base clk3 for Standard-mode (100Khz) min tBuf 4.7us
 * 0x3c : 100.8Khz	: 3.225Mhz					  : 4.96us
 * 0x3d : 99.2Khz	: 3.174Mhz					  : 5.04us
 * 0x3e : 97.65Khz	: 3.125Mhz					  : 5.12us
 * 0x40 : 97.75Khz	: 3.03Mhz					  : 5.28us
 * 0x41 : 99.5Khz	: 2.98Mhz					  : 5.36us (default)
 * I2CG10[15:8] base clk2 for Fast-mode (400Khz) min tBuf 1.3us
 * 0x12 : 400Khz	: 10Mhz						  : 1.6us
 * I2CG10[7:0] base clk1 for Fast-mode Plus (1Mhz) min tBuf 0.5us
 * 0x08 : 1Mhz		: 20Mhz						  : 0.8us
 */
#define I2CCG_DIV_CTRL 0xC6411208

/* 0x00 : I2CC Controller/Target Function Control Register  */
#define AST2600_I2CC_FUN_CTRL		0x00
#define AST2600_I2CC_SLAVE_ADDR_RX_EN		BIT(20)
#define AST2600_I2CC_MASTER_RETRY_MASK		GENMASK(19, 18)
#define AST2600_I2CC_MASTER_RETRY(x)		(((x) & GENMASK(1, 0)) << 18)
#define AST2600_I2CC_BUS_AUTO_RELEASE		BIT(17)
#define AST2600_I2CC_M_SDA_LOCK_EN			BIT(16)
#define AST2600_I2CC_MULTI_MASTER_DIS		BIT(15)
#define AST2600_I2CC_M_SCL_DRIVE_EN			BIT(14)
#define AST2600_I2CC_MSB_STS				BIT(9)
#define AST2600_I2CC_SDA_DRIVE_1T_EN		BIT(8)
#define AST2600_I2CC_M_SDA_DRIVE_1T_EN		BIT(7)
#define AST2600_I2CC_M_HIGH_SPEED_EN		BIT(6)
/* reserver 5 : 2 */
#define AST2600_I2CC_SLAVE_EN			BIT(1)
#define AST2600_I2CC_MASTER_EN			BIT(0)

/* 0x04 : I2CC Controller/Target Clock and AC Timing Control Register #1 */
#define AST2600_I2CC_AC_TIMING		0x04
#define AST2600_I2CC_TTIMEOUT(x)			(((x) & GENMASK(4, 0)) << 24)
#define AST2600_I2CC_TCKHIGHMIN(x)			(((x) & GENMASK(3, 0)) << 20)
#define AST2600_I2CC_TCKHIGH(x)			(((x) & GENMASK(3, 0)) << 16)
#define AST2600_I2CC_TCKLOW(x)			(((x) & GENMASK(3, 0)) << 12)
#define AST2600_I2CC_THDDAT(x)			(((x) & GENMASK(1, 0)) << 10)
#define AST2600_I2CC_TOUTBASECLK(x)			(((x) & GENMASK(1, 0)) << 8)
#define AST2600_I2CC_TBASECLK(x)			((x) & GENMASK(3, 0))
#define AST2600_I2CC_AC_TIMING_MASK		GENMASK(23, 0)

/* 0x08 : I2CC Controller/Target Transmit/Receive Byte Buffer Register */
#define AST2600_I2CC_STS_AND_BUFF		0x08
#define AST2600_I2CC_TX_DIR_MASK			GENMASK(31, 29)
#define AST2600_I2CC_SDA_OE				BIT(28)
#define AST2600_I2CC_SDA_O				BIT(27)
#define AST2600_I2CC_SCL_OE				BIT(26)
#define AST2600_I2CC_SCL_O				BIT(25)

#define AST2600_I2CC_SCL_LINE_STS			BIT(18)
#define AST2600_I2CC_SDA_LINE_STS			BIT(17)
#define AST2600_I2CC_BUS_BUSY_STS			BIT(16)

#define AST2600_I2CC_GET_RX_BUFF(x)			(((x) >> 8) & GENMASK(7, 0))

/* 0x0C : I2CC Controller/Target Pool Buffer Control Register  */
#define AST2600_I2CC_BUFF_CTRL		0x0C
#define AST2600_I2CC_GET_RX_BUF_LEN(x)      (((x) & GENMASK(29, 24)) >> 24)
#define AST2600_I2CC_SET_RX_BUF_LEN(x)		(((((x) - 1) & GENMASK(4, 0)) << 16) | BIT(0))
#define AST2600_I2CC_SET_TX_BUF_LEN(x)		(((((x) - 1) & GENMASK(4, 0)) << 8) | BIT(0))
#define AST2600_I2CC_GET_TX_BUF_LEN(x)      ((((x) & GENMASK(12, 8)) >> 8) + 1)

/* 0x10 : I2CM Controller Interrupt Control Register */
#define AST2600_I2CM_IER			0x10
/* 0x14 : I2CM Controller Interrupt Status Register   : WC */
#define AST2600_I2CM_ISR			0x14

#define AST2600_I2CM_PKT_TIMEOUT			BIT(18)
#define AST2600_I2CM_PKT_ERROR			BIT(17)
#define AST2600_I2CM_PKT_DONE			BIT(16)

#define AST2600_I2CM_BUS_RECOVER_FAIL		BIT(15)
#define AST2600_I2CM_SDA_DL_TO			BIT(14)
#define AST2600_I2CM_BUS_RECOVER			BIT(13)
#define AST2600_I2CM_SMBUS_ALERT			BIT(12)

#define AST2600_I2CM_SCL_LOW_TO			BIT(6)
#define AST2600_I2CM_ABNORMAL			BIT(5)
#define AST2600_I2CM_NORMAL_STOP			BIT(4)
#define AST2600_I2CM_ARBIT_LOSS			BIT(3)
#define AST2600_I2CM_RX_DONE			BIT(2)
#define AST2600_I2CM_TX_NAK				BIT(1)
#define AST2600_I2CM_TX_ACK				BIT(0)

/* 0x18 : I2CM Controller Command/Status Register   */
#define AST2600_I2CM_CMD_STS		0x18
#define AST2600_I2CM_PKT_ADDR(x)			(((x) & GENMASK(6, 0)) << 24)
#define AST2600_I2CM_PKT_EN				BIT(16)
#define AST2600_I2CM_SDA_OE_OUT_DIR			BIT(15)
#define AST2600_I2CM_SDA_O_OUT_DIR			BIT(14)
#define AST2600_I2CM_SCL_OE_OUT_DIR			BIT(13)
#define AST2600_I2CM_SCL_O_OUT_DIR			BIT(12)
#define AST2600_I2CM_RECOVER_CMD_EN			BIT(11)

#define AST2600_I2CM_RX_DMA_EN			BIT(9)
#define AST2600_I2CM_TX_DMA_EN			BIT(8)
/* Command Bit */
#define AST2600_I2CM_RX_BUFF_EN			BIT(7)
#define AST2600_I2CM_TX_BUFF_EN			BIT(6)
#define AST2600_I2CM_STOP_CMD			BIT(5)
#define AST2600_I2CM_RX_CMD_LAST			BIT(4)
#define AST2600_I2CM_RX_CMD				BIT(3)

#define AST2600_I2CM_TX_CMD				BIT(1)
#define AST2600_I2CM_START_CMD			BIT(0)

/* 0x1C : I2CM Controller DMA Transfer Length Register	 */
#define AST2600_I2CM_DMA_LEN		0x1C
/* Tx Rx support length 1 ~ 4096 */
#define AST2600_I2CM_SET_RX_DMA_LEN(x)	((((x) & GENMASK(11, 0)) << 16) | BIT(31))
#define AST2600_I2CM_SET_TX_DMA_LEN(x)	(((x) & GENMASK(11, 0)) | BIT(15))

/* 0x20 : I2CS Target Interrupt Control Register   */
#define AST2600_I2CS_IER			0x20
/* 0x24 : I2CS Target Interrupt Status Register	 */
#define AST2600_I2CS_ISR			0x24

#define AST2600_I2CS_ADDR_INDICATE_MASK	GENMASK(31, 30)
#define AST2600_I2CS_SLAVE_PENDING			BIT(29)

#define AST2600_I2CS_WAIT_TX_DMA			BIT(25)
#define AST2600_I2CS_WAIT_RX_DMA			BIT(24)

#define AST2600_I2CS_ADDR3_NAK			BIT(22)
#define AST2600_I2CS_ADDR2_NAK			BIT(21)
#define AST2600_I2CS_ADDR1_NAK			BIT(20)

#define AST2600_I2CS_ADDR_MASK			GENMASK(19, 18)
#define AST2600_I2CS_PKT_ERROR			BIT(17)
#define AST2600_I2CS_PKT_DONE			BIT(16)
#define AST2600_I2CS_INACTIVE_TO			BIT(15)

#define AST2600_I2CS_SLAVE_MATCH			BIT(7)
#define AST2600_I2CS_ABNOR_STOP			BIT(5)
#define AST2600_I2CS_STOP				BIT(4)
#define AST2600_I2CS_RX_DONE_NAK			BIT(3)
#define AST2600_I2CS_RX_DONE			BIT(2)
#define AST2600_I2CS_TX_NAK				BIT(1)
#define AST2600_I2CS_TX_ACK				BIT(0)

/* 0x28 : I2CS Target CMD/Status Register   */
#define AST2600_I2CS_CMD_STS		0x28
#define AST2600_I2CS_ACTIVE_ALL			GENMASK(18, 17)
#define AST2600_I2CS_PKT_MODE_EN			BIT(16)
#define AST2600_I2CS_AUTO_NAK_NOADDR		BIT(15)
#define AST2600_I2CS_AUTO_NAK_EN			BIT(14)

#define AST2600_I2CS_ALT_EN				BIT(10)
#define AST2600_I2CS_RX_DMA_EN			BIT(9)
#define AST2600_I2CS_TX_DMA_EN			BIT(8)
#define AST2600_I2CS_RX_BUFF_EN			BIT(7)
#define AST2600_I2CS_TX_BUFF_EN			BIT(6)
#define AST2600_I2CS_RX_CMD_LAST			BIT(4)

#define AST2600_I2CS_TX_CMD				BIT(2)

#define AST2600_I2CS_DMA_LEN		0x2C
#define AST2600_I2CS_SET_RX_DMA_LEN(x)	(((((x) - 1) & GENMASK(11, 0)) << 16) | BIT(31))
#define AST2600_I2CS_SET_TX_DMA_LEN(x)	((((x) - 1) & GENMASK(11, 0)) | BIT(15))

/* I2CM Controller DMA Tx Buffer Register   */
#define AST2600_I2CM_TX_DMA			0x30
/* I2CM Controller DMA Rx Buffer Register	*/
#define AST2600_I2CM_RX_DMA			0x34
/* I2CS Target DMA Tx Buffer Register   */
#define AST2600_I2CS_TX_DMA			0x38
/* I2CS Target DMA Rx Buffer Register   */
#define AST2600_I2CS_RX_DMA			0x3C

#define AST2600_I2CS_ADDR_CTRL		0x40

#define	AST2600_I2CS_ADDR3_MASK		GENMASK(22, 16)
#define	AST2600_I2CS_ADDR2_MASK		GENMASK(14, 8)
#define	AST2600_I2CS_ADDR1_MASK		GENMASK(6, 0)

#define AST2600_I2CM_DMA_LEN_STS		0x48
#define AST2600_I2CS_DMA_LEN_STS		0x4C

#define AST2600_I2C_GET_TX_DMA_LEN(x)		((x) & GENMASK(12, 0))
#define AST2600_I2C_GET_RX_DMA_LEN(x)        (((x) & GENMASK(28, 16)) >> 16)

/* 0x40 : Target Device Address Register */
#define AST2600_I2CS_ADDR3_ENABLE			BIT(23)
#define AST2600_I2CS_ADDR3(x)			((x) << 16)
#define AST2600_I2CS_ADDR2_ENABLE			BIT(15)
#define AST2600_I2CS_ADDR2(x)			((x) << 8)
#define AST2600_I2CS_ADDR1_ENABLE			BIT(7)
#define AST2600_I2CS_ADDR1(x)			(x)

#define CONTROLLER_TRIGGER_LAST_STOP	(AST2600_I2CM_RX_CMD_LAST | AST2600_I2CM_STOP_CMD)
#define TARGET_TRIGGER_CMD	(AST2600_I2CS_ACTIVE_ALL | AST2600_I2CS_PKT_MODE_EN)

#define AST_I2C_TIMEOUT_CLK		0x1

struct ast2600_i2c_bus {
	struct i2c_adapter	adap;
	struct device		*dev;
	void __iomem		*reg_base;
	struct regmap		*global_regs;
	struct clk		*clk;
	struct i2c_timings	timing_info;
	struct completion	cmd_complete;
	struct i2c_msg		*msgs;
	u32			apb_clk;
	u32			timeout;
	int			irq;
	int			cmd_err;
	int			msgs_index;
	int			msgs_count;
	int			controller_xfer_cnt;
	size_t			buf_size;
	bool			multi_master;
	bool			stop_pending;
	void __iomem		*buf_base;
};

static void ast2600_i2c_ac_timing_config(struct ast2600_i2c_bus *i2c_bus)
{
	unsigned long base_clk[16];
	int baseclk_idx = 0;
	int divisor = 0;
	u32 clk_div_reg = I2CCG_DIV_CTRL;
	u32 scl_low;
	u32 scl_high;
	u32 data;

	regmap_read(i2c_bus->global_regs, AST2600_I2CG_CLK_DIV_CTRL, &clk_div_reg);

	for (int i = 0; i < ARRAY_SIZE(base_clk); i++) {
		if (i == 0)
			base_clk[i] = i2c_bus->apb_clk;
		else if (i < 5)
			base_clk[i] = (i2c_bus->apb_clk * 2) /
			   (((clk_div_reg >> ((i - 1) * 8)) & GENMASK(7, 0)) + 2);
		else
			base_clk[i] = base_clk[4] >> (i - 4);

		if ((base_clk[i] / i2c_bus->timing_info.bus_freq_hz) <= 32) {
			baseclk_idx = i;
			divisor = DIV_ROUND_UP(base_clk[i], i2c_bus->timing_info.bus_freq_hz);
			break;
		}
	}
	baseclk_idx = clamp_t(int, baseclk_idx, 0, 15);
	divisor = clamp_t(int, divisor, 2, 32);
	scl_low = clamp_t(int, divisor * 9 / 16 - 1, 0, 15);
	scl_high = clamp_t(int, divisor - scl_low - 2, 1, 15);
	data = (scl_high - 1) << 20 | scl_high << 16 | scl_low << 12 | baseclk_idx;
	if (i2c_bus->timeout) {
		data |= AST2600_I2CC_TOUTBASECLK(AST_I2C_TIMEOUT_CLK);
		data |= AST2600_I2CC_TTIMEOUT(i2c_bus->timeout);
	}

	writel(data, i2c_bus->reg_base + AST2600_I2CC_AC_TIMING);
}

static int ast2600_i2c_recover_bus(struct ast2600_i2c_bus *i2c_bus)
{
	u32 state = readl(i2c_bus->reg_base + AST2600_I2CC_STS_AND_BUFF);
	int ret = 0;
	u32 ctrl;
	int r;

	dev_dbg(i2c_bus->dev, "%d-bus recovery bus [%x]\n", i2c_bus->adap.nr, state);

	/* reset controller */
	ctrl = readl(i2c_bus->reg_base + AST2600_I2CC_FUN_CTRL);
	writel(ctrl & ~AST2600_I2CC_MASTER_EN, i2c_bus->reg_base + AST2600_I2CC_FUN_CTRL);
	writel(ctrl, i2c_bus->reg_base + AST2600_I2CC_FUN_CTRL);

	reinit_completion(&i2c_bus->cmd_complete);
	i2c_bus->cmd_err = 0;

	/* Check SDA/SCL status in the status register. */
	state = readl(i2c_bus->reg_base + AST2600_I2CC_STS_AND_BUFF);
	if (!(state & AST2600_I2CC_SDA_LINE_STS) && (state & AST2600_I2CC_SCL_LINE_STS)) {
		writel(AST2600_I2CM_RECOVER_CMD_EN, i2c_bus->reg_base + AST2600_I2CM_CMD_STS);
		r = wait_for_completion_timeout(&i2c_bus->cmd_complete, i2c_bus->adap.timeout);
		if (r == 0) {
			dev_dbg(i2c_bus->dev, "recovery timed out\n");
			writel(0, i2c_bus->reg_base + AST2600_I2CM_IER);
			synchronize_irq(i2c_bus->irq);
			writel(readl(i2c_bus->reg_base + AST2600_I2CM_ISR),
			       i2c_bus->reg_base + AST2600_I2CM_ISR);
			ctrl = readl(i2c_bus->reg_base + AST2600_I2CC_FUN_CTRL);
			writel(ctrl & ~AST2600_I2CC_MASTER_EN,
			       i2c_bus->reg_base + AST2600_I2CC_FUN_CTRL);
			writel(ctrl, i2c_bus->reg_base + AST2600_I2CC_FUN_CTRL);
			writel(AST2600_I2CM_PKT_DONE | AST2600_I2CM_BUS_RECOVER,
			       i2c_bus->reg_base + AST2600_I2CM_IER);
			return -ETIMEDOUT;
		} else if (i2c_bus->cmd_err) {
			dev_dbg(i2c_bus->dev, "recovery error\n");
			ret = -EPROTO;
		}
	}

	/* Recovery done */
	state = readl(i2c_bus->reg_base + AST2600_I2CC_STS_AND_BUFF);
	if (state & AST2600_I2CC_BUS_BUSY_STS) {
		dev_dbg(i2c_bus->dev, "Can't recover bus [%x]\n", state);
		ret = -EPROTO;
	}

	return ret;
}

static int ast2600_i2c_setup_buff_tx(u32 cmd, struct ast2600_i2c_bus *i2c_bus)
{
	struct i2c_msg *msg = &i2c_bus->msgs[i2c_bus->msgs_index];
	int xfer_len = msg->len - i2c_bus->controller_xfer_cnt;
	u32 wbuf_dword;
	int i;

	cmd |= AST2600_I2CM_PKT_EN;

	if (xfer_len > i2c_bus->buf_size)
		xfer_len = i2c_bus->buf_size;
	else if (i2c_bus->msgs_index + 1 == i2c_bus->msgs_count)
		cmd |= AST2600_I2CM_STOP_CMD;

	if (cmd & AST2600_I2CM_START_CMD)
		cmd |= AST2600_I2CM_PKT_ADDR(msg->addr);

	if (xfer_len) {
		cmd |= AST2600_I2CM_TX_BUFF_EN | AST2600_I2CM_TX_CMD;
		/*
		 * The controller's buffer register supports dword writes only.
		 * Therefore, write dwords to the buffer register in a 4-byte aligned,
		 * and write the remaining unaligned data at the end.
		 */
		for (i = 0; i < xfer_len; i += 4) {
			int xfer_cnt = i2c_bus->controller_xfer_cnt + i;

			switch (min(xfer_len - i, 4) % 4) {
			case 1:
				wbuf_dword = msg->buf[xfer_cnt];
				break;
			case 2:
				wbuf_dword = get_unaligned_le16(&msg->buf[xfer_cnt]);
				break;
			case 3:
				wbuf_dword = get_unaligned_le24(&msg->buf[xfer_cnt]);
				break;
			default:
				wbuf_dword = get_unaligned_le32(&msg->buf[xfer_cnt]);
				break;
			}
			writel(wbuf_dword, i2c_bus->buf_base + i);
		}
		writel(AST2600_I2CC_SET_TX_BUF_LEN(xfer_len),
		       i2c_bus->reg_base + AST2600_I2CC_BUFF_CTRL);
	}

	writel(cmd, i2c_bus->reg_base + AST2600_I2CM_CMD_STS);

	return 0;
}

static int ast2600_i2c_setup_buff_rx(u32 cmd, struct ast2600_i2c_bus *i2c_bus)
{
	struct i2c_msg *msg = &i2c_bus->msgs[i2c_bus->msgs_index];
	int xfer_len = msg->len - i2c_bus->controller_xfer_cnt;

	cmd |= AST2600_I2CM_PKT_EN | AST2600_I2CM_RX_BUFF_EN | AST2600_I2CM_RX_CMD;

	if (cmd & AST2600_I2CM_START_CMD)
		cmd |= AST2600_I2CM_PKT_ADDR(msg->addr);

	if (msg->flags & I2C_M_RECV_LEN) {
		dev_dbg(i2c_bus->dev, "smbus read\n");
		xfer_len = 1;
	} else if (xfer_len > i2c_bus->buf_size) {
		xfer_len = i2c_bus->buf_size;
	} else {
		cmd |= AST2600_I2CM_RX_CMD_LAST;
		if (i2c_bus->msgs_index + 1 == i2c_bus->msgs_count)
			cmd |= AST2600_I2CM_STOP_CMD;
	}

	if (xfer_len <= 0)
		return -EINVAL;

	writel(AST2600_I2CC_SET_RX_BUF_LEN(xfer_len), i2c_bus->reg_base + AST2600_I2CC_BUFF_CTRL);

	writel(cmd, i2c_bus->reg_base + AST2600_I2CM_CMD_STS);

	return 0;
}

static int ast2600_i2c_do_start(struct ast2600_i2c_bus *i2c_bus)
{
	struct i2c_msg *msg = &i2c_bus->msgs[i2c_bus->msgs_index];

	/* send start */
	dev_dbg(i2c_bus->dev, "[%d] %s %d byte%s %s 0x%02x\n",
		i2c_bus->msgs_index, str_read_write(msg->flags & I2C_M_RD),
		msg->len, str_plural(msg->len),
		msg->flags & I2C_M_RD ? "from" : "to", msg->addr);

	i2c_bus->controller_xfer_cnt = 0;

	if (msg->flags & I2C_M_RD)
		return ast2600_i2c_setup_buff_rx(AST2600_I2CM_START_CMD, i2c_bus);

	return ast2600_i2c_setup_buff_tx(AST2600_I2CM_START_CMD, i2c_bus);
}

static int ast2600_i2c_irq_err_to_errno(u32 irq_status)
{
	if (irq_status & AST2600_I2CM_ARBIT_LOSS)
		return -EAGAIN;
	if (irq_status & (AST2600_I2CM_SDA_DL_TO | AST2600_I2CM_SCL_LOW_TO))
		return -ETIMEDOUT;
	if (irq_status & (AST2600_I2CM_ABNORMAL))
		return -EPROTO;

	return 0;
}

static int ast2600_i2c_clamp_len(struct ast2600_i2c_bus *i2c_bus,
				 struct i2c_msg *msg, int len)
{
	int remaining = msg->len - i2c_bus->controller_xfer_cnt;

	if (len > i2c_bus->buf_size)
		len = i2c_bus->buf_size;
	if (remaining < 0)
		remaining = 0;
	if (len > remaining)
		len = remaining;
	return len;
}

static int ast2600_i2c_wait_stop(struct ast2600_i2c_bus *i2c_bus)
{
	u32 sts;
	int ret;

	ret = readl_poll_timeout(i2c_bus->reg_base + AST2600_I2CM_ISR, sts,
				 sts & AST2600_I2CM_NORMAL_STOP, 1000, 10000);
	if (ret)
		return ret;

	writel(AST2600_I2CM_NORMAL_STOP, i2c_bus->reg_base + AST2600_I2CM_ISR);

	return 0;
}

static void ast2600_i2c_controller_packet_irq(struct ast2600_i2c_bus *i2c_bus, u32 sts)
{
	struct i2c_msg *msg;
	int xfer_len;
	int i;

	if (!i2c_bus->msgs) {
		writel(AST2600_I2CM_PKT_DONE, i2c_bus->reg_base + AST2600_I2CM_ISR);
		return;
	}
	if (i2c_bus->msgs_index >= i2c_bus->msgs_count) {
		writel(AST2600_I2CM_PKT_DONE, i2c_bus->reg_base + AST2600_I2CM_ISR);
		return;
	}
	msg = &i2c_bus->msgs[i2c_bus->msgs_index];

	sts &= ~AST2600_I2CM_PKT_DONE;
	writel(AST2600_I2CM_PKT_DONE, i2c_bus->reg_base + AST2600_I2CM_ISR);
	switch (sts) {
	case AST2600_I2CM_PKT_ERROR:
		WRITE_ONCE(i2c_bus->msgs, NULL);
		i2c_bus->cmd_err = -EAGAIN;
		complete(&i2c_bus->cmd_complete);
		break;
	case AST2600_I2CM_PKT_ERROR | AST2600_I2CM_TX_NAK: /* a0 fix for issue */
		fallthrough;
	case AST2600_I2CM_PKT_ERROR | AST2600_I2CM_TX_NAK | AST2600_I2CM_NORMAL_STOP:
		WRITE_ONCE(i2c_bus->msgs, NULL);
		i2c_bus->cmd_err = -ENXIO;
		complete(&i2c_bus->cmd_complete);
		break;
	case AST2600_I2CM_NORMAL_STOP:
		/* write 0 byte only have stop isr */
		i2c_bus->msgs_index++;
		if (i2c_bus->msgs_index < i2c_bus->msgs_count) {
			if (ast2600_i2c_do_start(i2c_bus)) {
				WRITE_ONCE(i2c_bus->msgs, NULL);
				i2c_bus->cmd_err = -EBUSY;
				complete(&i2c_bus->cmd_complete);
			}
		} else {
			WRITE_ONCE(i2c_bus->msgs, NULL);
			i2c_bus->cmd_err = i2c_bus->msgs_index;
			complete(&i2c_bus->cmd_complete);
		}
		break;
	case AST2600_I2CM_TX_ACK:
	case AST2600_I2CM_TX_ACK | AST2600_I2CM_NORMAL_STOP:
		xfer_len = AST2600_I2CC_GET_TX_BUF_LEN(readl(i2c_bus->reg_base +
						       AST2600_I2CC_BUFF_CTRL));
		xfer_len = ast2600_i2c_clamp_len(i2c_bus, msg, xfer_len);
		i2c_bus->controller_xfer_cnt += xfer_len;

		if (i2c_bus->controller_xfer_cnt == msg->len) {
			i2c_bus->msgs_index++;
			if (i2c_bus->msgs_index == i2c_bus->msgs_count) {
				WRITE_ONCE(i2c_bus->msgs, NULL);
				i2c_bus->cmd_err = i2c_bus->msgs_index;
				complete(&i2c_bus->cmd_complete);
			} else {
				if (ast2600_i2c_do_start(i2c_bus)) {
					WRITE_ONCE(i2c_bus->msgs, NULL);
					i2c_bus->cmd_err = -EBUSY;
					complete(&i2c_bus->cmd_complete);
				}
			}
		} else {
			ast2600_i2c_setup_buff_tx(0, i2c_bus);
		}
		break;
	case AST2600_I2CM_RX_DONE:
	case AST2600_I2CM_RX_DONE | AST2600_I2CM_NORMAL_STOP:
		xfer_len = AST2600_I2CC_GET_RX_BUF_LEN(readl(i2c_bus->reg_base +
							     AST2600_I2CC_BUFF_CTRL));
		xfer_len = ast2600_i2c_clamp_len(i2c_bus, msg, xfer_len);
		for (i = 0; i < xfer_len; i++)
			msg->buf[i2c_bus->controller_xfer_cnt + i] =
				readb(i2c_bus->buf_base + i2c_bus->buf_size + i);

		if (msg->flags & I2C_M_RECV_LEN) {
			u8 recv_len = AST2600_I2CC_GET_RX_BUFF(readl(i2c_bus->reg_base
					       + AST2600_I2CC_STS_AND_BUFF));

			msg->len = min_t(unsigned int, recv_len, I2C_SMBUS_BLOCK_MAX);
			msg->len += ((msg->flags & I2C_CLIENT_PEC) ? 2 : 1);
			msg->flags &= ~I2C_M_RECV_LEN;
			if (!recv_len) {
				/*
				 * Workaround: a standalone STOP triggers NORMAL_STOP in
				 * ISR without generating another IRQ, so poll for it from
				 * process context.
				 */
				i2c_bus->controller_xfer_cnt = msg->len;
				WRITE_ONCE(i2c_bus->stop_pending, true);
				writel(CONTROLLER_TRIGGER_LAST_STOP,
				       i2c_bus->reg_base + AST2600_I2CM_CMD_STS);

				WRITE_ONCE(i2c_bus->msgs, NULL);
				i2c_bus->cmd_err = -EPROTO;
				complete(&i2c_bus->cmd_complete);
				break;
			}
			i2c_bus->controller_xfer_cnt = 1;
		} else {
			i2c_bus->controller_xfer_cnt += xfer_len;
		}

		if (i2c_bus->controller_xfer_cnt == msg->len) {
			i2c_bus->msgs_index++;
			if (i2c_bus->msgs_index == i2c_bus->msgs_count) {
				WRITE_ONCE(i2c_bus->msgs, NULL);
				i2c_bus->cmd_err = i2c_bus->msgs_index;
				complete(&i2c_bus->cmd_complete);
			} else {
				if (ast2600_i2c_do_start(i2c_bus)) {
					WRITE_ONCE(i2c_bus->msgs, NULL);
					i2c_bus->cmd_err = -EBUSY;
					complete(&i2c_bus->cmd_complete);
				}
			}
		} else if (ast2600_i2c_setup_buff_rx(0, i2c_bus)) {
			WRITE_ONCE(i2c_bus->msgs, NULL);
			i2c_bus->cmd_err = -EINVAL;
			complete(&i2c_bus->cmd_complete);
		}
		break;
	default:
		dev_dbg(i2c_bus->dev, "unhandled sts %x\n", sts);
		break;
	}
}

static int ast2600_i2c_controller_irq(struct ast2600_i2c_bus *i2c_bus)
{
	u32 sts = readl(i2c_bus->reg_base + AST2600_I2CM_ISR);
	u32 ctrl;

	sts &= ~AST2600_I2CM_SMBUS_ALERT;

	if (sts & AST2600_I2CM_BUS_RECOVER_FAIL) {
		writel(AST2600_I2CM_BUS_RECOVER_FAIL, i2c_bus->reg_base + AST2600_I2CM_ISR);
		ctrl = readl(i2c_bus->reg_base + AST2600_I2CC_FUN_CTRL);
		writel(0, i2c_bus->reg_base + AST2600_I2CC_FUN_CTRL);
		writel(ctrl, i2c_bus->reg_base + AST2600_I2CC_FUN_CTRL);
		WRITE_ONCE(i2c_bus->msgs, NULL);
		i2c_bus->cmd_err = -EPROTO;
		complete(&i2c_bus->cmd_complete);
		return 1;
	}

	if (sts & AST2600_I2CM_BUS_RECOVER) {
		writel(AST2600_I2CM_BUS_RECOVER, i2c_bus->reg_base + AST2600_I2CM_ISR);
		WRITE_ONCE(i2c_bus->msgs, NULL);
		i2c_bus->cmd_err = 0;
		complete(&i2c_bus->cmd_complete);
		return 1;
	}

	i2c_bus->cmd_err = ast2600_i2c_irq_err_to_errno(sts);
	if (i2c_bus->cmd_err) {
		WRITE_ONCE(i2c_bus->msgs, NULL);
		writel(AST2600_I2CM_PKT_DONE, i2c_bus->reg_base + AST2600_I2CM_ISR);
		complete(&i2c_bus->cmd_complete);
		return 1;
	}

	if (sts & AST2600_I2CM_PKT_DONE) {
		ast2600_i2c_controller_packet_irq(i2c_bus, sts);
		return 1;
	}

	return 0;
}

static irqreturn_t ast2600_i2c_bus_irq(int irq, void *dev_id)
{
	struct ast2600_i2c_bus *i2c_bus = dev_id;

	return IRQ_RETVAL(ast2600_i2c_controller_irq(i2c_bus));
}

static int ast2600_i2c_controller_xfer(struct i2c_adapter *adap, struct i2c_msg *msgs, int num)
{
	struct ast2600_i2c_bus *i2c_bus = i2c_get_adapdata(adap);
	unsigned long timeout;
	int ret;

	if (!i2c_bus->multi_master &&
	    (readl(i2c_bus->reg_base + AST2600_I2CC_STS_AND_BUFF) & AST2600_I2CC_BUS_BUSY_STS)) {
		ret = ast2600_i2c_recover_bus(i2c_bus);
		if (ret)
			return ret;
	}

	i2c_bus->cmd_err = 0;
	i2c_bus->msgs = msgs;
	i2c_bus->msgs_index = 0;
	i2c_bus->msgs_count = num;
	WRITE_ONCE(i2c_bus->stop_pending, false);
	reinit_completion(&i2c_bus->cmd_complete);
	ret = ast2600_i2c_do_start(i2c_bus);
	if (ret)
		goto controller_out;
	timeout = wait_for_completion_timeout(&i2c_bus->cmd_complete, i2c_bus->adap.timeout);
	if (timeout == 0) {
		u32 ctrl = readl(i2c_bus->reg_base + AST2600_I2CC_FUN_CTRL);

		dev_dbg(i2c_bus->dev, "timeout isr[%x], sts[%x]\n",
			readl(i2c_bus->reg_base + AST2600_I2CM_ISR),
			readl(i2c_bus->reg_base + AST2600_I2CC_STS_AND_BUFF));

		writel(0, i2c_bus->reg_base + AST2600_I2CM_IER);
		synchronize_irq(i2c_bus->irq);
		writel(readl(i2c_bus->reg_base + AST2600_I2CM_ISR),
		       i2c_bus->reg_base + AST2600_I2CM_ISR);

		writel(ctrl & ~AST2600_I2CC_MASTER_EN, i2c_bus->reg_base + AST2600_I2CC_FUN_CTRL);
		writel(ctrl, i2c_bus->reg_base + AST2600_I2CC_FUN_CTRL);

		WRITE_ONCE(i2c_bus->msgs, NULL);
		writel(AST2600_I2CM_PKT_DONE | AST2600_I2CM_BUS_RECOVER,
		       i2c_bus->reg_base + AST2600_I2CM_IER);

		/*
		 * A slave holding SCL low can stall the transfer and trigger
		 * a master timeout. In multi-master mode, attempt bus recovery
		 * if the bus is still busy.
		 */
		if (i2c_bus->multi_master &&
		    (readl(i2c_bus->reg_base + AST2600_I2CC_STS_AND_BUFF) &
		    AST2600_I2CC_BUS_BUSY_STS))
			ast2600_i2c_recover_bus(i2c_bus);
		ret = -ETIMEDOUT;
	} else {
		ret = i2c_bus->cmd_err;
	}

	if (READ_ONCE(i2c_bus->stop_pending)) {
		int stop_ret;

		stop_ret = ast2600_i2c_wait_stop(i2c_bus);
		WRITE_ONCE(i2c_bus->stop_pending, false);
		WRITE_ONCE(i2c_bus->msgs, NULL);
		if (stop_ret) {
			i2c_bus->cmd_err = stop_ret;
			ret = stop_ret;
		}
	}

	dev_dbg(i2c_bus->dev, "bus%d-m: %d end\n", i2c_bus->adap.nr, i2c_bus->cmd_err);

controller_out:
	i2c_bus->msgs = NULL;
	return ret;
}

static int ast2600_i2c_init(struct ast2600_i2c_bus *i2c_bus)
{
	u32 fun_ctrl = AST2600_I2CC_BUS_AUTO_RELEASE | AST2600_I2CC_MASTER_EN;

	/* I2C Reset */
	writel(0, i2c_bus->reg_base + AST2600_I2CC_FUN_CTRL);

	if (!i2c_bus->multi_master)
		fun_ctrl |= AST2600_I2CC_MULTI_MASTER_DIS;

	/* Enable Controller Mode */
	writel(fun_ctrl, i2c_bus->reg_base + AST2600_I2CC_FUN_CTRL);
	/* disable target address */
	writel(0, i2c_bus->reg_base + AST2600_I2CS_ADDR_CTRL);

	/* Set AC Timing */
	ast2600_i2c_ac_timing_config(i2c_bus);

	/* Clear Interrupt */
	writel(GENMASK(27, 0), i2c_bus->reg_base + AST2600_I2CM_ISR);

	return 0;
}

static u32 ast2600_i2c_functionality(struct i2c_adapter *adap)
{
	return I2C_FUNC_I2C | I2C_FUNC_SMBUS_EMUL | I2C_FUNC_SMBUS_BLOCK_DATA;
}

static const struct i2c_algorithm i2c_ast2600_algorithm = {
	.xfer = ast2600_i2c_controller_xfer,
	.functionality = ast2600_i2c_functionality,
};

static const struct i2c_adapter_quirks ast2600_i2c_quirks = {
	.flags = I2C_AQ_NO_ZERO_LEN_READ,
};

static int ast2600_i2c_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct ast2600_i2c_bus *i2c_bus;
	void __iomem *buf_base;
	struct reset_control *rst;
	struct resource *res;
	u32 global_ctrl = 0;
	int ret;

	if (!device_property_present(dev, "aspeed,global-regs"))
		return -ENODEV;

	i2c_bus = devm_kzalloc(dev, sizeof(*i2c_bus), GFP_KERNEL);
	if (!i2c_bus)
		return -ENOMEM;

	i2c_bus->reg_base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(i2c_bus->reg_base))
		return PTR_ERR(i2c_bus->reg_base);

	rst = devm_reset_control_get_shared_deasserted(dev, NULL);
	if (IS_ERR(rst))
		return dev_err_probe(dev, PTR_ERR(rst), "Missing reset ctrl\n");

	i2c_bus->global_regs =
		syscon_regmap_lookup_by_phandle(dev_of_node(dev), "aspeed,global-regs");
	if (IS_ERR(i2c_bus->global_regs))
		return PTR_ERR(i2c_bus->global_regs);

	regmap_read(i2c_bus->global_regs, AST2600_I2CG_CTRL, &global_ctrl);
	if ((global_ctrl & AST2600_GLOBAL_INIT) != AST2600_GLOBAL_INIT) {
		regmap_update_bits(i2c_bus->global_regs, AST2600_I2CG_CTRL,
				   AST2600_GLOBAL_INIT, AST2600_GLOBAL_INIT);
		regmap_write(i2c_bus->global_regs, AST2600_I2CG_CLK_DIV_CTRL, I2CCG_DIV_CTRL);
	}

	i2c_bus->dev = dev;
	i2c_bus->multi_master = device_property_read_bool(dev, "multi-master");

	buf_base = devm_platform_get_and_ioremap_resource(pdev, 1, &res);
	if (IS_ERR(buf_base))
		return dev_err_probe(dev, PTR_ERR(buf_base), "Missing buffer resource\n");
	i2c_bus->buf_base = buf_base;
	i2c_bus->buf_size = resource_size(res) / 2;

	/*
	 * i2c timeout counter: use base clk4 1Mhz,
	 * per unit: 1/(1000/1024) = 1024us
	 */
	ret = device_property_read_u32(dev, "i2c-scl-clk-low-timeout-us", &i2c_bus->timeout);
	if (!ret) {
		i2c_bus->timeout = DIV_ROUND_UP(i2c_bus->timeout, 1024);
		if (i2c_bus->timeout > GENMASK(4, 0)) {
			dev_warn(dev,
				 "i2c-scl-clk-low-timeout-us exceeds HW max (31 * 1024us), clamped\n");
			i2c_bus->timeout = GENMASK(4, 0);
		}
	}

	init_completion(&i2c_bus->cmd_complete);

	i2c_bus->irq = platform_get_irq(pdev, 0);
	if (i2c_bus->irq < 0)
		return i2c_bus->irq;

	platform_set_drvdata(pdev, i2c_bus);

	i2c_bus->clk = devm_clk_get(i2c_bus->dev, NULL);
	if (IS_ERR(i2c_bus->clk))
		return dev_err_probe(i2c_bus->dev, PTR_ERR(i2c_bus->clk), "Can't get clock\n");

	i2c_bus->apb_clk = clk_get_rate(i2c_bus->clk);

	i2c_parse_fw_timings(i2c_bus->dev, &i2c_bus->timing_info, true);
	if (!i2c_bus->timing_info.bus_freq_hz) {
		dev_warn(dev, "invalid clock-frequency 0, using default 100kHz\n");
		i2c_bus->timing_info.bus_freq_hz = I2C_MAX_STANDARD_MODE_FREQ;
	}

	/* Initialize the I2C adapter */
	i2c_bus->adap.owner = THIS_MODULE;
	i2c_bus->adap.algo = &i2c_ast2600_algorithm;
	i2c_bus->adap.quirks = &ast2600_i2c_quirks;
	i2c_bus->adap.retries = 0;
	i2c_bus->adap.dev.parent = i2c_bus->dev;
	device_set_node(&i2c_bus->adap.dev, dev_fwnode(dev));
	strscpy(i2c_bus->adap.name, pdev->name);
	i2c_set_adapdata(&i2c_bus->adap, i2c_bus);

	ret = ast2600_i2c_init(i2c_bus);
	if (ret < 0)
		return dev_err_probe(dev, ret, "Unable to initialize i2c %d\n", ret);

	ret = devm_request_irq(dev, i2c_bus->irq, ast2600_i2c_bus_irq, 0,
			       dev_name(dev), i2c_bus);
	if (ret < 0) {
		ret = dev_err_probe(dev, ret, "Unable to request irq %d\n",
				    i2c_bus->irq);
		goto err;
	}

	writel(AST2600_I2CM_PKT_DONE | AST2600_I2CM_BUS_RECOVER,
	       i2c_bus->reg_base + AST2600_I2CM_IER);

	ret = i2c_add_adapter(&i2c_bus->adap);
	if (ret)
		goto err;

	return 0;

err:
	writel(0, i2c_bus->reg_base + AST2600_I2CC_FUN_CTRL);
	writel(0, i2c_bus->reg_base + AST2600_I2CM_IER);
	return ret;
}

static void ast2600_i2c_remove(struct platform_device *pdev)
{
	struct ast2600_i2c_bus *i2c_bus = platform_get_drvdata(pdev);

	i2c_del_adapter(&i2c_bus->adap);

	/* Disable everything. */
	writel(0, i2c_bus->reg_base + AST2600_I2CC_FUN_CTRL);
	writel(0, i2c_bus->reg_base + AST2600_I2CM_IER);
}

static const struct of_device_id ast2600_i2c_of_match[] = {
	{ .compatible = "aspeed,ast2600-i2c-bus" },
	{ }
};
MODULE_DEVICE_TABLE(of, ast2600_i2c_of_match);

static struct platform_driver ast2600_i2c_driver = {
	.probe		= ast2600_i2c_probe,
	.remove		= ast2600_i2c_remove,
	.driver		= {
		.name		= "ast2600-i2c",
		.of_match_table	= ast2600_i2c_of_match,
	},
};
module_platform_driver(ast2600_i2c_driver);

MODULE_AUTHOR("Ryan Chen <ryan_chen@aspeedtech.com>");
MODULE_DESCRIPTION("ASPEED AST2600 I2C Controller Driver");
MODULE_LICENSE("GPL");
