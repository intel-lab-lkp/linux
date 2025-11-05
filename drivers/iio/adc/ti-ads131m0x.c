// SPDX-License-Identifier: GPL-2.0-only
/*
 * Driver for Texas Instruments ADS131M0x family ADC chips.
 *
 * Copyright (C) 2024 Protonic Holland
 * Copyright (C) 2025 Oleksij Rempel <kernel@pengutronix.de>, Pengutronix
 *
 * Primary Datasheet Reference (used for citations):
 * ADS131M08 8-Channel, Simultaneously-Sampling, 24-Bit, Delta-Sigma ADC
 * Document SBAS950B, Revised February 2021
 * https://www.ti.com/lit/ds/symlink/ads131m08.pdf
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/iio/iio.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/property.h>
#include <linux/spi/spi.h>

/* Max channels supported by the largest variant in the family (ADS131M08) */
#define ADS131M_MAX_CHANNELS		8

/* ADS131M08 tolerates up to 25 MHz SCLK.
 */
#define ADS131M_MAX_SCLK_HZ		25000000

/* Section 6.7, t_REGACQ (min time after reset) is 5us */
#define ADS131M_RESET_DELAY_US		10

/*
 * SPI Frame word count calculation.
 * Frame = N channel words + 1 response word + 1 CRC word.
 * Word size depends on WLENGTH bits in MODE register (Default 24-bit).
 */
#define ADS131M_FRAME_WSIZE(nch)	(nch + 2)
/*
 * SPI Frame byte size calculation.
 * Assumes default word size of 24 bits (3 bytes).
 */
#define ADS131M_FRAME_BSIZE(nch)	(ADS131M_FRAME_WSIZE(nch) * 3)
/*
 * Index calculation for the start byte of channel 'x' data within the RX buffer.
 * Assumes 24-bit words (3 bytes per word).
 * The received frame starts with the response word (e.g., STATUS register
 * content when NULL command was sent), followed by data for channels 0 to N-1,
 * and finally the output CRC word.
 * Response = index 0..2, Chan0 = index 3..5, Chan1 = index 6..8, ...
 * Index for ChanX = 3 (response) + x * 3 (channel data size).
 */
#define ADS131M_CHANNEL_INDEX(x)	(x * 3 + 3)

#define ADS131M_CMD_NULL		0x0000
#define ADS131M_CMD_RESET		0x0011

#define ADS131M_CMD_RREG(a, n) \
	(0xa000 | ((u16)(a & 0x1f) << 7) | (u16)(n & 0x7f))
#define ADS131M_CMD_WREG(a, n) \
	(0x6000 | ((u16)(a & 0x1f) << 7) | (u16)(n & 0x7f))

/*  STATUS Register (0x01h) bit definitions */
#define ADS131M_STATUS_CRC_ERR		BIT(12) /* Input CRC Error */

#define ADS131M_REG_MODE		0x02
#define ADS131M_MODE_RX_CRC_EN		BIT(12) /* Enable Input CRC */
#define ADS131M_MODE_CRC_TYPE_ANSI	BIT(11) /* 0=CCITT, 1=ANSI */
#define ADS131M_MODE_RESET_FLAG		BIT(10)

struct ads131m_configuration {
	const struct iio_chan_spec	*channels;
	u8				num_channels;
	u16				reset_ack;
};

enum ads131m_device_id {
	ADS131M08_ID,
	ADS131M06_ID,
	ADS131M04_ID,
	ADS131M03_ID,
	ADS131M02_ID,
};

struct ads131m_priv {
	struct spi_device *spi;
	struct clk *clk;
	struct mutex lock;
	u8 num_channels;
	const struct ads131m_configuration *config;
	u8 tx_buffer[ADS131M_FRAME_BSIZE(ADS131M_MAX_CHANNELS)]
		__aligned(IIO_DMA_MINALIGN);
	u8 rx_buffer[ADS131M_FRAME_BSIZE(ADS131M_MAX_CHANNELS)]
		__aligned(IIO_DMA_MINALIGN);
	struct spi_transfer xfer[1];
	struct spi_message msg;
	unsigned int gain[ADS131M_MAX_CHANNELS];
};

