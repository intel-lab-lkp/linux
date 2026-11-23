// SPDX-License-Identifier: GPL-2.0+
//
// Platform driver for Loongson-2K0300 SPI IO Support
//
// Copyright (C) 2017-2026 Loongson Technology Corporation Limited

#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/completion.h>
#include <linux/err.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/spi/spi.h>
#include <linux/string.h>

/* Loongson SPI-IO Controller registers */
#define LOONGSON_SPI_IO_CR1_REG			0x00
#define LOONGSON_SPI_IO_CR2_REG			0x04
#define LOONGSON_SPI_IO_CR3_REG			0x08
#define LOONGSON_SPI_IO_CR4_REG			0x0c
#define LOONGSON_SPI_IO_IER_REG			0x10
#define LOONGSON_SPI_IO_SR1_REG			0x14
#define LOONGSON_SPI_IO_SR2_REG			0x18
#define LOONGSON_SPI_IO_CFG1_REG		0x20
#define LOONGSON_SPI_IO_CFG2_REG		0x24
#define LOONGSON_SPI_IO_CFG3_REG		0x28
#define LOONGSON_SPI_IO_CRC1_REG		0x30
#define LOONGSON_SPI_IO_CRC2_REG		0x34
#define LOONGSON_SPI_IO_DR_REG			0x40

/* Bitfields of LOONGSON_SPI_IO_CR1_REG */
#define LOONGSON_SPI_IO_CR1_SPE			BIT(0)
#define LOONGSON_SPI_IO_CR1_CSTART		BIT(1)
#define LOONGSON_SPI_IO_CR1_AUTOSUS		BIT(2)
#define LOONGSON_SPI_IO_CR1_SSREV		BIT(8)

#define LOONGSON_SPI_IO_CR1_START_MASK		(LOONGSON_SPI_IO_CR1_CSTART | \
						 LOONGSON_SPI_IO_CR1_AUTOSUS)

/* Bitfields of LOONGSON_SPI_IO_CR2_REG */
#define LOONGSON_SPI_IO_CR2_RXFTHLV		GENMASK(1, 0)
#define LOONGSON_SPI_IO_CR2_TXFTHLV		GENMASK(9, 8)

/* Bitfields of LOONGSON_SPI_IO_CR3_REG */
#define LOONGSON_SPI_IO_CR3_TSIZE		GENMASK(15, 0)

/* Bitfields of LOONGSON_SPI_IO_CR4_REG */
#define LOONGSON_SPI_IO_CR4_CTSIZE		GENMASK(15, 0)

/* Bitfields of LOONGSON_SPI_IO_IER_REG */
#define LOONGSON_SPI_IO_IER_RXAIE		BIT(0)
#define LOONGSON_SPI_IO_IER_TXAIE		BIT(1)
#define LOONGSON_SPI_IO_IER_DXAIE		BIT(2)
#define LOONGSON_SPI_IO_IER_RXEIE		BIT(4)
#define LOONGSON_SPI_IO_IER_TXEIE		BIT(5)
#define LOONGSON_SPI_IO_IER_SUSPIE		BIT(7)
#define LOONGSON_SPI_IO_IER_OVRIE		BIT(8)
#define LOONGSON_SPI_IO_IER_UDRIE		BIT(9)
#define LOONGSON_SPI_IO_IER_CRCEIE		BIT(10)
#define LOONGSON_SPI_IO_IER_MODFIE		BIT(11)
#define LOONGSON_SPI_IO_IER_EOTIE		BIT(15)

