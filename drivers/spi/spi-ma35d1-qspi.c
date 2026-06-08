// SPDX-License-Identifier: GPL-2.0-or-later
//
// Nuvoton MA35D1 QSPI controller driver
//
// Copyright (c) 2026 Nuvoton Technology Corp.
// Author: Chi-Wen Weng <cwweng@nuvoton.com>

#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/clk.h>
#include <linux/device.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/reset.h>
#include <linux/spi/spi.h>
#include <linux/spi/spi-mem.h>

/* Register offset definitions */
#define NUVOTON_QSPI_CTL_OFFSET		0x00 /* Control Register, RW */
#define NUVOTON_QSPI_CLKDIV_OFFSET	0x04 /* Clock Divider Register, RW */
#define NUVOTON_QSPI_SSCTL_OFFSET	0x08 /* Slave Select Register, RW */
#define NUVOTON_QSPI_FIFOCTL_OFFSET	0x10 /* FIFO Control Register, RW */
#define NUVOTON_QSPI_STATUS_OFFSET	0x14 /* Status Register, RW */
#define NUVOTON_QSPI_TX_OFFSET		0x20 /* Data Transmit Register, WO */
#define NUVOTON_QSPI_RX_OFFSET		0x30 /* Data Receive Register, RO */

/* QSPI Control Register bit masks */
#define NUVOTON_QSPI_CTL_QUADIOEN_MASK	BIT(22) /* Quad I/O Mode Enable */
#define NUVOTON_QSPI_CTL_DUALIOEN_MASK	BIT(21) /* Dual I/O Mode Enable */
#define NUVOTON_QSPI_CTL_DATDIR_MASK	BIT(20) /* Data Port Direction Control */
#define NUVOTON_QSPI_CTL_REORDER_MASK	BIT(19) /* Byte Reorder Function Enable */
#define NUVOTON_QSPI_CTL_LSB_MASK	BIT(13) /* Send LSB First */
#define NUVOTON_QSPI_CTL_DWIDTH_MASK	GENMASK(12, 8) /* Data Width */
#define NUVOTON_QSPI_CTL_SUSPITV_MASK	GENMASK(7, 4) /* Suspend Interval */
#define NUVOTON_QSPI_CTL_CLKPOL_MASK	BIT(3) /* Clock Polarity */
#define NUVOTON_QSPI_CTL_TXNEG_MASK	BIT(2) /* Transmit on Negative Edge */
#define NUVOTON_QSPI_CTL_RXNEG_MASK	BIT(1) /* Receive on Negative Edge */
#define NUVOTON_QSPI_CTL_SPIEN_MASK	BIT(0) /* QSPI Transfer Control Enable */

/* QSPI Clock Divider Register bit masks */
#define NUVOTON_QSPI_CLKDIV_MASK	GENMASK(8, 0) /* Clock Divider */

/* QSPI Slave Select Control Register bit masks */
#define NUVOTON_QSPI_SSCTL_SSACTPOL_MASK	BIT(2) /* Slave Selection Active Polarity */
#define NUVOTON_QSPI_SSCTL_SS1_MASK	BIT(1) /* Slave Selection 1 Control */
#define NUVOTON_QSPI_SSCTL_SS0_MASK	BIT(0) /* Slave Selection 0 Control */

/* QSPI FIFO Control Register bit masks */
#define NUVOTON_QSPI_FIFOCTL_TXRST_MASK	BIT(1) /* Transmit Reset */
#define NUVOTON_QSPI_FIFOCTL_RXRST_MASK	BIT(0) /* Receive Reset */

/* QSPI Status Register bit masks */
#define NUVOTON_QSPI_STATUS_TXRXRST_MASK	BIT(23) /* TX or RX Reset Status */
#define NUVOTON_QSPI_STATUS_TXFULL_MASK	BIT(17) /* Transmit FIFO Full */
#define NUVOTON_QSPI_STATUS_TXEMPTY_MASK	BIT(16) /* Transmit FIFO Empty */
#define NUVOTON_QSPI_STATUS_SPIENSTS_MASK	BIT(15) /* QSPI Enable Status */
#define NUVOTON_QSPI_STATUS_RXFULL_MASK	BIT(9) /* Receive FIFO Full */
#define NUVOTON_QSPI_STATUS_RXEMPTY_MASK	BIT(8) /* Receive FIFO Empty */
#define NUVOTON_QSPI_STATUS_UNITIF_MASK	BIT(1) /* Unit Transfer Interrupt Flag */
#define NUVOTON_QSPI_STATUS_BUSY_MASK	BIT(0) /* Busy Status */