/**
 * ads131m_crc_ccitt_byte - Calculate CRC-16-CCITT (Poly 0x1021) for one byte
 * @crc: The previous 16-bit CRC value
 * @data: The new byte of data
 *
 * Device CRC:
 * - CRC-16-CCITT, polynomial 0x1021
 * - MSB-first (non-reflected), init 0xFFFF, no final xor
 * - Computed over 24-bit words as sent on the wire (MSB-aligned), including
 *   padded bits of command/data words (See Table 8-7 CRC Types).
 * Note: This differs from kernel crc_ccitt(), which is a reflected variant.
 *
 * Return: The new 16-bit CRC value.
 */
static inline u16 ads131m_crc_ccitt_byte(u16 crc, u8 data)
{
	int i;

	crc ^= ((u16)data << 8);
	for (i = 0; i < 8; i++) {
		if (crc & 0x8000)
			crc = (crc << 1) ^ 0x1021;
		else
			crc = (crc << 1);
	}
	return crc & 0xFFFF;
}

/**
 * ads131m_crc_calculate - Calculate CRC-16-CCITT over a buffer
 * @buffer: The data buffer to process
 * @len: The length of the buffer
 *
 * This function processes a buffer with the CCITT algorithm required
 * by the device, using the 0xFFFF seed.
 *
 * Return: The final 16-bit CRC.
 */
static u16 ads131m_crc_calculate(const u8 *buffer, size_t len)
{
	u16 crc = 0xFFFF;
	size_t i;

	for (i = 0; i < len; i++)
		crc = ads131m_crc_ccitt_byte(crc, buffer[i]);

	return crc;
}

/**
 * ads131m_tx_frame_unlocked - Sends a command frame with Input CRC
 * @priv: Device private data structure.
 * @command: The 16-bit command to send (e.g., NULL, RREG, RESET).
 *
 * Assumes the mutex lock is held.
 * This function sends a command in Word 0, and its calculated 16-bit
 * CRC in Word 1, as required when Input CRC is enabled.
 *
 * Return: 0 on success, or a negative error code from spi_sync.
 */
static int ads131m_tx_frame_unlocked(struct ads131m_priv *priv, u32 command)
{
	int ret;
	u16 crc;

	/*
	 * Zero the entire TX buffer to send a valid frame.
	 */
	memset(priv->tx_buffer, 0, ADS131M_FRAME_BSIZE(priv->num_channels));

	/*
	 * Word 0: 16-bit command, MSB-aligned in 24-bit word.
	 */
	put_unaligned_be24(command << 8, &priv->tx_buffer[0]);

	/*
	 * Word 1: Input CRC
	 * Calculated over the 3 bytes of Word 0.
	 */
	crc = ads131m_crc_calculate(priv->tx_buffer, 3);
	put_unaligned_be24(crc << 8, &priv->tx_buffer[3]);

	return spi_sync(priv->spi, &priv->msg);
}

/**
 * ads131m_rx_frame_unlocked - Receives a full SPI data frame.
 * @priv: Device private data structure.
 *
 * This function sends a NULL command (with its CRC) to clock out a
 * full SPI frame from the device (e.g., response + channel data + CRC).
 *
 * Assumes the mutex lock is held.
 *
 * Return: 0 on success, or a negative error code from spi_sync.
 */
static int ads131m_rx_frame_unlocked(struct ads131m_priv *priv)
{
	return ads131m_tx_frame_unlocked(priv, ADS131M_CMD_NULL);
}

/**
 * ads131m_check_status_crc_err - Checks for an Input CRC error.
 * @priv: Device private data structure.
 *
 * Sends a NULL command to fetch the STATUS register and checks the
 * CRC_ERR bit. This is used to verify the integrity of the previous
 * command (like RREG or WREG).
 *
 * Context: This function assumes the mutex 'lock' is held.
 * Return: 0 on success, -EIO if CRC_ERR bit is set.
 */