/* Bitfields of LOONGSON_SPI_IO_SR1_REG */
#define LOONGSON_SPI_IO_SR1_RXA			BIT(0)
#define LOONGSON_SPI_IO_SR1_TXA			BIT(1)
#define LOONGSON_SPI_IO_SR1_DXA			BIT(2)
#define LOONGSON_SPI_IO_SR1_RXE			BIT(4)
#define LOONGSON_SPI_IO_SR1_TXE			BIT(5)
#define LOONGSON_SPI_IO_SR1_SUSP		BIT(7)
#define LOONGSON_SPI_IO_SR1_OVR			BIT(8)
#define LOONGSON_SPI_IO_SR1_UDR			BIT(9)
#define LOONGSON_SPI_IO_SR1_CRCE		BIT(10)
#define LOONGSON_SPI_IO_SR1_MODF		BIT(11)
#define LOONGSON_SPI_IO_SR1_EOT			BIT(15)

#define LOONGSON_SPI_IO_SR1_ERR_MASK		(LOONGSON_SPI_IO_SR1_OVR | \
						 LOONGSON_SPI_IO_SR1_UDR | \
						 LOONGSON_SPI_IO_SR1_MODF | \
						 LOONGSON_SPI_IO_SR1_CRCE)

/* Bitfields of LOONGSON_SPI_IO_SR2_REG */
#define LOONGSON_SPI_IO_SR2_RXFLV		GENMASK(2, 0)
#define LOONGSON_SPI_IO_SR2_TXFLV		GENMASK(10, 8)

/* Bitfields of LOONGSON_SPI_IO_CFG1_REG */
#define LOONGSON_SPI_IO_CFG1_CPOL		BIT(0)
#define LOONGSON_SPI_IO_CFG1_CPHA		BIT(1)
#define LOONGSON_SPI_IO_CFG1_LSBFRST		BIT(7)
#define LOONGSON_SPI_IO_CFG1_DSIZE		GENMASK(12, 8)

/* Bitfields of LOONGSON_SPI_IO_CFG2_REG */
#define LOONGSON_SPI_IO_CFG2_BRDEC		GENMASK(7, 2)
#define LOONGSON_SPI_IO_CFG2_BRINT		GENMASK(15, 8)

#define LOONGSON_SPI_IO_CFG2_MIN_BRINT		2
#define LOONGSON_SPI_IO_CFG2_MAX_BRINT		FIELD_MAX(LOONGSON_SPI_IO_CFG2_BRINT)

/* Bitfields of LOONGSON_SPI_IO_CFG3_REG */
#define LOONGSON_SPI_IO_CFG3_MSTR		BIT(0)
#define LOONGSON_SPI_IO_CFG3_DIOSWP		BIT(1)
#define LOONGSON_SPI_IO_CFG3_DIE		BIT(2)
#define LOONGSON_SPI_IO_CFG3_DOE		BIT(3)
#define LOONGSON_SPI_IO_CFG3_SSMODE		GENMASK(9, 8)

/* Bitfields of LOONGSON_SPI_IO_CRC1_REG */
#define LOONGSON_SPI_IO_CRC1_CRCEN		BIT(0)
#define LOONGSON_SPI_IO_CRC1_CRCPOLY		GENMASK(31, 1)

/* Bitfields of LOONGSON_SPI_IO_CRC2_REG */
#define LOONGSON_SPI_IO_CRC2_RCRCINI		BIT(0)
#define LOONGSON_SPI_IO_CRC2_TCRCINI		BIT(1)
#define LOONGSON_SPI_IO_CRC2_CRCSIZE		GENMASK(12, 8)

/**
 * enum xfer_mode - Loongson SPI transfer mode
 * @XFER_IDLE: no transfer is in progress
 * @XFER_INTERRUPT: interrupt-driven batch transfer
 */
enum xfer_mode {
	XFER_IDLE,
	XFER_INTERRUPT,
};

/**
 * struct loongson_spi_priv - private data for the Loongson SPI controller
 * @controller: SPI controller instance
 * @dev: device backing the controller
 * @base: MMIO base address of the SPI controller
 * @hz: current SPI bus clock in Hz
 * @clk_rate: parent clock rate in Hz
 * @mode: current SPI mode bits
 * @bits_per_word: current word size in bits
 * @frame_bytes: bytes occupied by one frame in the FIFO and transfer buffers
 * @irq: interrupt number
 * @tx_buf: current transmit buffer for interrupt mode
 * @rx_buf: current receive buffer for interrupt mode
 * @remaining: bytes left in the current interrupt transfer
 * @xfer_ret: result of the current transfer
 * @batch_size: number of bytes in the current interrupt batch
 * @xfer_completion: completion signaled when the current transfer finishes
 * @xfer_mode: current transfer mode
 */