#define NUVOTON_QSPI_DEFAULT_NUM_CS	2
#define NUVOTON_QSPI_DEFAULT_BPW	8
#define NUVOTON_QSPI_TIMEOUT_US		10000

struct nuvoton_qspi {
	void __iomem *regs;
	struct clk *clk;
	struct device *dev;
	u32 speed_hz;
	u8 bits_per_word;
};

static u32 nuvoton_qspi_read(struct nuvoton_qspi *qspi, u32 reg)
{
	return readl(qspi->regs + reg);
}

static void nuvoton_qspi_write(struct nuvoton_qspi *qspi, u32 val, u32 reg)
{
	writel(val, qspi->regs + reg);
}

static void nuvoton_qspi_update_bits(struct nuvoton_qspi *qspi, u32 reg,
				     u32 mask, u32 val)
{
	u32 tmp;

	tmp = nuvoton_qspi_read(qspi, reg);
	tmp &= ~mask;
	tmp |= val & mask;
	nuvoton_qspi_write(qspi, tmp, reg);
}

static int nuvoton_qspi_wait_ready(struct nuvoton_qspi *qspi)
{
	u32 val;

	return readl_poll_timeout(qspi->regs + NUVOTON_QSPI_STATUS_OFFSET,
					 val,
					 !(val & NUVOTON_QSPI_STATUS_BUSY_MASK),
					 0, NUVOTON_QSPI_TIMEOUT_US);
}

static int nuvoton_qspi_reset_fifo(struct nuvoton_qspi *qspi)
{
	u32 val;

	val = nuvoton_qspi_read(qspi, NUVOTON_QSPI_FIFOCTL_OFFSET);
	val |= NUVOTON_QSPI_FIFOCTL_TXRST_MASK |
	       NUVOTON_QSPI_FIFOCTL_RXRST_MASK;
	nuvoton_qspi_write(qspi, val, NUVOTON_QSPI_FIFOCTL_OFFSET);

	/* FIFO reset is extremely fast, safe to keep atomic for this micro-wait */
	return readl_poll_timeout_atomic(qspi->regs + NUVOTON_QSPI_STATUS_OFFSET,
					 val,
					 !(val & NUVOTON_QSPI_STATUS_TXRXRST_MASK),
					 1, NUVOTON_QSPI_TIMEOUT_US);
}

static int nuvoton_qspi_set_speed(struct nuvoton_qspi *qspi, u32 speed_hz)
{
	unsigned long clk_rate;
	u32 div;

	if (!speed_hz)
		return -EINVAL;

	if (qspi->speed_hz == speed_hz)
		return 0;

	clk_rate = clk_get_rate(qspi->clk);
	if (!clk_rate) {
		dev_err(qspi->dev, "failed to get clock rate\n");
		return -EINVAL;
	}

	div = DIV_ROUND_UP(clk_rate, speed_hz) - 1;
	if (div > FIELD_MAX(NUVOTON_QSPI_CLKDIV_MASK)) {
		dev_err(qspi->dev, "unsupported SPI clock %u Hz\n", speed_hz);
		return -EINVAL;
	}

	nuvoton_qspi_write(qspi, FIELD_PREP(NUVOTON_QSPI_CLKDIV_MASK, div),
			   NUVOTON_QSPI_CLKDIV_OFFSET);
	qspi->speed_hz = speed_hz;

	return 0;
}

static int nuvoton_qspi_set_bits_per_word(struct nuvoton_qspi *qspi, u8 bpw)
{
	u32 val;

	if (bpw != 8 && bpw != 16 && bpw != 32)
		return -EINVAL;

	if (bpw == 32)
		val = NUVOTON_QSPI_CTL_REORDER_MASK;
	else
		val = FIELD_PREP(NUVOTON_QSPI_CTL_DWIDTH_MASK, bpw);

	nuvoton_qspi_update_bits(qspi, NUVOTON_QSPI_CTL_OFFSET,
				 NUVOTON_QSPI_CTL_DWIDTH_MASK |
				 NUVOTON_QSPI_CTL_REORDER_MASK, val);
	qspi->bits_per_word = bpw;

	return 0;
}