static int ads131m_check_status_crc_err(struct ads131m_priv *priv)
{
	int ret;
	u16 status;

	ret = ads131m_rx_frame_unlocked(priv);
	if (ret < 0) {
		dev_err(&priv->spi->dev, "SPI error on STATUS read for CRC check\n");
		return ret;
	}

	status = get_unaligned_be16(&priv->rx_buffer[0]);
	if (status & ADS131M_STATUS_CRC_ERR) {
		dev_err(&priv->spi->dev, "Previous input CRC error, STATUS=0x%04x\n",
			status);
		return -EIO;
	}

	return 0;
}

/**
 * ads131m_write_reg_unlocked - Writes a single register and verifies the ACK.
 * @priv: Device private data structure.
 * @reg: The 8-bit register address.
 * @val: The 16-bit value to write.
 *
 * This function performs the full 3-cycle WREG operation with Input CRC:
 * 1. (Cycle 1) Sends WREG command, data, and its calculated CRC.
 * 2. (Cycle 2) Sends NULL+CRC to retrieve the response from Cycle 1.
 * 3. Verifies the response is the correct ACK for the WREG.
 * 4. (Cycle 3) Sends NULL+CRC to retrieve STATUS and check for CRC_ERR.
 *
 * Assumes the mutex lock is held.
 *
 * Return: 0 on success, or a negative error code.
 */
static int ads131m_write_reg_unlocked(struct ads131m_priv *priv, u8 reg,
				      u16 val)
{
	u16 command, expected_ack, response, crc;
	int ret;

	command = ADS131M_CMD_WREG(reg, 0); /* n=0 for 1 register */
	/*
	 * Per Table 8-11, WREG response is: 010a aaaa ammm mmmm
	 * For 1 reg (n=0 -> m=0): 010a aaaa a000 0000 = 0x4000 | (reg << 7)
	 */
	expected_ack = 0x4000 | (reg << 7);

	/*
	 * Cycle 1: Send WREG Command + Data + Input CRC
	 */

	/* Zero the entire TX buffer */
	memset(priv->tx_buffer, 0, ADS131M_FRAME_BSIZE(priv->num_channels));

	/* Word 0: WREG command, 1 reg (n=0), MSB-aligned */
	put_unaligned_be24(command << 8, &priv->tx_buffer[0]);

	/* Word 1: Data, MSB-aligned */
	put_unaligned_be24(val << 8, &priv->tx_buffer[3]);

	/*
	 * Word 2: Input CRC
	 * Calculated over Word 0 (Cmd) and Word 1 (Data).
	 */
	crc = ads131m_crc_calculate(priv->tx_buffer, 6);
	put_unaligned_be24(crc << 8, &priv->tx_buffer[6]);

	/* We ignore the RX buffer (it's from the *previous* command) */
	ret = spi_sync(priv->spi, &priv->msg);

	if (ret < 0) {
		dev_err(&priv->spi->dev, "SPI error on WREG (cycle 1)\n");
		goto write_err;
	}

	/*
	 * Cycle 2: Send NULL Command to get the WREG response
	 */
	ret = ads131m_rx_frame_unlocked(priv);
	if (ret < 0) {
		dev_err(&priv->spi->dev, "SPI error on WREG ACK (cycle 2)\n");
		goto write_err;
	}

	/*
	 * Response is in the first 2 bytes of the RX buffer
	 * (MSB-aligned 16-bit response)
	 */
	response = get_unaligned_be16(&priv->rx_buffer[0]);

	if (response != expected_ack) {
		dev_err(&priv->spi->dev,
			"WREG(0x%02x) failed, expected ACK 0x%04x, got 0x%04x\n",
			reg, expected_ack, response);
		ret = -EIO;
		/*
		 * Don't unlock yet, still need to do Cycle 3 to clear
		 * any potential CRC_ERR flag from this failed command.
		 */
	} else {
		dev_dbg(&priv->spi->dev, "WREG(0x%02x) ACK 0x%04x OK\n",
			reg, response);
	}

	/*
	 * Cycle 3: Check STATUS for Input CRC error.
	 * This is necessary even if ACK was wrong, to clear the CRC_ERR flag.
	 */
	if (ads131m_check_status_crc_err(priv) < 0)
		ret = -EIO;

write_err:
	return ret;
}

