// SPDX-License-Identifier: GPL-2.0-only
/*
 * Driver for Texas Instruments ADS131M02 family ADC chips.
 *
 * Copyright (C) 2024 Protonic Holland
 * Copyright (C) 2025 Oleksij Rempel <kernel@pengutronix.de>, Pengutronix
 *
 * Primary Datasheet Reference (used for citations):
 * ADS131M08 8-Channel, Simultaneously-Sampling, 24-Bit, Delta-Sigma ADC
 * Document SBAS950B, Revised February 2021
 * https://www.ti.com/lit/ds/symlink/ads131m08.pdf
 */

#include <linux/cleanup.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/iio/iio.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/regulator/consumer.h>
#include <linux/spi/spi.h>
#include <linux/unaligned.h>

/* Max channels supported by the largest variant in the family (ADS131M08) */
#define ADS131M_MAX_CHANNELS		8

/* Section 6.7, t_REGACQ (min time after reset) is 5us */
#define ADS131M_RESET_DELAY_US		5

#define ADS131M_WORD_SIZE_BYTES		3
#define ADS131M_RESPONSE_WORDS		1
#define ADS131M_CRC_WORDS		1

/*
 * SPI Frame word count calculation.
 * Frame = N channel words + 1 response word + 1 CRC word.
 * Word size depends on WLENGTH bits in MODE register (Default 24-bit).
 */
#define ADS131M_FRAME_WORDS(nch) \
	((nch) + ADS131M_RESPONSE_WORDS + ADS131M_CRC_WORDS)

/*
 * SPI Frame byte size calculation.
 * Assumes default word size of 24 bits (3 bytes).
 */
#define ADS131M_FRAME_BYTES(nch) \
	(ADS131M_FRAME_WORDS(nch) * ADS131M_WORD_SIZE_BYTES)

/*
 * Index calculation for the start byte of channel 'x' data within the RX buffer.
 * Assumes 24-bit words (3 bytes per word).
 * The received frame starts with the response word (e.g., STATUS register
 * content when NULL command was sent), followed by data for channels 0 to N-1,
 * and finally the output CRC word.
 * Response = index 0..2, Chan0 = index 3..5, Chan1 = index 6..8, ...
 * Index for ChanX = 3 (response) + x * 3 (channel data size).
 */
#define ADS131M_CHANNEL_INDEX(x) \
	((x) * ADS131M_WORD_SIZE_BYTES + ADS131M_WORD_SIZE_BYTES)

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
#define ADS131M_MODE_CRC_TYPE_ANSI	BIT(11) /* 0 = CCITT, 1 = ANSI */
#define ADS131M_MODE_RESET_FLAG		BIT(10)

#define ADS131M_REG_CLOCK		0x03
#define ADS131M_CLOCK_XTAL_DIS		BIT(7)
#define ADS131M_CLOCK_EXTREF_EN		BIT(6)

/* 1.2V internal reference, in millivolts, for IIO_VAL_FRACTIONAL_LOG2 */
#define ADS131M_VREF_INTERNAL_MV	1200
/* 24-bit resolution */
#define ADS131M_RESOLUTION_BITS		24
/* Divisor is 2^(Res - 1) for signed 2's complement */
#define ADS131M_SCALE_DIVISOR		(1UL << (ADS131M_RESOLUTION_BITS - 1))
/* External ref FSR = Vref * 0.96 */
#define ADS131M_EXTREF_SCALE_NUM	96
#define ADS131M_EXTREF_SCALE_DEN	100

struct ads131m_configuration {
	const struct iio_chan_spec *channels;
	u8 num_channels;
	u16 reset_ack;
	bool supports_extref;
	bool supports_xtal;
	const char *name;
};

struct ads131m_priv {
	struct iio_dev *indio_dev;
	struct spi_device *spi;
	struct clk *clk;
	const struct ads131m_configuration *config;
	struct gpio_desc *reset_gpio;