static int nuvoton_qspi_setup_transfer(struct spi_device *spi,
				       u32 speed_hz, u8 bpw)
{
	struct nuvoton_qspi *qspi = spi_controller_get_devdata(spi->controller);
	u32 mode = spi->mode & SPI_MODE_X_MASK;
	u32 ctl = 0;
	int ret;

	if (!speed_hz)
		speed_hz = spi->max_speed_hz;

	if (!bpw)
		bpw = NUVOTON_QSPI_DEFAULT_BPW;

	ret = nuvoton_qspi_set_speed(qspi, speed_hz);
	if (ret)
		return ret;

	ret = nuvoton_qspi_set_bits_per_word(qspi, bpw);
	if (ret)
		return ret;

	if (mode == SPI_MODE_0 || mode == SPI_MODE_3)
		ctl |= NUVOTON_QSPI_CTL_TXNEG_MASK;
	else
		ctl |= NUVOTON_QSPI_CTL_RXNEG_MASK;

	if (spi->mode & SPI_CPOL)
		ctl |= NUVOTON_QSPI_CTL_CLKPOL_MASK;

	if (spi->mode & SPI_LSB_FIRST)
		ctl |= NUVOTON_QSPI_CTL_LSB_MASK;

	nuvoton_qspi_update_bits(qspi, NUVOTON_QSPI_CTL_OFFSET,
				 NUVOTON_QSPI_CTL_TXNEG_MASK |
				 NUVOTON_QSPI_CTL_RXNEG_MASK |
				 NUVOTON_QSPI_CTL_CLKPOL_MASK |
				 NUVOTON_QSPI_CTL_LSB_MASK, ctl);

	return 0;
}

static void nuvoton_qspi_set_bus_width(struct nuvoton_qspi *qspi,
				       unsigned int buswidth,
				       enum spi_mem_data_dir dir)
{
	u32 ctl = 0;

	if (buswidth == 4)
		ctl |= NUVOTON_QSPI_CTL_QUADIOEN_MASK;
	else if (buswidth == 2)
		ctl |= NUVOTON_QSPI_CTL_DUALIOEN_MASK;

	if (buswidth > 1 && dir == SPI_MEM_DATA_OUT)
		ctl |= NUVOTON_QSPI_CTL_DATDIR_MASK;

	nuvoton_qspi_update_bits(qspi, NUVOTON_QSPI_CTL_OFFSET,
				 NUVOTON_QSPI_CTL_QUADIOEN_MASK |
				 NUVOTON_QSPI_CTL_DUALIOEN_MASK |
				 NUVOTON_QSPI_CTL_DATDIR_MASK, ctl);
}

static u32 nuvoton_qspi_tx_word(const void *txbuf, unsigned int idx, u8 bpw)
{
	if (!txbuf)
		return 0;

	if (bpw <= 8)
		return ((const u8 *)txbuf)[idx];
	if (bpw <= 16)
		return ((const u16 *)txbuf)[idx];

	return ((const u32 *)txbuf)[idx];
}

static void nuvoton_qspi_rx_word(void *rxbuf, unsigned int idx, u32 val, u8 bpw)
{
	if (!rxbuf)
		return;

	if (bpw <= 8)
		((u8 *)rxbuf)[idx] = val;
	else if (bpw <= 16)
		((u16 *)rxbuf)[idx] = val;
	else
		((u32 *)rxbuf)[idx] = val;
}

static int nuvoton_qspi_wait_tx_not_full(struct nuvoton_qspi *qspi)
{
	u32 val;

	return readl_poll_timeout_atomic(qspi->regs + NUVOTON_QSPI_STATUS_OFFSET,
					 val,
					 !(val & NUVOTON_QSPI_STATUS_TXFULL_MASK),
					 0, NUVOTON_QSPI_TIMEOUT_US);
}

static int nuvoton_qspi_wait_rx_not_empty(struct nuvoton_qspi *qspi)
{
	u32 val;

	return readl_poll_timeout_atomic(qspi->regs + NUVOTON_QSPI_STATUS_OFFSET,
					 val,
					 !(val & NUVOTON_QSPI_STATUS_RXEMPTY_MASK),
					 0, NUVOTON_QSPI_TIMEOUT_US);
}