/**
 * ads131m_read_reg_unlocked - Reads a single register from the device.
 * @priv: Device private data structure.
 * @reg: The 8-bit register address.
 * @val: Pointer to store the 16-bit register value.
 *
 * This function performs the full 3-cycle RREG operation with Input CRC:
 * 1. (Cycle 1) Sends the RREG command + Input CRC.
 * 2. (Cycle 2) Sends NULL+CRC to retrieve the register data.
 * 3. (Cycle 3) Sends NULL+CRC to retrieve STATUS and check for CRC_ERR.
 *
 * Assumes the mutex lock is held.
 *
 * Return: 0 on success, or a negative error code.
 */
static int ads131m_read_reg_unlocked(struct ads131m_priv *priv, u8 reg, u16 *val)
{
	u16 command;
	int ret;

	command = ADS131M_CMD_RREG(reg, 0); /* n=0 for 1 register */

	/*
	 * Cycle 1: Send RREG Command + Input CRC
	 * We ignore the RX buffer (it's from the previous command).
	 */
	ret = ads131m_tx_frame_unlocked(priv, command);
	if (ret < 0) {
		dev_err(&priv->spi->dev, "SPI error on RREG (cycle 1)\n");
		goto read_err;
	}

	/*
	 * Cycle 2: Send NULL Command to get the register data
	 */
	ret = ads131m_rx_frame_unlocked(priv);
	if (ret < 0) {
		dev_err(&priv->spi->dev, "SPI error on RREG data (cycle 2)\n");
		goto read_err;
	}

	/*
	 * Per datasheet, for a single reg read, the response is the data.
	 * It's in the first 2 bytes of the RX buffer (MSB-aligned 16-bit).
	 */
	*val = get_unaligned_be16(&priv->rx_buffer[0]);

	dev_dbg(&priv->spi->dev, "RREG(0x%02x) = 0x%04x\n", reg, *val);

	/*
	 * Cycle 3: Check STATUS for Input CRC error.
	 * The RREG command does not execute if CRC is bad, but we read
	 * STATUS anyway to clear the flag in case it was set.
	 */
	if (ads131m_check_status_crc_err(priv) < 0)
		ret = -EIO;

read_err:
	return ret;
}

/**
 * ads131m_rmw_reg - Reads, modifies, and writes a single register.
 * @priv: Device private data structure.
 * @reg: The 8-bit register address.
 * @clear: Bitmask of bits to clear.
 * @set: Bitmask of bits to set.
 *
 * This function performs an atomic read-modify-write operation on a register.
 * It reads the register, applies the clear and set masks, and writes
 * the new value back if it has changed.
 *
 * Context: This function handles its own mutex locking
 *
 * Return: 0 on success, or a negative error code.
 */
static int ads131m_rmw_reg(struct ads131m_priv *priv, u8 reg, u16 clear,
			   u16 set)
{
	u16 old_val, new_val;
	int ret = 0;

	mutex_lock(&priv->lock);

	ret = ads131m_read_reg_unlocked(priv, reg, &old_val);
	if (ret < 0)
		goto rmw_unlock;

	new_val = (old_val & ~clear) | set;

	if (new_val == old_val)
		goto rmw_unlock;

	ret = ads131m_write_reg_unlocked(priv, reg, new_val);

rmw_unlock:
	mutex_unlock(&priv->lock);
	return ret;
}

/**
 * ads131m_verify_output_crc - Verifies the CRC of the received SPI frame.
 * @priv: Device private data structure.
 *
 * This function calculates the CRC-16-CCITT (Poly 0x1021, Seed 0xFFFF) over
 * the received response and channel data, and compares it to the CRC word
 * received at the end of the SPI frame.
 *
 * Context: Must be called with mutex lock held.
 *
 * Return: 0 on success, -EIO on CRC mismatch.
 */