struct loongson_spi_priv {
	struct spi_controller   *controller;
	struct device		*dev;
	void __iomem		*base;
	u32                     hz;
	u64                     clk_rate;
	u8                      mode;
	u8			bits_per_word;
	u8			frame_bytes;

	int			irq;
	const u8		*tx_buf;
	u8			*rx_buf;
	u32			remaining;
	int			xfer_ret;
	u32			batch_size;
	struct completion	xfer_completion;
	enum xfer_mode		xfer_mode;
};

static void loongson_spi_writel(struct loongson_spi_priv *priv, u32 reg, u32 data)
{
	writel(data, priv->base + reg);
}

static u32 loongson_spi_readl(struct loongson_spi_priv *priv, u32 reg)
{
	return readl(priv->base + reg);
}

static void loongson_spi_update_bits(struct loongson_spi_priv *priv, u32 reg,
				     u32 mask, u32 val)
{
	u32 tmp = readl(priv->base + reg);

	tmp &= ~mask;
	tmp |= val & mask;
	writel(tmp, priv->base + reg);
}

/**
 * loongson_spi_io_read_dr() - Read one complete frame from the RX FIFO
 * @priv: controller private data
 *
 * The APB read width follows the 1, 2 or 4 byte FIFO storage width selected
 * by DSIZE. The unused high bits are zero and the low byte is stored first.
 *
 * Return: one frame in a little-endian container.
 */
static u32 loongson_spi_io_read_dr(struct loongson_spi_priv *priv)
{
	void __iomem *dr = priv->base + LOONGSON_SPI_IO_DR_REG;

	if (priv->frame_bytes == 1)
		return readb(dr);
	if (priv->frame_bytes == 2)
		return readw(dr);

	return readl(dr);
}

/**
 * loongson_spi_io_write_dr() - Write one complete frame to the TX FIFO
 * @priv: controller private data
 * @data: frame in a little-endian container
 *
 * Select the APB write strobes for a 1, 2 or 4 byte FIFO access according to
 * the storage width selected by DSIZE.
 */
static void loongson_spi_io_write_dr(struct loongson_spi_priv *priv, u32 data)
{
	void __iomem *dr = priv->base + LOONGSON_SPI_IO_DR_REG;

	if (priv->frame_bytes == 1)
		writeb(data, dr);
	else if (priv->frame_bytes == 2)
		writew(data, dr);
	else
		writel(data, dr);
}

/**
 * loongson_spi_io_cfg_write() - Write a configuration register
 * @priv: controller private data
 * @reg: configuration register offset
 * @val: value to write
 *
 * CFG1/CFG2/CFG3 and CRCx writes are rejected while CR1.SPE is set.
 * Temporarily disable the peripheral and restore CR1 around the write.
 * The caller must ensure that no transfer is active.
 */
static void loongson_spi_io_cfg_write(struct loongson_spi_priv *priv, u32 reg, u32 val)
{
	u32 cr1 = loongson_spi_readl(priv, LOONGSON_SPI_IO_CR1_REG);

	loongson_spi_writel(priv, LOONGSON_SPI_IO_CR1_REG,
			    cr1 & ~LOONGSON_SPI_IO_CR1_SPE);
	loongson_spi_writel(priv, reg, val);
	loongson_spi_writel(priv, LOONGSON_SPI_IO_CR1_REG, cr1);
}

/**
 * loongson_spi_io_timeout() - Calculate a transfer completion timeout
 * @priv: controller private data
 * @xfer: SPI transfer being timed
 *
 * Use twice the calculated wire time, with a minimum timeout of one second.
 *
 * Return: timeout in jiffies.
 */