static int nuvoton_qspi_txrx(struct nuvoton_qspi *qspi, const void *txbuf,
			     void *rxbuf, unsigned int len)
{
	unsigned int bytes_per_word = DIV_ROUND_UP(qspi->bits_per_word, 8);
	unsigned int words;
	u32 val;
	int ret;
	int i;

	if (!len)
		return 0;

	if (len % bytes_per_word)
		return -EINVAL;

	words = len / bytes_per_word;

	ret = nuvoton_qspi_reset_fifo(qspi);
	if (ret) {
		dev_err(qspi->dev, "FIFO reset timed out\n");
		return ret;
	}

	/*
	 * Use conservative word-by-word PIO access. This keeps the initial driver
	 * simple and avoids relying on FIFO threshold interrupts or DMA support.
	 */
	for (i = 0; i < words; i++) {
		ret = nuvoton_qspi_wait_tx_not_full(qspi);
		if (ret) {
			dev_err(qspi->dev, "TX FIFO full timeout\n");
			return ret;
		}

		nuvoton_qspi_write(qspi, nuvoton_qspi_tx_word(txbuf, i,
							      qspi->bits_per_word),
				   NUVOTON_QSPI_TX_OFFSET);

		ret = nuvoton_qspi_wait_rx_not_empty(qspi);
		if (ret) {
			dev_err(qspi->dev, "RX FIFO empty timeout\n");
			return ret;
		}

		val = nuvoton_qspi_read(qspi, NUVOTON_QSPI_RX_OFFSET);
		if (rxbuf)
			nuvoton_qspi_rx_word(rxbuf, i, val, qspi->bits_per_word);
	}

	ret = nuvoton_qspi_wait_ready(qspi);
	if (ret)
		dev_err(qspi->dev, "controller busy timeout\n");

	return ret;
}

static int nuvoton_qspi_hw_init(struct nuvoton_qspi *qspi)
{
	u32 val;
	int ret;

	ret = nuvoton_qspi_set_bits_per_word(qspi, NUVOTON_QSPI_DEFAULT_BPW);
	if (ret)
		return ret;

	nuvoton_qspi_update_bits(qspi, NUVOTON_QSPI_CTL_OFFSET,
				 NUVOTON_QSPI_CTL_SUSPITV_MASK |
				 NUVOTON_QSPI_CTL_TXNEG_MASK |
				 NUVOTON_QSPI_CTL_RXNEG_MASK |
				 NUVOTON_QSPI_CTL_CLKPOL_MASK |
				 NUVOTON_QSPI_CTL_LSB_MASK,
				 NUVOTON_QSPI_CTL_TXNEG_MASK);

	val = nuvoton_qspi_read(qspi, NUVOTON_QSPI_CTL_OFFSET);
	nuvoton_qspi_write(qspi, val | NUVOTON_QSPI_CTL_SPIEN_MASK,
			   NUVOTON_QSPI_CTL_OFFSET);

	ret = readl_poll_timeout(qspi->regs + NUVOTON_QSPI_STATUS_OFFSET, val,
				 (val & NUVOTON_QSPI_STATUS_SPIENSTS_MASK),
				 1, NUVOTON_QSPI_TIMEOUT_US);
	if (ret) {
		dev_err(qspi->dev, "failed to enable controller\n");
		return ret;
	}

	ret = nuvoton_qspi_reset_fifo(qspi);
	if (ret)
		dev_err(qspi->dev, "FIFO reset timed out\n");

	return ret;
}

static bool nuvoton_qspi_mem_supports_op(struct spi_mem *mem,
					 const struct spi_mem_op *op)
{
	if (!spi_mem_default_supports_op(mem, op))
		return false;

	if (op->cmd.buswidth > 4 || op->addr.buswidth > 4 ||
	    op->dummy.buswidth > 4 || op->data.buswidth > 4)
		return false;

	if (op->cmd.nbytes != 1)
		return false;

	if (op->addr.nbytes > 4)
		return false;

	return true;
}