static int ads131m_verify_output_crc(struct ads131m_priv *priv)
{
	size_t data_len;
	u16 calculated_crc;
	u16 received_crc;

	/*
	 * Frame: [Response][Chan 0]...[Chan N-1][CRC Word]
	 * Data for CRC: [Response][Chan 0]...[Chan N-1]
	 * Data length = (N_channels + 1) * 3 bytes (at 24-bit word size)
	 */
	data_len = ADS131M_FRAME_BSIZE(priv->num_channels) - 3;
	calculated_crc = ads131m_crc_calculate(priv->rx_buffer, data_len);

	/*
	 * The received 16-bit CRC is MSB-aligned in the last 24-bit word.
	 * We extract it from the first 2 bytes (BE) of that word.
	 */
	received_crc = get_unaligned_be16(&priv->rx_buffer[data_len]);

	if (calculated_crc != received_crc) {
		dev_err_ratelimited(&priv->spi->dev,
				    "Output CRC error. Got %04x, expected %04x\n",
				    received_crc, calculated_crc);
		return -EIO;
	}

	return 0;
}

/**
 * ads131m_adc_read - Reads channel data, checks input and output CRCs.
 * @priv: Device private data structure.
 * @channel: The channel number to read.
 * @val: Pointer to store the raw 24-bit value.
 *
 * This function sends a NULL command (with Input CRC) to retrieve data.
 * It checks the received STATUS word for any Input CRC errors from the
 * previous command, and then verifies the Output CRC of the current
 * data frame.
 *
 * Return: 0 on success, or a negative error code.
 */
static int ads131m_adc_read(struct ads131m_priv *priv, u8 channel, s32 *val)
{
	int ret;
	u8 *buf;
	u16 status;

	mutex_lock(&priv->lock);

	/* Send NULL command + Input CRC, and receive data frame */
	ret = ads131m_rx_frame_unlocked(priv);
	if (ret < 0) {
		mutex_unlock(&priv->lock);
		return ret;
	}

	/*
	 * Check STATUS word (Word 0) for an Input CRC Error from the
	 * previous SPI frame.
	 */
	status = get_unaligned_be16(&priv->rx_buffer[0]);
	if (status & ADS131M_STATUS_CRC_ERR) {
		dev_err_ratelimited(&priv->spi->dev,
				    "Previous input CRC Error reported in STATUS (0x%04x)\n",
				    status);
	}

	/*
	 * Validate the output CRC on the current data frame to ensure
	 * data integrity.
	 */
	ret = ads131m_verify_output_crc(priv);
	if (ret < 0) {
		mutex_unlock(&priv->lock);
		return ret;
	}

	buf = &priv->rx_buffer[ADS131M_CHANNEL_INDEX(channel)];
	*val = sign_extend32(get_unaligned_be24(buf), 23);

	mutex_unlock(&priv->lock);

	return 0;
}

static int ads131m_read_raw(struct iio_dev *indio_dev,
			   struct iio_chan_spec const *channel, int *val,
			   int *val2, long mask)
{
	struct ads131m_priv *priv = iio_priv(indio_dev);
	int ret;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		ret = ads131m_adc_read(priv, channel->channel, val);
		if (ret)
			return ret;
		return IIO_VAL_INT;
	case IIO_CHAN_INFO_SCALE:
		/*
		 * Scale = (Vref / Gain) / 2^(Resolution - 1)
		 * Vref = 1.2V (1200mV) [DS, 8.3.4], Resolution = 24 bits
		 * LSB = (1.2V / Gain) / 2^23
		 *
		 * Using IIO_VAL_FRACTIONAL_LOG2, the unit is millivolts.
		 * Scale = val * 2^(-val2)
		 * Scale = 1200 * 2^-(23 + log2(Gain))
		 *
		 * priv->gain[] stores log2(Gain) (e.g., 0 for Gain=1).
		 */
		*val = 1200; /* 1.2Vref, in millivolts */
		*val2 = 23 + priv->gain[channel->channel];
		return IIO_VAL_FRACTIONAL_LOG2;
	default:
		return -EINVAL;
	}
}