static unsigned long loongson_spi_io_timeout(struct loongson_spi_priv *priv,
					     struct spi_transfer *xfer)
{
	u64 frames = xfer->len / priv->frame_bytes;
	u64 nsec = frames * priv->bits_per_word * NSEC_PER_SEC;
	u32 rate = priv->hz ? priv->hz : 1000;
	u64 msec = DIV_ROUND_UP_ULL(nsec, (u64)rate * NSEC_PER_MSEC);

	return msecs_to_jiffies(max_t(u64, msec * 2, 1000));
}

static void loongson_spi_io_set_thresh(struct loongson_spi_priv *priv)
{
	loongson_spi_update_bits(priv, LOONGSON_SPI_IO_CR2_REG,
				 LOONGSON_SPI_IO_CR2_RXFTHLV |
				 LOONGSON_SPI_IO_CR2_TXFTHLV, 0);
}

/**
 * loongson_spi_io_set_cs() - Set the hardware chip-select output
 * @spi: SPI target device
 * @high: true to assert chip select
 *
 * The SSREV bit controls the hardware chip-select assertion state.
 */
static void loongson_spi_io_set_cs(struct spi_device *spi, bool high)
{
	struct loongson_spi_priv *priv = spi_controller_get_devdata(spi->controller);
	u32 val = high ? LOONGSON_SPI_IO_CR1_SSREV : 0;

	loongson_spi_update_bits(priv, LOONGSON_SPI_IO_CR1_REG,
				 LOONGSON_SPI_IO_CR1_SSREV, val);
}

/**
 * loongson_spi_io_set_clk() - Program the SPI bus clock divider
 * @priv: controller private data
 * @hz: requested SPI bus clock in Hz
 */
static void loongson_spi_io_set_clk(struct loongson_spi_priv *priv, unsigned int hz)
{
	u32 val;

	val = clamp_val(DIV_ROUND_UP_ULL(priv->clk_rate, hz),
			LOONGSON_SPI_IO_CFG2_MIN_BRINT, LOONGSON_SPI_IO_CFG2_MAX_BRINT);

	loongson_spi_io_cfg_write(priv, LOONGSON_SPI_IO_CFG2_REG,
				  FIELD_PREP(LOONGSON_SPI_IO_CFG2_BRINT, val));

	priv->hz = hz;
}

/**
 * loongson_spi_io_set_mode() - Apply SPI mode bits and word size
 * @priv: controller private data
 * @mode: SPI mode bits from the target device
 * @bits_per_word: word size in bits
 *
 * Configure CPOL, CPHA and LSB_FIRST. DSIZE is programmed as the word size
 * minus one; pin direction and SPI_3WIRE are handled separately per transfer.
 */
static void loongson_spi_io_set_mode(struct loongson_spi_priv *priv, u32 mode,
				     u8 bits_per_word)
{
	u32 val = 0;

	if (mode & SPI_CPOL)
		val |= LOONGSON_SPI_IO_CFG1_CPOL;

	if (mode & SPI_CPHA)
		val |= LOONGSON_SPI_IO_CFG1_CPHA;

	if (mode & SPI_LSB_FIRST)
		val |= LOONGSON_SPI_IO_CFG1_LSBFRST;

	val |= FIELD_PREP(LOONGSON_SPI_IO_CFG1_DSIZE, bits_per_word - 1);
	loongson_spi_io_cfg_write(priv, LOONGSON_SPI_IO_CFG1_REG, val);

	priv->mode = mode;
	priv->bits_per_word = bits_per_word;

	if (bits_per_word <= 8)
		priv->frame_bytes = 1;
	else if (bits_per_word <= 16)
		priv->frame_bytes = 2;
	else
		priv->frame_bytes = 4;
}