static void nuvoton_qspi_set_cs_level(struct nuvoton_qspi *qspi,
				      unsigned int cs, bool assert)
{
	u32 mask;
	u32 val;

	switch (cs) {
	case 0:
		mask = NUVOTON_QSPI_SSCTL_SS0_MASK;
		break;
	case 1:
		mask = NUVOTON_QSPI_SSCTL_SS1_MASK;
		break;
	default:
		dev_warn(qspi->dev, "invalid chip select %u\n", cs);
		return;
	}

	val = nuvoton_qspi_read(qspi, NUVOTON_QSPI_SSCTL_OFFSET);

	if (assert)
		val |= mask;
	else
		val &= ~mask;

	nuvoton_qspi_write(qspi, val, NUVOTON_QSPI_SSCTL_OFFSET);
}

static void nuvoton_qspi_set_cs(struct spi_device *spi, bool enable)
{
	struct nuvoton_qspi *qspi = spi_controller_get_devdata(spi->controller);

	nuvoton_qspi_set_cs_level(qspi, spi_get_chipselect(spi, 0), enable);
}

static void nuvoton_qspi_mem_set_cs(struct spi_device *spi, bool enable)
{
	struct nuvoton_qspi *qspi = spi_controller_get_devdata(spi->controller);
	bool assert = enable;

	if (spi->mode & SPI_CS_HIGH)
		assert = !assert;

	nuvoton_qspi_set_cs_level(qspi, spi_get_chipselect(spi, 0), assert);
}

static int nuvoton_qspi_mem_exec_op(struct spi_mem *mem,
				    const struct spi_mem_op *op)
{
	struct spi_device *spi = mem->spi;
	struct nuvoton_qspi *qspi = spi_controller_get_devdata(spi->controller);
	u8 opcode = op->cmd.opcode;
	u8 addr[4];
	int ret;
	int i;

	ret = nuvoton_qspi_setup_transfer(spi, op->max_freq, NUVOTON_QSPI_DEFAULT_BPW);
	if (ret)
		return ret;

	nuvoton_qspi_mem_set_cs(spi, true);

	nuvoton_qspi_set_bus_width(qspi, op->cmd.buswidth, SPI_MEM_DATA_OUT);
	ret = nuvoton_qspi_txrx(qspi, &opcode, NULL, 1);
	if (ret)
		goto out_deassert_cs;

	if (op->addr.nbytes) {
		for (i = 0; i < op->addr.nbytes; i++)
			addr[i] = op->addr.val >> (8 * (op->addr.nbytes - i - 1));

		nuvoton_qspi_set_bus_width(qspi, op->addr.buswidth,
					   SPI_MEM_DATA_OUT);
		ret = nuvoton_qspi_txrx(qspi, addr, NULL, op->addr.nbytes);
		if (ret)
			goto out_deassert_cs;
	}

	if (op->dummy.nbytes) {
		nuvoton_qspi_set_bus_width(qspi, op->dummy.buswidth,
					   SPI_MEM_DATA_IN);
		ret = nuvoton_qspi_txrx(qspi, NULL, NULL, op->dummy.nbytes);
		if (ret)
			goto out_deassert_cs;
	}

	if (op->data.nbytes) {
		nuvoton_qspi_set_bus_width(qspi, op->data.buswidth,
					   op->data.dir);
		ret = nuvoton_qspi_txrx(qspi,
					op->data.dir == SPI_MEM_DATA_OUT ?
					op->data.buf.out : NULL,
					op->data.dir == SPI_MEM_DATA_IN ?
					op->data.buf.in : NULL,
					op->data.nbytes);
	}

out_deassert_cs:
	nuvoton_qspi_set_bus_width(qspi, 1, SPI_MEM_DATA_IN);
	nuvoton_qspi_mem_set_cs(spi, false);

	return ret;
}

static const struct spi_controller_mem_ops nuvoton_qspi_mem_ops = {
	.supports_op = nuvoton_qspi_mem_supports_op,
	.exec_op = nuvoton_qspi_mem_exec_op,
};

static int nuvoton_qspi_transfer_one(struct spi_controller *ctlr,
				     struct spi_device *spi,
				     struct spi_transfer *xfer)
{
	struct nuvoton_qspi *qspi = spi_controller_get_devdata(ctlr);
	enum spi_mem_data_dir dir = SPI_MEM_DATA_IN;
	unsigned int buswidth = 1;
	int ret;