#define ADS131M_VOLTAGE_CHANNEL(num)	\
	{ \
		.type = IIO_VOLTAGE, \
		.indexed = 1, \
		.channel = (num), \
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) | \
			BIT(IIO_CHAN_INFO_SCALE) \
	}

static const struct iio_chan_spec ads131m08_channels[] = {
	ADS131M_VOLTAGE_CHANNEL(0),
	ADS131M_VOLTAGE_CHANNEL(1),
	ADS131M_VOLTAGE_CHANNEL(2),
	ADS131M_VOLTAGE_CHANNEL(3),
	ADS131M_VOLTAGE_CHANNEL(4),
	ADS131M_VOLTAGE_CHANNEL(5),
	ADS131M_VOLTAGE_CHANNEL(6),
	ADS131M_VOLTAGE_CHANNEL(7),
};

static const struct iio_chan_spec ads131m06_channels[] = {
	ADS131M_VOLTAGE_CHANNEL(0),
	ADS131M_VOLTAGE_CHANNEL(1),
	ADS131M_VOLTAGE_CHANNEL(2),
	ADS131M_VOLTAGE_CHANNEL(3),
	ADS131M_VOLTAGE_CHANNEL(4),
	ADS131M_VOLTAGE_CHANNEL(5),
};

static const struct iio_chan_spec ads131m04_channels[] = {
	ADS131M_VOLTAGE_CHANNEL(0),
	ADS131M_VOLTAGE_CHANNEL(1),
	ADS131M_VOLTAGE_CHANNEL(2),
	ADS131M_VOLTAGE_CHANNEL(3),
};

static const struct iio_chan_spec ads131m03_channels[] = {
	ADS131M_VOLTAGE_CHANNEL(0),
	ADS131M_VOLTAGE_CHANNEL(1),
	ADS131M_VOLTAGE_CHANNEL(2),
};

static const struct iio_chan_spec ads131m02_channels[] = {
	ADS131M_VOLTAGE_CHANNEL(0),
	ADS131M_VOLTAGE_CHANNEL(1),
};

static const struct ads131m_configuration ads131m_config[] = {
	[ADS131M08_ID] = {
		.channels = ads131m08_channels,
		.num_channels = ARRAY_SIZE(ads131m08_channels),
		.reset_ack = 0xFF28,
	},
	[ADS131M06_ID] = {
		.channels = ads131m06_channels,
		.num_channels = ARRAY_SIZE(ads131m06_channels),
		.reset_ack = 0xFF26,
	},
	[ADS131M04_ID] = {
		.channels = ads131m04_channels,
		.num_channels = ARRAY_SIZE(ads131m04_channels),
		.reset_ack = 0xFF24,
	},
	[ADS131M03_ID] = {
		.channels = ads131m03_channels,
		.num_channels = ARRAY_SIZE(ads131m03_channels),
		.reset_ack = 0xFF23,
	},
	[ADS131M02_ID] = {
		.channels = ads131m02_channels,
		.num_channels = ARRAY_SIZE(ads131m02_channels),
		.reset_ack = 0xFF22,
	},
};

static const struct iio_info ads131m_info = {
	.read_raw = ads131m_read_raw,
};

/*
 * Prepares the reusable SPI message structure for a full-duplex transfer.
 * The ADS131M requires sending a command frame while simultaneously
 * receiving the response/data frame from the previous command cycle.
 *
 * This message is optimized for the primary data acquisition workflow:
 * sending a single-word command (like NULL) and receiving a full data
 * frame (Response + N*Channels + CRC).
 *
 * This pre-configured message is NOT suitable for variable-length SPI
 * transactions (e.g., multi-word WREG or multi-response RREG),
 * which would require a separate, dynamically-sized spi_message.
 */
static void ads131m_prepare_message(struct ads131m_priv *priv)
{
	priv->xfer[0].tx_buf = &priv->tx_buffer[0];
	priv->xfer[0].rx_buf = &priv->rx_buffer[0];
	priv->xfer[0].len = ADS131M_FRAME_BSIZE(priv->num_channels);
	spi_message_init_with_transfers(&priv->msg, &priv->xfer[0], 1);
}