/**
 * loongson_spi_io_set_pin_mode() - Select MISO/MOSI directions for a transfer
 * @spi: SPI target device
 * @xfer: SPI transfer being prepared
 *
 * Normal mode supports full-duplex and TX-only/RX-only simplex transfers.
 * In SPI_3WIRE mode, TX drives MOSI while RX switches MOSI to an input.
 * Buffers supplied by SPI core for dummy TX/RX do not select a real direction.
 *
 * Return: 0 on success, or %-EINVAL if a 3-wire transfer requests both
 * directions in the same transfer.
 */
static int loongson_spi_io_set_pin_mode(struct spi_device *spi,
					struct spi_transfer *xfer)
{
	struct spi_controller *ctrl = spi->controller;
	struct loongson_spi_priv *priv = spi_controller_get_devdata(ctrl);
	bool tx = xfer->tx_buf && xfer->tx_buf != ctrl->dummy_tx;
	bool rx = xfer->rx_buf && xfer->rx_buf != ctrl->dummy_rx;
	u32 val = LOONGSON_SPI_IO_CFG3_MSTR;

	if (spi->mode & SPI_3WIRE) {
		if (tx && rx) {
			dev_err(&spi->dev, "3-wire mode is half-duplex only");
			return -EINVAL;
		}

		if (rx)
			val |= LOONGSON_SPI_IO_CFG3_DIOSWP |
			       LOONGSON_SPI_IO_CFG3_DIE;
		else
			val |= LOONGSON_SPI_IO_CFG3_DOE;
	} else {
		/* No buffer in either direction defaults to full-duplex. */
		if (tx || !rx)
			val |= LOONGSON_SPI_IO_CFG3_DOE;
		if (rx || !tx)
			val |= LOONGSON_SPI_IO_CFG3_DIE;
	}

	if (loongson_spi_readl(priv, LOONGSON_SPI_IO_CFG3_REG) != val)
		loongson_spi_io_cfg_write(priv, LOONGSON_SPI_IO_CFG3_REG, val);

	return 0;
}

/**
 * loongson_spi_io_update_state() - Apply per-transfer clock and device mode
 * @spi: SPI target device
 * @t: SPI transfer, or NULL when only the device mode needs updating
 */
static void loongson_spi_io_update_state(struct spi_device *spi, struct spi_transfer *t)
{
	struct loongson_spi_priv *priv = spi_controller_get_devdata(spi->controller);
	u8 bits_per_word = spi->bits_per_word;

	if (t && t->bits_per_word)
		bits_per_word = t->bits_per_word;

	if (t && priv->hz != t->speed_hz)
		loongson_spi_io_set_clk(priv, t->speed_hz);

	if (((spi->mode ^ priv->mode) & (SPI_MODE_X_MASK | SPI_LSB_FIRST | SPI_3WIRE)) ||
	    priv->bits_per_word != bits_per_word)
		loongson_spi_io_set_mode(priv, spi->mode, bits_per_word);
}

/**
 * loongson_spi_io_setup() - Validate an SPI target chip select
 * @spi: SPI target device being configured
 *
 * Return: 0 if the chip select is supported, otherwise %-EINVAL.
 */
static int loongson_spi_io_setup(struct spi_device *spi)
{
	if (spi_get_chipselect(spi, 0) >= spi->controller->num_chipselect)
		return -EINVAL;

	return 0;
}

/**
 * loongson_spi_io_start_batch() - Start one interrupt-driven FIFO batch
 * @priv: controller private data
 *
 * Preload up to four FIFO bytes using whole frames, with zero-filled frames
 * when no TX buffer is present. TSIZE counts frames rather than bytes.
 *
 * Return: number of bytes in the started batch, or 0 if no data remains.
 */