	struct regulator *refin_supply;

	struct spi_transfer xfer;
	struct spi_message msg;

	/*
	 * Protects the shared tx_buffer and rx_buffer. More importantly,
	 * this serializes all SPI communication to ensure the atomicity
	 * of multi-cycle command sequences (like WREG, RREG, or RESET).
	 */
	struct mutex lock;

	/*
	 * DMA-safe buffers are placed at the end of the struct.
	 * This prevents the compiler from inserting large, wasteful
	 * padding blocks between non-aligned fields.
	 *
	 * Both buffers require alignment, as the size of tx_buffer (30 bytes)
	 * is not a multiple of typical DMA alignment values.
	 */
	u8 tx_buffer[ADS131M_FRAME_BYTES(ADS131M_MAX_CHANNELS)]
		__aligned(IIO_DMA_MINALIGN);
	u8 rx_buffer[ADS131M_FRAME_BYTES(ADS131M_MAX_CHANNELS)]
		__aligned(IIO_DMA_MINALIGN);
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
 * This function sends a command in Word 0, and its calculated 16-bit
 * CRC in Word 1, as required when Input CRC is enabled.
 *
 * Return: 0 on success, or a negative error code from spi_sync.
 */
static int ads131m_tx_frame_unlocked(struct ads131m_priv *priv, u32 command)
{
	struct iio_dev *indio_dev = priv->indio_dev;
	u16 crc;

	lockdep_assert_held(&priv->lock);

	memset(priv->tx_buffer, 0, ADS131M_FRAME_BYTES(indio_dev->num_channels));

	/* Word 0: 16-bit command, MSB-aligned in 24-bit word */
	put_unaligned_be16(command, &priv->tx_buffer[0]);

	/* Word 1: Input CRC. Calculated over the 3 bytes of Word 0. */
	crc = ads131m_crc_calculate(priv->tx_buffer, 3);
	put_unaligned_be16(crc, &priv->tx_buffer[3]);

	return spi_sync(priv->spi, &priv->msg);
}

/**
 * ads131m_rx_frame_unlocked - Receives a full SPI data frame.
 * @priv: Device private data structure.
 *
 * This function sends a NULL command (with its CRC) to clock out a
 * full SPI frame from the device (e.g., response + channel data + CRC).
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
 * Return: 0 on success, -EIO if CRC_ERR bit is set.
 */
static int ads131m_check_status_crc_err(struct ads131m_priv *priv)
{
	struct device *dev = &priv->spi->dev;
	u16 status;
	int ret;

	lockdep_assert_held(&priv->lock);

	ret = ads131m_rx_frame_unlocked(priv);
	if (ret < 0) {
		dev_err_ratelimited(dev,
				    "SPI error on STATUS read for CRC check\n");
		return ret;
	}

	status = get_unaligned_be16(&priv->rx_buffer[0]);
	if (status & ADS131M_STATUS_CRC_ERR) {
		dev_err_ratelimited(dev,
				    "Previous input CRC error, STATUS = 0x%04x\n",
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
 * Return: 0 on success, or a negative error code.
 */
static int ads131m_write_reg_unlocked(struct ads131m_priv *priv, u8 reg,
				      u16 val)
{
	struct iio_dev *indio_dev = priv->indio_dev;
	u16 command, expected_ack, response, crc;
	struct device *dev = &priv->spi->dev;
	int ret_crc_err = 0;
	int ret;

	lockdep_assert_held(&priv->lock);

	command = ADS131M_CMD_WREG(reg, 0); /* n = 0 for 1 register */
	/*
	 * Per Table 8-11, WREG response is: 010a aaaa ammm mmmm
	 * For 1 reg (n = 0 -> m = 0): 010a aaaa a000 0000 = 0x4000 | (reg << 7)
	 */
	expected_ack = 0x4000 | (reg << 7);

	/* Cycle 1: Send WREG Command + Data + Input CRC */

	memset(priv->tx_buffer, 0, ADS131M_FRAME_BYTES(indio_dev->num_channels));

	/* Word 0: WREG command, 1 reg (n = 0), MSB-aligned */
	put_unaligned_be16(command, &priv->tx_buffer[0]);

	/* Word 1: Data, MSB-aligned */
	put_unaligned_be16(val, &priv->tx_buffer[3]);

	/* Word 2: Input CRC. Calculated over Word 0 (Cmd) and Word 1 (Data). */
	crc = ads131m_crc_calculate(priv->tx_buffer, 6);
	put_unaligned_be16(crc, &priv->tx_buffer[6]);

	/* Ignore the RX buffer (it's from the previous command) */
	ret = spi_sync(priv->spi, &priv->msg);
	if (ret < 0) {
		dev_err_ratelimited(dev,
				    "SPI error on WREG (cycle 1)\n");
		return ret;
	}

	/* Cycle 2: Send NULL Command to get the WREG response */
	ret = ads131m_rx_frame_unlocked(priv);
	if (ret < 0) {
		dev_err_ratelimited(dev,
				    "SPI error on WREG ACK (cycle 2)\n");
		return ret;
	}

	/*
	 * Response is in the first 2 bytes of the RX buffer
	 * (MSB-aligned 16-bit response)
	 */
	response = get_unaligned_be16(&priv->rx_buffer[0]);
	if (response != expected_ack) {
		dev_err_ratelimited(dev,
				    "WREG(0x%02x) failed, expected ACK 0x%04x, got 0x%04x\n",
				    reg, expected_ack, response);
		ret_crc_err = -EIO;
		/*
		 * Don't return yet, still need to do Cycle 3 to clear
		 * any potential CRC_ERR flag from this failed command.
		 */
	}

	/*
	 * Cycle 3: Check STATUS for Input CRC error.
	 * This is necessary even if ACK was wrong, to clear the CRC_ERR flag.
	 */
	ret = ads131m_check_status_crc_err(priv);

	return ret < 0 ? ret : ret_crc_err;
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
 * Return: 0 on success, or a negative error code.
 */
static int ads131m_read_reg_unlocked(struct ads131m_priv *priv, u8 reg, u16 *val)
{
	struct device *dev = &priv->spi->dev;
	u16 command;
	int ret;

	lockdep_assert_held(&priv->lock);

	command = ADS131M_CMD_RREG(reg, 0); /* n=0 for 1 register */

	/*
	 * Cycle 1: Send RREG Command + Input CRC
	 * Ignore the RX buffer (it's from the previous command)
	 */
	ret = ads131m_tx_frame_unlocked(priv, command);
	if (ret < 0) {
		dev_err_ratelimited(dev, "SPI error on RREG (cycle 1)\n");
		return ret;
	}

	/* Cycle 2: Send NULL Command to get the register data */
	ret = ads131m_rx_frame_unlocked(priv);
	if (ret < 0) {
		dev_err_ratelimited(dev, "SPI error on RREG data (cycle 2)\n");
		return ret;
	}

	/*
	 * Per datasheet, for a single reg read, the response is the data.
	 * It's in the first 2 bytes of the RX buffer (MSB-aligned 16-bit).
	 */
	*val = get_unaligned_be16(&priv->rx_buffer[0]);

	/*
	 * Cycle 3: Check STATUS for Input CRC error.
	 * The RREG command does not execute if CRC is bad, but we read
	 * STATUS anyway to clear the flag in case it was set.
	 */
	return ads131m_check_status_crc_err(priv);
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
 * Return: 0 on success, or a negative error code.
 */
static int ads131m_rmw_reg(struct ads131m_priv *priv, u8 reg, u16 clear,
			   u16 set)
{
	u16 old_val, new_val;
	int ret;

	guard(mutex)(&priv->lock);

	ret = ads131m_read_reg_unlocked(priv, reg, &old_val);
	if (ret < 0)
		return ret;

	new_val = (old_val & ~clear) | set;

	if (new_val == old_val)
		return 0;

	return ads131m_write_reg_unlocked(priv, reg, new_val);
}

/**
 * ads131m_verify_output_crc - Verifies the CRC of the received SPI frame.
 * @priv: Device private data structure.
 *
 * This function calculates the CRC-16-CCITT (Poly 0x1021, Seed 0xFFFF) over
 * the received response and channel data, and compares it to the CRC word
 * received at the end of the SPI frame.
 *
 * Return: 0 on success, -EIO on CRC mismatch.
 */
static int ads131m_verify_output_crc(struct ads131m_priv *priv)
{
	struct iio_dev *indio_dev = priv->indio_dev;
	struct device *dev = &priv->spi->dev;
	u16 calculated_crc;
	u16 received_crc;
	size_t data_len;

	lockdep_assert_held(&priv->lock);

	/*
	 * Frame: [Response][Chan 0]...[Chan N-1][CRC Word]
	 * Data for CRC: [Response][Chan 0]...[Chan N-1]
	 * Data length = (N_channels + 1) * 3 bytes (at 24-bit word size)
	 */
	data_len = ADS131M_FRAME_BYTES(indio_dev->num_channels) - 3;
	calculated_crc = ads131m_crc_calculate(priv->rx_buffer, data_len);

	/*
	 * The received 16-bit CRC is MSB-aligned in the last 24-bit word.
	 * We extract it from the first 2 bytes (BE) of that word.
	 */
	received_crc = get_unaligned_be16(&priv->rx_buffer[data_len]);

	if (calculated_crc != received_crc) {
		dev_err_ratelimited(dev,
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
	struct device *dev = &priv->spi->dev;
	u16 status;
	int ret;
	u8 *buf;

	guard(mutex)(&priv->lock);

	/* Send NULL command + Input CRC, and receive data frame */
	ret = ads131m_rx_frame_unlocked(priv);
	if (ret < 0)
		return ret;

	/*
	 * Check STATUS word (Word 0) for an Input CRC Error from the
	 * previous SPI frame.
	 */
	status = get_unaligned_be16(&priv->rx_buffer[0]);
	if (status & ADS131M_STATUS_CRC_ERR) {
		dev_err_ratelimited(dev,
				    "Previous input CRC Error reported in STATUS (0x%04x)\n",
				    status);
	}

	/*
	 * Validate the output CRC on the current data frame to ensure
	 * data integrity.
	 */
	ret = ads131m_verify_output_crc(priv);
	if (ret < 0)
		return ret;

	buf = &priv->rx_buffer[ADS131M_CHANNEL_INDEX(channel)];
	*val = sign_extend32(get_unaligned_be24(buf), 23);

	return 0;
}

static int ads131m_read_scale(struct ads131m_priv *priv, int *val, int *val2)
{
	struct device *dev = &priv->spi->dev;
	int vref_uv;
	int ret;

	/* This driver assumes default PGA gain of 1 */
	if (IS_ERR_OR_NULL(priv->refin_supply)) {
		/*
		 * Internal 1.2V reference.
		 * Scale = 1200mV / 2^23
		 */
		*val = ADS131M_VREF_INTERNAL_MV;
		*val2 = ADS131M_RESOLUTION_BITS - 1;

		return IIO_VAL_FRACTIONAL_LOG2;
	}

	ret = regulator_get_voltage(priv->refin_supply);
	if (ret < 0) {
		dev_err_ratelimited(dev, "failed to get refin voltage: %pe\n",
				    ERR_PTR(ret));
		return ret;
	}

	vref_uv = ret;

	/*
	 * External reference.
	 * Scale(mV) = (Vref_uV * 0.96) / (1000 * 2^23)
	 */
	*val = div_s64((s64)vref_uv * ADS131M_EXTREF_SCALE_NUM, 1000);
	*val2 = ADS131M_EXTREF_SCALE_DEN * ADS131M_SCALE_DIVISOR;

	return IIO_VAL_FRACTIONAL;
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
		return ads131m_read_scale(priv, val, val2);
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

static const struct iio_chan_spec ads131m02_channels[] = {
	ADS131M_VOLTAGE_CHANNEL(0),
	ADS131M_VOLTAGE_CHANNEL(1),
};

static const struct iio_chan_spec ads131m03_channels[] = {
	ADS131M_VOLTAGE_CHANNEL(0),
	ADS131M_VOLTAGE_CHANNEL(1),
	ADS131M_VOLTAGE_CHANNEL(2),
};

static const struct iio_chan_spec ads131m04_channels[] = {
	ADS131M_VOLTAGE_CHANNEL(0),
	ADS131M_VOLTAGE_CHANNEL(1),
	ADS131M_VOLTAGE_CHANNEL(2),
	ADS131M_VOLTAGE_CHANNEL(3),
};

static const struct iio_chan_spec ads131m06_channels[] = {
	ADS131M_VOLTAGE_CHANNEL(0),
	ADS131M_VOLTAGE_CHANNEL(1),
	ADS131M_VOLTAGE_CHANNEL(2),
	ADS131M_VOLTAGE_CHANNEL(3),
	ADS131M_VOLTAGE_CHANNEL(4),
	ADS131M_VOLTAGE_CHANNEL(5),
};

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

static const struct ads131m_configuration ads131m02_config = {
	.channels = ads131m02_channels,
	.num_channels = ARRAY_SIZE(ads131m02_channels),
	.reset_ack = 0xff22,
	.name = "ads131m02",
};

static const struct ads131m_configuration ads131m03_config = {
	.channels = ads131m03_channels,
	.num_channels = ARRAY_SIZE(ads131m03_channels),
	.reset_ack = 0xff23,
	.name = "ads131m03",
};

static const struct ads131m_configuration ads131m04_config = {
	.channels = ads131m04_channels,
	.num_channels = ARRAY_SIZE(ads131m04_channels),
	.reset_ack = 0xff24,
	.name = "ads131m04",
};

static const struct ads131m_configuration ads131m06_config = {
	.channels = ads131m06_channels,
	.num_channels = ARRAY_SIZE(ads131m06_channels),
	.reset_ack = 0xff26,
	.supports_extref = true,
	.supports_xtal = true,
	.name = "ads131m06",
};

static const struct ads131m_configuration ads131m08_config = {
	.channels = ads131m08_channels,
	.num_channels = ARRAY_SIZE(ads131m08_channels),
	.reset_ack = 0xff28,
	.supports_extref = true,
	.supports_xtal = true,
	.name = "ads131m08",
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
 *
 * Return: 0 on success, or a negative error code.
 */
static int ads131m_prepare_message(struct ads131m_priv *priv)
{
	struct iio_dev *indio_dev = priv->indio_dev;
	struct device *dev = &priv->spi->dev;
	int ret;

	priv->xfer.tx_buf = priv->tx_buffer;
	priv->xfer.rx_buf = priv->rx_buffer;
	priv->xfer.len = ADS131M_FRAME_BYTES(indio_dev->num_channels);
	spi_message_init_with_transfers(&priv->msg, &priv->xfer, 1);

	ret = devm_spi_optimize_message(dev, priv->spi, &priv->msg);
	if (ret)
		return dev_err_probe(dev, ret, "failed to optimize SPI message\n");

	return 0;
}

/**
 * ads131m_hw_reset - Pulses the optional hardware reset GPIO.
 * @priv: Device private data structure.
 *
 * Pulses the /RESET line to perform a hardware reset and waits the
 * required t_REGACQ time for the device to be ready.
 *
 * Return: 0 on success, or a negative error code.
 */
static int ads131m_hw_reset(struct ads131m_priv *priv)
{
	struct device *dev = &priv->spi->dev;
	int ret;

	/* Datasheet: Hold /RESET low for > 2 f_CLKIN cycles. 1us is ample. */
	ret = gpiod_set_value_cansleep(priv->reset_gpio, 1);
	if (ret < 0)
		return dev_err_probe(dev, ret, "Failed to assert reset GPIO\n");

	fsleep(1);

	ret = gpiod_set_value_cansleep(priv->reset_gpio, 0);
	if (ret < 0)
		return dev_err_probe(dev, ret, "Failed to deassert reset GPIO\n");

	/* Wait t_REGACQ (5us) for registers to be accessible */
	fsleep(ADS131M_RESET_DELAY_US);

	return 0;
}

/**
 * ads131m_sw_reset - Issues a software RESET and verifies ACK.
 * @priv: Device private data structure.
 *
 * This function sends a RESET command (with Input CRC), waits t_REGACQ,
 * reads back the RESET ACK, and then sends a final NULL to check for
 * any input CRC errors.
 *
 * Return: 0 on success, or a negative error code.
 */
static int ads131m_sw_reset(struct ads131m_priv *priv)
{
	u16 expected_ack = priv->config->reset_ack;
	struct device *dev = &priv->spi->dev;
	u16 response;
	int ret;

	guard(mutex)(&priv->lock);

	ret = ads131m_tx_frame_unlocked(priv, ADS131M_CMD_RESET);
	if (ret < 0)
		return dev_err_probe(dev, ret, "Failed to send RESET command\n");

	/* Wait t_REGACQ (5us) for device to be ready after reset */
	fsleep(ADS131M_RESET_DELAY_US);

	/* Cycle 2: Send NULL + CRC to retrieve the response to the RESET */
	ret = ads131m_rx_frame_unlocked(priv);
	if (ret < 0)
		return dev_err_probe(dev, ret, "Failed to read RESET ACK\n");

	response = get_unaligned_be16(&priv->rx_buffer[0]);

	/* Check against the device-specific ACK value */
	if (response != expected_ack)
		return dev_err_probe(dev, -EIO,
				     "RESET ACK mismatch, got 0x%04x, expected 0x%04x\n",
				     response, expected_ack);

	/* Cycle 3: Check STATUS for Input CRC error on the RESET command. */
	return ads131m_check_status_crc_err(priv);
}

/**
 * ads131m_reset - Resets the device using hardware or software.
 * @priv: Device private data structure.
 *
 * This function performs a hardware reset if the reset-gpio is provided,
 * otherwise it issues a software RESET command via SPI.
 *
 * Note: The software reset path also validates the device's reset
 * acknowledgment against the expected ID for the compatible string.
 * The hardware reset path bypasses this ID check.
 *
 * Return: 0 on success, or a negative error code.
 */
static int ads131m_reset(struct ads131m_priv *priv)
{
	if (priv->reset_gpio)
		return ads131m_hw_reset(priv);

	return ads131m_sw_reset(priv);
}

/*
 * This is a local helper to be passed to devm_add_action_or_reset.
 * It mimics the non-exported regulator_action_disable() from devres.c.
 */
static void ads131m_regulator_disable_action(void *data)
{
	struct regulator *regulator = data;

	regulator_disable(regulator);
}

/**
 * ads131m_power_init - Get and enable regulators.
 * @priv: Device private data structure.
 *
 * Return: 0 on success, or a negative error code.
 */
static int ads131m_power_init(struct ads131m_priv *priv)
{
	struct device *dev = &priv->spi->dev;
	static const char * const supply_ids[] = { "avdd", "dvdd" };
	int ret;

	/* Get and enable the mandatory supplies */
	ret = devm_regulator_bulk_get_enable(dev,
					     ARRAY_SIZE(supply_ids),
					     supply_ids);
	if (ret < 0)
		return dev_err_probe(dev, ret, "failed to enable regulators\n");

	/* If the model doesn't support extref, we're done. */
	if (!priv->config->supports_extref)
		return 0;

	/*
	 * Get the optional external reference. This schedules regulator_put()
	 * automatically.
	 */
	priv->refin_supply = devm_regulator_get_optional(dev, "refin");
	if (IS_ERR(priv->refin_supply)) {
		/* -ENODEV is fine, it just means we'll use the internal ref */
		if (PTR_ERR(priv->refin_supply) == -ENODEV)
			priv->refin_supply = NULL;
		else
			return dev_err_probe(dev, PTR_ERR(priv->refin_supply),
					     "failed to get refin regulator\n");
	}

	/* If the regulator was not found, we're done. */
	if (!priv->refin_supply)
		return 0;

	/*
	 * We need to store the regulator pointer for scaling, but
	 * devm_regulator_get_enable_optional() doesn't return it.
	 * We must use devm_regulator_get_optional(), manually enable,
	 * and schedule the disable action.
	 */
	ret = regulator_enable(priv->refin_supply);
	if (ret) {
		/*
		 * Enable failed. devm_regulator_put() is already scheduled,
		 * so we just return the error.
		 */
		return dev_err_probe(dev, ret, "failed to enable refin regulator\n");
	}

	/*
	 * Schedule our local ads131m_regulator_disable_action() helper
	 * to be called on driver unload.
	 */
	return devm_add_action_or_reset(dev, ads131m_regulator_disable_action,
					priv->refin_supply);
}

/**
 * ads131m_hw_init - Initialize the ADC hardware.
 * @priv: Device private data structure.
 * @is_xtal: True if 'clock-names' is "xtal", false if "clkin".
 *
 * This function performs the hardware-specific initialization sequence:
 * - Issues a software RESET command to clear FIFOs and defaults.
 * - Configures the MODE register to clear RESET, set CCITT CRC,
 * and enable Input CRC checking.
 *
 * Return: 0 on success, or a negative error code.
 */
static int ads131m_hw_init(struct ads131m_priv *priv, bool is_xtal)
{
	struct device *dev = &priv->spi->dev;
	u16 mode_clear, mode_set;
	int ret;

	/*
	 * Issue a software RESET to ensure device is in a known state.
	 * This clears the 2-deep FIFO and resets all registers to default.
	 */
	ret = ads131m_reset(priv);
	if (ret < 0)
		return ret;

	/*
	 * Configure CLOCK register (0x03) based on DT properties.
	 * This register only needs configuration for 32-pin (M06/M08)
	 * variants, as the configurable bits (XTAL_DIS, EXTREF_EN)
	 * are reserved on 20-pin (M02/M03/M04) variants.
	 */
	if (priv->config->supports_xtal || priv->config->supports_extref) {
		u16 clk_clear = 0;
		u16 clk_set = 0;

		if (priv->config->supports_xtal) {
			if (!is_xtal) /* "clkin" */
				clk_set |= ADS131M_CLOCK_XTAL_DIS;
		}

		if (priv->config->supports_extref) {
			if (!IS_ERR_OR_NULL(priv->refin_supply))
				clk_set |= ADS131M_CLOCK_EXTREF_EN;
			else
				clk_clear |= ADS131M_CLOCK_EXTREF_EN;
		}

		ret = ads131m_rmw_reg(priv, ADS131M_REG_CLOCK, clk_clear,
				      clk_set);
		if (ret < 0)
			return dev_err_probe(dev, ret,
					     "Failed to configure CLOCK register\n");
	} else if (is_xtal) {
		/* 20-pin (M02/M03/M04) variant with "xtal" clock-name */
		return dev_err_probe(dev, -EINVAL,
				     "xtal clock not supported on this model\n");
	}

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
	mode_clear = ADS131M_MODE_CRC_TYPE_ANSI | ADS131M_MODE_RESET_FLAG;
	mode_set = ADS131M_MODE_RX_CRC_EN;

	ret = ads131m_rmw_reg(priv, ADS131M_REG_MODE, mode_clear, mode_set);
	if (ret < 0)
		return dev_err_probe(dev, ret, "Failed to configure MODE register\n");

	return 0;
}

static int ads131m_probe(struct spi_device *spi)
{
	const struct ads131m_configuration *config;
	struct device *dev = &spi->dev;
	struct iio_dev *indio_dev;
	struct ads131m_priv *priv;
	const char *clock_name;
	bool is_xtal;
	int ret;

	indio_dev = devm_iio_device_alloc(dev, sizeof(*priv));
	if (!indio_dev)
		return -ENOMEM;

	priv = iio_priv(indio_dev);
	priv->indio_dev = indio_dev;
	priv->spi = spi;

	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->info = &ads131m_info;

	config = spi_get_device_match_data(spi);
	if (!config)
		return dev_err_probe(dev, -EINVAL,
				     "No device configuration data found\n");

	priv->config = config;
	indio_dev->name = config->name;
	indio_dev->channels = config->channels;
	indio_dev->num_channels = config->num_channels;

	/* Get the external clock source connected to the CLKIN pin */
	priv->clk = devm_clk_get_enabled(dev, NULL);
	if (IS_ERR(priv->clk))
		return dev_err_probe(dev, PTR_ERR(priv->clk),
				     "clk get enabled failed\n");

	ret = device_property_read_string(dev, "clock-names", &clock_name);
	if (ret < 0)
		return dev_err_probe(dev, ret,
				     "device property 'clock-names' not found\n");
	is_xtal = (strcmp(clock_name, "xtal") == 0);

	priv->reset_gpio = devm_gpiod_get_optional(dev, "reset",
						   GPIOD_OUT_LOW);
	if (IS_ERR(priv->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(priv->reset_gpio),
				     "Failed to get reset GPIO\n");

	ret = devm_mutex_init(dev, &priv->lock);
	if (ret < 0)
		return ret;

	ret = ads131m_prepare_message(priv);
	if (ret < 0)
		return ret;

	ret = ads131m_power_init(priv);
	if (ret < 0)
		return ret;

	ret = ads131m_hw_init(priv, is_xtal);
	if (ret < 0)
		return ret;

	return devm_iio_device_register(dev, indio_dev);
}

static const struct of_device_id ads131m_of_match[] = {
	{ .compatible = "ti,ads131m02", .data = &ads131m02_config },
	{ .compatible = "ti,ads131m03", .data = &ads131m03_config },
	{ .compatible = "ti,ads131m04", .data = &ads131m04_config },
	{ .compatible = "ti,ads131m06", .data = &ads131m06_config },
	{ .compatible = "ti,ads131m08", .data = &ads131m08_config },
	{ } /* Fixed sentinel */
};
MODULE_DEVICE_TABLE(of, ads131m_of_match);

static const struct spi_device_id ads131m_id[] = {
	{ "ads131m02", (kernel_ulong_t)&ads131m02_config },
	{ "ads131m03", (kernel_ulong_t)&ads131m03_config },
	{ "ads131m04", (kernel_ulong_t)&ads131m04_config },
	{ "ads131m06", (kernel_ulong_t)&ads131m06_config },
	{ "ads131m08", (kernel_ulong_t)&ads131m08_config },
	{ }
};
MODULE_DEVICE_TABLE(spi, ads131m_id);

static struct spi_driver ads131m_driver = {
	.driver = {
		.name = "ads131m02",
		.of_match_table = ads131m_of_match,
	},
	.probe = ads131m_probe,
	.id_table = ads131m_id,
};
module_spi_driver(ads131m_driver);

MODULE_AUTHOR("David Jander <david@protonic.nl>");
MODULE_DESCRIPTION("Texas Instruments ADS131M02 ADC driver");
MODULE_LICENSE("GPL");