static void ads131m_clk_disable_unprepare(void *arg)
{
	struct clk *clk = arg;

	clk_disable_unprepare(clk);
}

/**
 * ads131m_soft_reset - Issues a software RESET and verifies ACK.
 * @priv: Device private data structure.
 *
 * This function sends a RESET command (with Input CRC), waits t_REGACQ,
 * reads back the RESET ACK, and then sends a final NULL to check for
 * any input CRC errors.
 *
 * Return: 0 on success, or a negative error code.
 */
static int ads131m_soft_reset(struct ads131m_priv *priv)
{
	struct spi_device *spi = priv->spi;
	u16 response;
	int ret;
	u16 expected_ack = priv->config->reset_ack;

	mutex_lock(&priv->lock);
	dev_dbg(&spi->dev, "Sending RESET command\n");
	ret = ads131m_tx_frame_unlocked(priv, ADS131M_CMD_RESET);
	if (ret < 0) {
		dev_err(&spi->dev, "Failed to send RESET command\n");
		goto err_unlock;
	}

	/* Wait t_REGACQ (5us) for device to be ready after reset */
	usleep_range(ADS131M_RESET_DELAY_US, ADS131M_RESET_DELAY_US + 5);

	/* Cycle 2: Send NULL+CRC to retrieve the response to the RESET */
	dev_dbg(&spi->dev, "Reading RESET ACK\n");
	ret = ads131m_rx_frame_unlocked(priv);
	if (ret < 0) {
		dev_err(&spi->dev, "Failed to read RESET ACK\n");
		goto err_unlock;
	}

	response = get_unaligned_be16(&priv->rx_buffer[0]);

	/* Check against the device-specific ACK value */
	if (response != expected_ack) {
		dev_warn(&spi->dev, "RESET ACK mismatch, got 0x%04x, expected 0x%04x\n",
			 response, expected_ack);
		ret = -EIO;
		goto err_unlock;
	}

	/* Cycle 3: Check STATUS for Input CRC error on the RESET command. */
	if (ads131m_check_status_crc_err(priv) < 0)
		ret = -EIO;

err_unlock:
	mutex_unlock(&priv->lock);
	return ret;
}

/**
 * ads131m_hw_init - Initialize the ADC hardware.
 * @priv: Device private data structure.
 *
 * This function performs the hardware-specific initialization sequence:
 * 1. Enables the main clock.
 * 2. Issues a software RESET command to clear FIFOs and defaults.
 * 3. Configures the MODE register to clear RESET, set CCITT CRC,
 * and enable Input CRC checking.
 *
 * Return: 0 on success, or a negative error code.
 */
static int ads131m_hw_init(struct ads131m_priv *priv)
{
	struct spi_device *spi = priv->spi;
	u16 clear_mask, set_mask;
	int ret;

	ret = clk_prepare_enable(priv->clk);
	if (ret) {
		dev_err(&spi->dev, "clk enable failed: %d\n", ret);
		return ret;
	}
	ret = devm_add_action_or_reset(&spi->dev, ads131m_clk_disable_unprepare,
				       priv->clk);
	if (ret) {
		clk_disable_unprepare(priv->clk);
		return ret;
	}

	/*
	 * Issue a software RESET to ensure device is in a known state.
	 * This clears the 2-deep FIFO and resets all registers to default.
	 */
	ret = ads131m_soft_reset(priv);
	if (ret < 0)
		return ret;

	/*
	 * The RESET command sets all registers to default, which means:
	 * 1. The RESET bit (Bit 10) in MODE is set to '1'.
	 * 2. The CRC_TYPE bit (Bit 11) in MODE is '0' (CCITT).
	 * 3. The RX_CRC_EN bit (Bit 12) in MODE is '0' (Disabled).
	 *
	 * We must:
	 * 1. Clear the RESET bit.
	 * 2. Enable Input CRC (RX_CRC_EN).
	 * 3. Explicitly clear the ANSI CRC bit (for certainty).
	 */
	clear_mask = ADS131M_MODE_CRC_TYPE_ANSI | ADS131M_MODE_RESET_FLAG;
	set_mask = ADS131M_MODE_RX_CRC_EN;

	ret = ads131m_rmw_reg(priv, ADS131M_REG_MODE, clear_mask, set_mask);
	if (ret < 0) {
		dev_err(&spi->dev, "Failed to configure MODE register\n");
		return ret;
	}

	return 0;
}