static int loongson_spi_io_start_batch(struct loongson_spi_priv *priv)
{
	u32 batch_size, frames;
	u32 tx_data;

	if (priv->remaining == 0)
		return 0;

	batch_size = min_t(u32, priv->remaining, 4);
	frames = batch_size / priv->frame_bytes;
	priv->batch_size = batch_size;

	loongson_spi_writel(priv, LOONGSON_SPI_IO_CR3_REG, frames - 1);
	loongson_spi_writel(priv, LOONGSON_SPI_IO_CR4_REG, 0);

	for (unsigned int i = 0; i < batch_size; i += priv->frame_bytes) {
		tx_data = 0;
		if (priv->tx_buf) {
			memcpy(&tx_data, priv->tx_buf, priv->frame_bytes);
			priv->tx_buf += priv->frame_bytes;
		}
		loongson_spi_io_write_dr(priv, tx_data);
	}

	loongson_spi_writel(priv, LOONGSON_SPI_IO_SR1_REG, LOONGSON_SPI_IO_SR1_EOT);
	loongson_spi_update_bits(priv, LOONGSON_SPI_IO_CR1_REG,
				 LOONGSON_SPI_IO_CR1_START_MASK,
				 LOONGSON_SPI_IO_CR1_START_MASK);

	return batch_size;
}

/**
 * loongson_spi_io_irq_handler() - Handle SPI error and end-of-transfer IRQs
 * @irq: interrupt number
 * @dev_id: controller private data
 *
 * In interrupt mode, consume the current RX batch and start the next one.
 * Controller errors disable further interrupts and complete the wait with an
 * error status.
 *
 * Return: %IRQ_HANDLED for a recognized error or EOT, otherwise %IRQ_NONE.
 */
static irqreturn_t loongson_spi_io_irq_handler(int irq, void *dev_id)
{
	struct loongson_spi_priv *priv = dev_id;
	u32 rx_data;
	u32 sr1;

	sr1 = loongson_spi_readl(priv, LOONGSON_SPI_IO_SR1_REG);

	if (sr1 & LOONGSON_SPI_IO_SR1_ERR_MASK) {
		dev_err(&priv->controller->dev, "SPI error: SR1=0x%x", sr1);
		loongson_spi_writel(priv, LOONGSON_SPI_IO_IER_REG, 0);
		priv->xfer_ret = -EIO;
		complete(&priv->xfer_completion);
		return IRQ_HANDLED;
	}

	if (!(sr1 & LOONGSON_SPI_IO_SR1_EOT))
		return IRQ_NONE;

	loongson_spi_writel(priv, LOONGSON_SPI_IO_SR1_REG, LOONGSON_SPI_IO_SR1_EOT);

	switch (priv->xfer_mode) {
	case XFER_INTERRUPT:
		for (unsigned int i = 0; i < priv->batch_size;
		     i += priv->frame_bytes) {
			rx_data = loongson_spi_io_read_dr(priv);
			if (priv->rx_buf) {
				memcpy(priv->rx_buf, &rx_data, priv->frame_bytes);
				priv->rx_buf += priv->frame_bytes;
			}
		}
		priv->remaining -= priv->batch_size;

		if (priv->remaining == 0) {
			loongson_spi_update_bits(priv, LOONGSON_SPI_IO_IER_REG,
						 LOONGSON_SPI_IO_IER_EOTIE, 0);
			loongson_spi_update_bits(priv, LOONGSON_SPI_IO_CR1_REG,
						 LOONGSON_SPI_IO_CR1_CSTART, 0);
			priv->xfer_ret = 0;
			complete(&priv->xfer_completion);
		} else {
			loongson_spi_io_start_batch(priv);
		}
		break;
	case XFER_IDLE:
	default:
		dev_warn(&priv->controller->dev, "EOT with no active transfer");
		break;
	}

	return IRQ_HANDLED;
}

/**
 * loongson_spi_io_transfer_one() - Execute one SPI transfer
 * @ctrl: SPI controller
 * @spi: SPI target device
 * @xfer: transfer to execute
 *
 * Apply clock, mode and pin-direction state before running interrupt-driven
 * FIFO batches.
 *
 * Return: 0 on success, or a negative error code.
 */