	ret = nuvoton_qspi_setup_transfer(spi, xfer->speed_hz, xfer->bits_per_word);
	if (ret)
		return ret;

	if (xfer->tx_buf && xfer->rx_buf) {
		if (xfer->tx_nbits != SPI_NBITS_SINGLE ||
		    xfer->rx_nbits != SPI_NBITS_SINGLE)
			return -EOPNOTSUPP;
	}

	if (xfer->tx_buf) {
		dir = SPI_MEM_DATA_OUT;
		if (xfer->tx_nbits == SPI_NBITS_QUAD)
			buswidth = 4;
		else if (xfer->tx_nbits == SPI_NBITS_DUAL)
			buswidth = 2;
	} else if (xfer->rx_buf) {
		if (xfer->rx_nbits == SPI_NBITS_QUAD)
			buswidth = 4;
		else if (xfer->rx_nbits == SPI_NBITS_DUAL)
			buswidth = 2;
	}

	nuvoton_qspi_set_bus_width(qspi, buswidth, dir);
	ret = nuvoton_qspi_txrx(qspi, xfer->tx_buf, xfer->rx_buf, xfer->len);
	nuvoton_qspi_set_bus_width(qspi, 1, SPI_MEM_DATA_IN);

	return ret;
}

static int nuvoton_qspi_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct spi_controller *ctlr;
	struct nuvoton_qspi *qspi;
	struct reset_control *rst;
	int ret;

	ctlr = devm_spi_alloc_host(dev, sizeof(*qspi));
	if (!ctlr)
		return -ENOMEM;

	platform_set_drvdata(pdev, ctlr);

	qspi = spi_controller_get_devdata(ctlr);
	qspi->dev = dev;
	qspi->bits_per_word = NUVOTON_QSPI_DEFAULT_BPW;

	qspi->regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(qspi->regs))
		return PTR_ERR(qspi->regs);

	rst = devm_reset_control_get_exclusive(dev, NULL);
	if (IS_ERR(rst))
		return dev_err_probe(dev, PTR_ERR(rst),
				     "failed to get reset\n");

	qspi->clk = devm_clk_get_enabled(dev, NULL);
	if (IS_ERR(qspi->clk))
		return dev_err_probe(dev, PTR_ERR(qspi->clk),
				     "failed to get and enable clock\n");

	ret = reset_control_assert(rst);
	if (ret)
		return dev_err_probe(dev, ret, "failed to assert reset\n");

	udelay(2);

	ret = reset_control_deassert(rst);
	if (ret)
		return dev_err_probe(dev, ret, "failed to deassert reset\n");

	ctlr->num_chipselect = NUVOTON_QSPI_DEFAULT_NUM_CS;
	ctlr->mem_ops = &nuvoton_qspi_mem_ops;
	ctlr->set_cs = nuvoton_qspi_set_cs;
	ctlr->transfer_one = nuvoton_qspi_transfer_one;
	ctlr->bits_per_word_mask = SPI_BPW_MASK(8) | SPI_BPW_MASK(16) |
				   SPI_BPW_MASK(32);
	ctlr->mode_bits = SPI_CPOL | SPI_CPHA | SPI_LSB_FIRST |
			  SPI_RX_DUAL | SPI_TX_DUAL |
			  SPI_RX_QUAD | SPI_TX_QUAD;
	ctlr->dev.of_node = dev->of_node;

	ret = nuvoton_qspi_hw_init(qspi);
	if (ret)
		return ret;

	ret = devm_spi_register_controller(dev, ctlr);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to register spi controller\n");

	return 0;
}

static const struct of_device_id nuvoton_qspi_of_match[] = {
	{ .compatible = "nuvoton,ma35d1-qspi" },
	{ }
};
MODULE_DEVICE_TABLE(of, nuvoton_qspi_of_match);

static struct platform_driver nuvoton_qspi_driver = {
	.driver = {
		.name = "ma35d1-qspi",
		.of_match_table = nuvoton_qspi_of_match,
	},
	.probe = nuvoton_qspi_probe,
};
module_platform_driver(nuvoton_qspi_driver);

MODULE_DESCRIPTION("Nuvoton MA35D1 QSPI controller driver");
MODULE_AUTHOR("Chi-Wen Weng <cwweng@nuvoton.com>");
MODULE_LICENSE("GPL");