static int ads131m_probe(struct spi_device *spi)
{
	const struct ads131m_configuration *config;
	struct iio_dev *indio_dev;
	struct ads131m_priv *priv;
	int ret;

	spi->mode = SPI_MODE_1;
	spi->bits_per_word = 8;

	if (!spi->max_speed_hz || spi->max_speed_hz > ADS131M_MAX_SCLK_HZ)
		spi->max_speed_hz = ADS131M_MAX_SCLK_HZ;

	ret = spi_setup(spi);
	if (ret < 0) {
		dev_err(&spi->dev, "Error in spi setup\n");
		return ret;
	}

	indio_dev = devm_iio_device_alloc(&spi->dev, sizeof(*priv));
	if (!indio_dev)
		return -ENOMEM;

	priv = iio_priv(indio_dev);
	priv->spi = spi;

	indio_dev->name = spi_get_device_id(spi)->name;
	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->info = &ads131m_info;

	config = device_get_match_data(&spi->dev);
	if (!config) {
		const struct spi_device_id *id;

		id = spi_get_device_id(spi);
		if (!id)
			return -ENODEV;

		config = (const void *)id->driver_data;
	}
	priv->config = config;

	indio_dev->channels = config->channels;
	indio_dev->num_channels = config->num_channels;
	priv->num_channels = config->num_channels;

	/* Get the external clock source connected to the CLKIN pin */
	priv->clk = devm_clk_get(&spi->dev, NULL);
	if (IS_ERR(priv->clk)) {
		ret = PTR_ERR(priv->clk);
		dev_err(&spi->dev, "clk get failed: %d\n", ret);
		return ret;
	}

	mutex_init(&priv->lock);
	/* Setup the reusable SPI message structure */
	ads131m_prepare_message(priv);

	/*
	 * Perform all hardware-specific initialization.
	 */
	ret = ads131m_hw_init(priv);
	if (ret < 0)
		return ret;

	return devm_iio_device_register(&spi->dev, indio_dev);
}

static const struct of_device_id ads131m_of_match[] = {
	{ .compatible = "ti,ads131m08", .data = &ads131m_config[ADS131M08_ID] },
	{ .compatible = "ti,ads131m06", .data = &ads131m_config[ADS131M06_ID] },
	{ .compatible = "ti,ads131m04", .data = &ads131m_config[ADS131M04_ID] },
	{ .compatible = "ti,ads131m03", .data = &ads131m_config[ADS131M03_ID] },
	{ .compatible = "ti,ads131m02", .data = &ads131m_config[ADS131M02_ID] },
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(of, ads131m_of_match);

static const struct spi_device_id ads131m_id[] = {
	{ "ads131m08", (kernel_ulong_t)&ads131m_config[ADS131M08_ID] },
	{ "ads131m06", (kernel_ulong_t)&ads131m_config[ADS131M06_ID] },
	{ "ads131m04", (kernel_ulong_t)&ads131m_config[ADS131M04_ID] },
	{ "ads131m03", (kernel_ulong_t)&ads131m_config[ADS131M03_ID] },
	{ "ads131m02", (kernel_ulong_t)&ads131m_config[ADS131M02_ID] },
	{ }
};
MODULE_DEVICE_TABLE(spi, ads131m_id);

static struct spi_driver ads131m_driver = {
	.driver = {
		.name = "ads131m0x",
		.of_match_table = of_match_ptr(ads131m_of_match),
	},
	.probe = ads131m_probe,
	.id_table = ads131m_id,
};
module_spi_driver(ads131m_driver);

MODULE_AUTHOR("David Jander <david@protonic.nl>");
MODULE_DESCRIPTION("Texas Instruments ADS131M0x ADC driver");
MODULE_LICENSE("GPL");