static int loongson_spi_io_transfer_one(struct spi_controller *ctrl, struct spi_device *spi,
					struct spi_transfer *xfer)
{
	struct loongson_spi_priv *priv = spi_controller_get_devdata(spi->controller);
	int ret;

	loongson_spi_io_update_state(spi, xfer);

	if (xfer->len == 0)
		return 0;

	if (xfer->len % priv->frame_bytes) {
		dev_err(&spi->dev, "length %u is not aligned to %u-byte frames",
			xfer->len, priv->frame_bytes);
		return -EINVAL;
	}

	ret = loongson_spi_io_set_pin_mode(spi, xfer);
	if (ret)
		return ret;

	loongson_spi_writel(priv, LOONGSON_SPI_IO_SR1_REG,
			    LOONGSON_SPI_IO_SR1_EOT | LOONGSON_SPI_IO_SR1_ERR_MASK);

	loongson_spi_io_set_thresh(priv);

	priv->tx_buf = xfer->tx_buf;
	priv->rx_buf = xfer->rx_buf;
	priv->remaining = xfer->len;
	priv->xfer_ret = 0;

	reinit_completion(&priv->xfer_completion);
	priv->xfer_mode = XFER_INTERRUPT;

	loongson_spi_update_bits(priv, LOONGSON_SPI_IO_CR1_REG,
				 LOONGSON_SPI_IO_CR1_AUTOSUS,
				 LOONGSON_SPI_IO_CR1_AUTOSUS);
	loongson_spi_update_bits(priv, LOONGSON_SPI_IO_IER_REG,
				 LOONGSON_SPI_IO_IER_EOTIE,
				 LOONGSON_SPI_IO_IER_EOTIE);

	loongson_spi_io_start_batch(priv);

	ret = wait_for_completion_timeout(&priv->xfer_completion,
					  loongson_spi_io_timeout(priv, xfer));
	priv->xfer_mode = XFER_IDLE;

	if (ret == 0) {
		loongson_spi_writel(priv, LOONGSON_SPI_IO_IER_REG, 0);
		dev_err(&spi->dev, "Interrupt transfer timeout");
		return -ETIMEDOUT;
	}

	return priv->xfer_ret;
}

/**
 * loongson_spi_io_reginit() - Restore the basic controller configuration
 * @priv: controller private data
 *
 * Select master full-duplex pin directions and enable the SPI peripheral.
 */
static void loongson_spi_io_reginit(struct loongson_spi_priv *priv)
{
	u32 cr1;

	/* CFG3 is only writable while SPE is clear; cfg_write() handles that. */
	loongson_spi_io_cfg_write(priv, LOONGSON_SPI_IO_CFG3_REG,
				  LOONGSON_SPI_IO_CFG3_MSTR |
				  LOONGSON_SPI_IO_CFG3_DIE |
				  LOONGSON_SPI_IO_CFG3_DOE);

	cr1 = loongson_spi_readl(priv, LOONGSON_SPI_IO_CR1_REG);
	cr1 &= ~LOONGSON_SPI_IO_CR1_START_MASK;
	cr1 |= LOONGSON_SPI_IO_CR1_SPE;
	loongson_spi_writel(priv, LOONGSON_SPI_IO_CR1_REG, cr1);
}

/**
 * loongson_spi_io_handle_err() - Quiesce hardware after a failed message
 * @ctrl: SPI controller
 * @msg: failed SPI message
 *
 * Stop the engine, discard stale RX FIFO data, clear status and software
 * transfer state, then restore the basic configuration.
 */
static void loongson_spi_io_handle_err(struct spi_controller *ctrl,
				       struct spi_message *msg)
{
	struct loongson_spi_priv *priv = spi_controller_get_devdata(ctrl);

	loongson_spi_writel(priv, LOONGSON_SPI_IO_IER_REG, 0);
	synchronize_irq(priv->irq);

	loongson_spi_update_bits(priv, LOONGSON_SPI_IO_CR1_REG,
				 LOONGSON_SPI_IO_CR1_START_MASK, 0);

	loongson_spi_io_set_thresh(priv);

	while (FIELD_GET(LOONGSON_SPI_IO_SR2_RXFLV,
			 loongson_spi_readl(priv, LOONGSON_SPI_IO_SR2_REG)))
		loongson_spi_io_read_dr(priv);

	loongson_spi_writel(priv, LOONGSON_SPI_IO_SR1_REG,
			    LOONGSON_SPI_IO_SR1_EOT | LOONGSON_SPI_IO_SR1_ERR_MASK);

	priv->tx_buf = NULL;
	priv->rx_buf = NULL;
	priv->remaining = 0;
	priv->batch_size = 0;
	priv->xfer_ret = 0;
	priv->xfer_mode = XFER_IDLE;
	reinit_completion(&priv->xfer_completion);

	loongson_spi_io_reginit(priv);
}

static int loongson_spi_io_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct loongson_spi_priv *priv;
	struct spi_controller *host;
	struct clk *clk;
	int ret;

	host = devm_spi_alloc_host(dev, sizeof(*priv));
	if (!host)
		return -ENOMEM;

	host->mode_bits = SPI_MODE_X_MASK | SPI_LSB_FIRST | SPI_3WIRE;
	host->bits_per_word_mask = SPI_BPW_MASK(8) | SPI_BPW_MASK(16) | SPI_BPW_MASK(32);
	host->setup = loongson_spi_io_setup;
	host->transfer_one = loongson_spi_io_transfer_one;
	host->set_cs = loongson_spi_io_set_cs;
	host->handle_err = loongson_spi_io_handle_err;
	host->num_chipselect = 1;
	dev_set_drvdata(dev, host);

	priv = spi_controller_get_devdata(host);

	priv->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(priv->base))
		return PTR_ERR(priv->base);

	priv->controller = host;
	priv->dev = dev;
	priv->xfer_mode = XFER_IDLE;
	priv->batch_size = 0;

	init_completion(&priv->xfer_completion);

	clk = devm_clk_get_enabled(dev, NULL);
	if (IS_ERR(clk))
		return dev_err_probe(dev, PTR_ERR(clk), "unable to get clock");
	priv->clk_rate = clk_get_rate(clk);
	if (!priv->clk_rate)
		return dev_err_probe(dev, -EINVAL, "clock rate is zero");

	/* the divider is >= 2, and BRINT is 8 bits wide. */
	host->max_speed_hz = (u32)min_t(u64, div_u64(priv->clk_rate, 2), U32_MAX);
	host->min_speed_hz = (u32)min_t(u64, DIV_ROUND_UP_ULL(priv->clk_rate, 255),
					U32_MAX);

	priv->irq = platform_get_irq(pdev, 0);
	if (priv->irq < 0)
		return dev_err_probe(dev, priv->irq, "no IRQ resource");

	ret = devm_request_irq(dev, priv->irq, loongson_spi_io_irq_handler,
			       IRQF_SHARED, dev_name(dev), priv);
	if (ret)
		return dev_err_probe(dev, ret, "failed to request IRQ");

	loongson_spi_io_reginit(priv);
	loongson_spi_writel(priv, LOONGSON_SPI_IO_IER_REG, 0);

	return devm_spi_register_controller(dev, host);
}

static const struct of_device_id loongson_spi_io_id_table[] = {
	{ .compatible = "loongson,ls2k0300-spi-io" },
	{ }
};
MODULE_DEVICE_TABLE(of, loongson_spi_io_id_table);

static struct platform_driver loongson_spi_io_plat_driver = {
	.probe = loongson_spi_io_probe,
	.driver	= {
		.name	= "loongson-spi-io",
		.of_match_table = loongson_spi_io_id_table,
	},
};
module_platform_driver(loongson_spi_io_plat_driver);

MODULE_DESCRIPTION("Loongson spi io controller driver");
MODULE_AUTHOR("Loongson Technology Corporation Limited");
MODULE_LICENSE("GPL");
