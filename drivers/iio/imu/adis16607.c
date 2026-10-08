// SPDX-License-Identifier: GPL-2.0-only
/*
 * ADIS16607 IMU driver
 *
 * Copyright 2026 Analog Devices Inc.
 */
#include <linux/bitfield.h>
#include <linux/bitops.h>
#include <linux/clk.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/iio/buffer.h>
#include <linux/iio/iio.h>
#include <linux/iio/imu/adis.h>
#include <linux/iio/sysfs.h>
#include <linux/iio/trigger_consumer.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/math.h>
#include <linux/units.h>
#include <linux/module.h>
#include <linux/mod_devicetable.h>
#include <linux/property.h>
#include <linux/regulator/consumer.h>
#include <linux/spi/spi.h>
#include <linux/unaligned.h>

/* Register addresses */
#define ADIS16607_REG_DEV_ID		0x00
#define ADIS16607_REG_REV_ID		0x01
#define ADIS16607_REG_SERIAL_NUM0	0x02
#define ADIS16607_REG_SERIAL_NUM1	0x03
#define ADIS16607_REG_SERIAL_NUM2	0x04
#define ADIS16607_REG_DIAG_STAT		0x05
#define ADIS16607_REG_X_ACCEL		0x06
#define ADIS16607_REG_Y_ACCEL		0x08
#define ADIS16607_REG_Z_ACCEL		0x0A
#define ADIS16607_REG_X_GYRO		0x0C
#define ADIS16607_REG_Y_GYRO		0x0E
#define ADIS16607_REG_Z_GYRO		0x10
#define ADIS16607_REG_X_DELTVEL		0x12
#define ADIS16607_REG_Y_DELTVEL		0x14
#define ADIS16607_REG_Z_DELTVEL		0x16
#define ADIS16607_REG_X_DELTANG		0x18
#define ADIS16607_REG_Y_DELTANG		0x1A
#define ADIS16607_REG_Z_DELTANG		0x1C
#define ADIS16607_REG_TEMP		0x20
#define ADIS16607_REG_FIFO_WORD_CNT	0x2B
#define ADIS16607_REG_USER_GPIO_CFG	0x2F
#define ADIS16607_REG_SPI_HALFDUPLEX	0x32
#define ADIS16607_REG_USER_SYNC		0x33
#define ADIS16607_REG_USER_DATA_CFG	0x34
#define ADIS16607_REG_USER_FIFO_CFG	0x35
#define ADIS16607_REG_SOFT_RESET	0x36
#define ADIS16607_REG_SELF_TEST		0x39
#define ADIS16607_REG_DEC_RATE		0x3A
#define ADIS16607_REG_DIGITAL_STATUS	0x4E

/* SPI Half-Duplex lock value */
#define ADIS16607_LOCK_SPI_HALFDUPLEX	0xB4B4

/* Masks */
#define ADIS16607_RESET_GPIO_MASK	GENMASK(2, 0)
#define ADIS16607_SYNC_GPIO_MASK	GENMASK(8, 6)
#define ADIS16607_DR_GPIO_MASK		GENMASK(11, 9)
#define ADIS16607_DEV_NOK_MASK		GENMASK(14, 12)
#define ADIS16607_DATA_CNTR_EN_MASK	BIT(14)
#define ADIS16607_BURST32_MASK		BIT(15)
#define ADIS16607_SYNC_MODE_MASK	BIT(15)
#define ADIS16607_BOOTLOADER_BUSY_MASK	BIT(0)

/* FIFO masks */
#define ADIS16607_FIFO_THR_MASK		GENMASK(10, 0)
#define ADIS16607_FIFO_FLUSH_MASK	BIT(15)
#define ADIS16607_FIFO_THR_MET_MASK	BIT(0)
#define ADIS16607_FIFO_DATA_CMD		0xA9

/* FIFO limits */
#define ADIS16607_MAX_FIFO_WM		38
#define ADIS16607_FIFO_WORDS_PER_SAMPLE	26
#define ADIS16607_FIFO_DATA_LEN		52

/* Burst read - Half-Duplex mode (DATA_CNTR enabled) */
#define ADIS16607_BURST_CMD_SIZE	4

/* Self-test registers and masks */
#define ADIS16607_REG_SELF_TEST_DATA(x)	((x) + 0x23)
#define ADIS16607_SNSR_SELF_TEST_MASK	BIT(6)
#define ADIS16607_ST_FORCE_MASK		BIT(7)

/* Self-test delta check limits */
#define ADIS16607_ACCEL_XY_DELTA_MAX	260
#define ADIS16607_ACCEL_Z_DELTA_MAX	4000
#define ADIS16607_GYRO_DELTA_MAX	2600

/* Sync modes */
enum adis16607_sync_mode {
	ADIS16607_SYNC_DEFAULT,
	ADIS16607_SYNC_DIRECT,
	ADIS16607_SYNC_SCALED,
};

struct adis16607_sync {
	u16 sync_mode;
	u16 min_rate;
	u16 max_rate;
};

struct adis16607_chip_info {
	const struct iio_chan_spec *channels;
	const struct adis16607_sync *sync_mode;
	const char *name;
	u32 num_channels;
	u32 gyro_max_val;
	u32 gyro_max_scale;
	u32 accel_max_val;
	u32 accel_max_scale;
	u32 temp_scale;
	u32 deltang_max_val;
	u32 deltvel_max_val;
	u32 int_clk;
	u16 max_dec;
	u8 num_sync;
};

struct adis16607 {
	const struct adis16607_chip_info *info;
	struct adis adis;
	unsigned long clk_freq;	/* millihertz */
	u64 sample_period_ns;
	u32 sync_mode;
	u16 fifo_watermark;
	/* Scratch buffer used for FIFO pop operations, see adis16607_fifo_pop() */
	u8 *fifo_buf;
	/* IIO buffer for reordered data - sized for max channels */
	u8 data[64] __aligned(IIO_DMA_MINALIGN);
};

enum {
	ADIS16607_SCAN_ACCEL_X,
	ADIS16607_SCAN_ACCEL_Y,
	ADIS16607_SCAN_ACCEL_Z,
	ADIS16607_SCAN_GYRO_X,
	ADIS16607_SCAN_GYRO_Y,
	ADIS16607_SCAN_GYRO_Z,
	ADIS16607_SCAN_DELTVEL_X,
	ADIS16607_SCAN_DELTVEL_Y,
	ADIS16607_SCAN_DELTVEL_Z,
	ADIS16607_SCAN_DELTANG_X,
	ADIS16607_SCAN_DELTANG_Y,
	ADIS16607_SCAN_DELTANG_Z,
	ADIS16607_SCAN_TEMP,
};

/*
 * SPI Half-Duplex read operation for ADIS16607.
 * Protocol:
 *   - First transfer: Send [reg | 0x80, 0, 0, 0] - device latches command
 *   - Second transfer: Send same command, device responds in bytes 2-3
 *   - CS toggles between transfers (cs_change = 1 on first xfer)
 * For 4-byte reads (24-bit sensor data):
 *   - Read MSW from reg, read LSW from reg+1
 *   - Combine as: ((val_msw << 8) & 0xFFFF00) | val_lsw
 */
static int adis16607_spi_read(struct adis *adis, unsigned int reg,
			      unsigned int *val, unsigned int size)
{
	struct spi_message msg;
	struct spi_transfer xfers[2] = {
		{
			.tx_buf = adis->tx,
			.len = 4,
			.bits_per_word = 8,
			.cs_change = 1,
		},
		{
			.tx_buf = adis->tx,
			.rx_buf = adis->rx,
			.len = 4,
			.bits_per_word = 8,
		},
	};
	unsigned int val_msw, val_lsw;
	int ret;

	switch (size) {
	case 2:
		adis->tx[0] = reg | BIT(7);
		adis->tx[1] = 0;
		adis->tx[2] = 0;
		adis->tx[3] = 0;

		spi_message_init(&msg);
		spi_message_add_tail(&xfers[0], &msg);
		spi_message_add_tail(&xfers[1], &msg);

		ret = spi_sync(adis->spi, &msg);
		if (ret)
			return ret;

		*val = get_unaligned_be16(&adis->rx[2]);
		break;

	case 4:
		/* Read MSW from reg */
		adis->tx[0] = reg | BIT(7);
		adis->tx[1] = 0;
		adis->tx[2] = 0;
		adis->tx[3] = 0;

		spi_message_init(&msg);
		spi_message_add_tail(&xfers[0], &msg);
		spi_message_add_tail(&xfers[1], &msg);

		ret = spi_sync(adis->spi, &msg);
		if (ret)
			return ret;

		val_msw = get_unaligned_be16(&adis->rx[2]);

		/* Read LSW from reg+1 */
		adis->tx[0] = (reg + 1) | BIT(7);

		spi_message_init(&msg);
		spi_message_add_tail(&xfers[0], &msg);
		spi_message_add_tail(&xfers[1], &msg);

		ret = spi_sync(adis->spi, &msg);
		if (ret)
			return ret;

		val_lsw = adis->rx[2];

		/* Combine: 24-bit value with MSW in upper 16 bits, LSW in lower 8 bits */
		*val = ((val_msw << 8) & 0xFFFF00) | val_lsw;
		break;

	default:
		return -EINVAL;
	}

	return 0;
}

/*
 * SPI Half-Duplex write operation for ADIS16607.
 * Protocol:
 *   - Send 3 bytes: [reg, val_high, val_low]
 *   - Only 16-bit writes are supported
 */
static int adis16607_spi_write(struct adis *adis, unsigned int reg,
			       unsigned int val, unsigned int size)
{
	if (size != 2)
		return -EINVAL;

	adis->tx[0] = reg;
	adis->tx[1] = (val >> 8) & 0xFF;
	adis->tx[2] = val & 0xFF;

	return spi_write(adis->spi, adis->tx, 3);
}

static int adis16607_reset(struct adis *adis)
{
	return adis16607_spi_write(adis, ADIS16607_REG_SOFT_RESET, BIT(0), 2);
}

static const struct adis_ops adis16607_ops = {
	.read = adis16607_spi_read,
	.write = adis16607_spi_write,
	.reset = adis16607_reset,
};

/*
 * FIFO/burst data offsets for each channel (in bytes from the data start,
 * i.e. after the 4-byte command prefix).
 * Each 32-bit inertial channel occupies 4 bytes (MSW + LSW), TEMP is 2 bytes.
 *
 * FIFO data layout (52 bytes, DATA_CNTR enabled, no checksum):
 *   Offset 0-3:   ACCEL_X  (32-bit: MSW, LSW)
 *   Offset 4-7:   ACCEL_Y
 *   Offset 8-11:  ACCEL_Z
 *   Offset 12-15: GYRO_X
 *   Offset 16-19: GYRO_Y
 *   Offset 20-23: GYRO_Z
 *   Offset 24-27: DELTVEL_X
 *   Offset 28-31: DELTVEL_Y
 *   Offset 32-35: DELTVEL_Z
 *   Offset 36-39: DELTANG_X
 *   Offset 40-43: DELTANG_Y
 *   Offset 44-47: DELTANG_Z
 *   Offset 48-49: TEMP (16-bit)
 *   Offset 50-51: DATA_CNTR (16-bit)
 *
 * Each 32-bit value encodes 24-bit sensor data as:
 *   value_24bit = (MSW << 8) | (LSW >> 8)
 */
static const u8 adis16607_burst_offsets[] = {
	[ADIS16607_SCAN_ACCEL_X] = 0,
	[ADIS16607_SCAN_ACCEL_Y] = 4,
	[ADIS16607_SCAN_ACCEL_Z] = 8,
	[ADIS16607_SCAN_GYRO_X] = 12,
	[ADIS16607_SCAN_GYRO_Y] = 16,
	[ADIS16607_SCAN_GYRO_Z] = 20,
	[ADIS16607_SCAN_DELTVEL_X] = 24,
	[ADIS16607_SCAN_DELTVEL_Y] = 28,
	[ADIS16607_SCAN_DELTVEL_Z] = 32,
	[ADIS16607_SCAN_DELTANG_X] = 36,
	[ADIS16607_SCAN_DELTANG_Y] = 40,
	[ADIS16607_SCAN_DELTANG_Z] = 44,
	[ADIS16607_SCAN_TEMP] = 48,
};

static void adis16607_fifo_buf_cleanup(void *arg)
{
	struct adis16607 *st = arg;

	kfree(st->fifo_buf);
}

/*
 * Sets up the scratch buffer used for FIFO pop operations (see
 * adis16607_fifo_pop()). The 32-bit burst mode and DATA_CNTR config bits
 * are enabled once during startup (adis16607_post_reset_setup()), not
 * here, since they don't depend on scan_mask.
 */
static int adis16607_update_scan_mode(struct iio_dev *indio_dev,
				      const unsigned long *scan_mask)
{
	struct adis16607 *st = iio_priv(indio_dev);
	unsigned int burst_len = ADIS16607_BURST_CMD_SIZE + ADIS16607_FIFO_DATA_LEN;

	kfree(st->fifo_buf);

	st->fifo_buf = kzalloc(burst_len, GFP_KERNEL);
	if (!st->fifo_buf)
		return -ENOMEM;

	return 0;
}

/*
 * Pop a single sample from FIFO using FIFO_DATA command (0xA9).
 * Returns the raw FIFO data in the provided buffer.
 */
static int adis16607_fifo_pop(struct adis16607 *st, u8 *data)
{
	struct spi_message msg;
	struct spi_transfer xfer = {
		.tx_buf = st->fifo_buf,
		.rx_buf = st->fifo_buf,
		.len = ADIS16607_BURST_CMD_SIZE + ADIS16607_FIFO_DATA_LEN,
		.bits_per_word = 8,
	};
	int ret;

	/* FIFO data command */
	st->fifo_buf[0] = ADIS16607_FIFO_DATA_CMD;
	st->fifo_buf[1] = 0;
	st->fifo_buf[2] = 0;
	st->fifo_buf[3] = 0;

	spi_message_init(&msg);
	spi_message_add_tail(&xfer, &msg);

	ret = spi_sync(st->adis.spi, &msg);
	if (ret)
		return ret;

	/* Copy data starting after command bytes */
	memcpy(data, st->fifo_buf + ADIS16607_BURST_CMD_SIZE, ADIS16607_FIFO_DATA_LEN);
	return 0;
}

/*
 * Process a single FIFO sample and push to IIO buffer.
 * FIFO data format differs from regular burst - no DIAG_STAT, data starts immediately.
 *
 * FIFO data layout (52 bytes):
 *   Offset 0-3:   ACCEL_X (32-bit)
 *   Offset 4-7:   ACCEL_Y (32-bit)
 *   Offset 8-11:  ACCEL_Z (32-bit)
 *   Offset 12-15: GYRO_X (32-bit)
 *   Offset 16-19: GYRO_Y (32-bit)
 *   Offset 20-23: GYRO_Z (32-bit)
 *   Offset 24-27: DELTVEL_X (32-bit)
 *   Offset 28-31: DELTVEL_Y (32-bit)
 *   Offset 32-35: DELTVEL_Z (32-bit)
 *   Offset 36-39: DELTANG_X (32-bit)
 *   Offset 40-43: DELTANG_Y (32-bit)
 *   Offset 44-47: DELTANG_Z (32-bit)
 *   Offset 48-49: TEMP (16-bit)
 *   Offset 50-51: DATA_CNTR (16-bit)
 */
static void adis16607_fifo_push_sample(struct iio_dev *indio_dev,
				       u8 *fifo_data, s64 timestamp)
{
	struct adis16607 *st = iio_priv(indio_dev);
	u16 msw, lsw;
	s32 val24;
	int bit, offset = 0;

	memset(st->data, 0, sizeof(st->data));

	for_each_set_bit(bit, indio_dev->active_scan_mask, indio_dev->masklength) {
		u8 burst_off;

		/* Skip the soft timestamp channel - it has no burst offset */
		if (bit >= ARRAY_SIZE(adis16607_burst_offsets))
			continue;

		burst_off = adis16607_burst_offsets[bit];

		if (bit == ADIS16607_SCAN_TEMP) {
			/* TEMP is 16-bit */
			put_unaligned_be16(get_unaligned_be16(&fifo_data[burst_off]),
					   &st->data[offset]);
			offset += 2;
		} else {
			/* All other channels are 24-bit stored as 32-bit */
			msw = get_unaligned_be16(&fifo_data[burst_off]);
			lsw = get_unaligned_be16(&fifo_data[burst_off + 2]);
			val24 = sign_extend32(((u32)msw << 8) | (lsw >> 8), 23);
			put_unaligned_be32(val24, &st->data[offset]);
			offset += 4;
		}
	}

	iio_push_to_buffers_with_timestamp(indio_dev, st->data, timestamp);
}

/*
 * Trigger handler - reads samples from the device FIFO. Triggered by the
 * DEV_NOK interrupt, which fires on any DIAG_STAT flag, not just
 * FIFO_THRESH_MET - the DIAG_STAT read below both checks which flag(s)
 * raised the interrupt and clears the latched status, and must happen
 * before any FIFO pop, or we'd pop for nothing on an unrelated flag.
 */
static irqreturn_t adis16607_trigger_handler(int irq, void *p)
{
	struct iio_poll_func *pf = p;
	struct iio_dev *indio_dev = pf->indio_dev;
	struct adis16607 *st = iio_priv(indio_dev);
	u8 fifo_sample[ADIS16607_FIFO_DATA_LEN];
	u16 diag_stat, word_cnt, sample_cnt, threshold;
	s64 timestamp = pf->timestamp;
	int ret, i;

	mutex_lock(&st->adis.state_lock);

	/* Check if FIFO threshold is met */
	ret = __adis_read_reg_16(&st->adis, ADIS16607_REG_DIAG_STAT, &diag_stat);
	if (ret)
		goto unlock;

	if (!(diag_stat & ADIS16607_FIFO_THR_MET_MASK))
		goto unlock;

	/* Temporarily disable FIFO before reading */
	ret = __adis_write_reg_16(&st->adis, ADIS16607_REG_USER_FIFO_CFG, 0);
	if (ret)
		goto unlock;

	/* Read FIFO word count */
	ret = __adis_read_reg_16(&st->adis, ADIS16607_REG_FIFO_WORD_CNT, &word_cnt);
	if (ret)
		goto reenable_fifo;

	/* Calculate number of complete samples */
	sample_cnt = word_cnt / ADIS16607_FIFO_WORDS_PER_SAMPLE;
	if (!sample_cnt)
		goto reenable_fifo;

	/* Pop and process each sample */
	for (i = 0; i < sample_cnt; i++) {
		ret = adis16607_fifo_pop(st, fifo_sample);
		if (ret)
			goto reenable_fifo;

		adis16607_fifo_push_sample(indio_dev, fifo_sample, timestamp);
		timestamp += st->sample_period_ns;
	}

reenable_fifo:
	/* Flush FIFO before re-enabling */
	ret = __adis_write_reg_16(&st->adis, ADIS16607_REG_USER_FIFO_CFG,
				  ADIS16607_FIFO_FLUSH_MASK);
	if (ret)
		dev_warn_ratelimited(&st->adis.spi->dev,
				     "failed to flush FIFO: %d\n", ret);

	/* Re-enable FIFO with watermark threshold */
	threshold = st->fifo_watermark * ADIS16607_FIFO_WORDS_PER_SAMPLE;
	ret = __adis_write_reg_16(&st->adis, ADIS16607_REG_USER_FIFO_CFG,
				  threshold & ADIS16607_FIFO_THR_MASK);
	if (ret)
		dev_warn_ratelimited(&st->adis.spi->dev,
				     "failed to re-enable FIFO: %d\n", ret);

unlock:
	mutex_unlock(&st->adis.state_lock);
	iio_trigger_notify_done(indio_dev->trig);
	return IRQ_HANDLED;
}

/* FIFO sysfs attributes */
IIO_STATIC_CONST_DEVICE_ATTR(hwfifo_watermark_min, "1");
IIO_STATIC_CONST_DEVICE_ATTR(hwfifo_watermark_max, __stringify(ADIS16607_MAX_FIFO_WM));

static ssize_t hwfifo_watermark_show(struct device *dev,
				     struct device_attribute *attr,
				     char *buf)
{
	struct iio_dev *indio_dev = dev_to_iio_dev(dev);
	struct adis16607 *st = iio_priv(indio_dev);

	return sysfs_emit(buf, "%u\n", st->fifo_watermark);
}

static ssize_t hwfifo_enabled_show(struct device *dev,
				   struct device_attribute *attr,
				   char *buf)
{
	struct iio_dev *indio_dev = dev_to_iio_dev(dev);
	struct adis16607 *st = iio_priv(indio_dev);
	u16 val;
	int ret;

	ret = adis_read_reg_16(&st->adis, ADIS16607_REG_USER_FIFO_CFG, &val);
	if (ret)
		return ret;

	return sysfs_emit(buf, "%d\n", (val & ADIS16607_FIFO_THR_MASK) ? 1 : 0);
}

static IIO_DEVICE_ATTR_RO(hwfifo_watermark, 0);
static IIO_DEVICE_ATTR_RO(hwfifo_enabled, 0);

static const struct iio_dev_attr *adis16607_fifo_attributes[] = {
	&iio_dev_attr_hwfifo_watermark_min,
	&iio_dev_attr_hwfifo_watermark_max,
	&iio_dev_attr_hwfifo_watermark,
	&iio_dev_attr_hwfifo_enabled,
	NULL
};

static int adis16607_buffer_postenable(struct iio_dev *indio_dev)
{
	struct adis16607 *st = iio_priv(indio_dev);
	struct adis *adis = &st->adis;
	u16 threshold;
	int ret;

	mutex_lock(&adis->state_lock);

	/* Flush FIFO to start clean */
	ret = __adis_write_reg_16(adis, ADIS16607_REG_USER_FIFO_CFG,
				  ADIS16607_FIFO_FLUSH_MASK);
	if (ret)
		goto unlock;

	/* Enable FIFO with watermark threshold (in word count) */
	threshold = st->fifo_watermark * ADIS16607_FIFO_WORDS_PER_SAMPLE;
	ret = __adis_write_reg_16(adis, ADIS16607_REG_USER_FIFO_CFG,
				  threshold & ADIS16607_FIFO_THR_MASK);

unlock:
	mutex_unlock(&adis->state_lock);
	return ret;
}

static int adis16607_buffer_postdisable(struct iio_dev *indio_dev)
{
	struct adis16607 *st = iio_priv(indio_dev);
	struct adis *adis = &st->adis;
	int ret;

	mutex_lock(&adis->state_lock);

	/* Disable FIFO */
	ret = __adis_write_reg_16(adis, ADIS16607_REG_USER_FIFO_CFG, 0);

	mutex_unlock(&adis->state_lock);
	return ret;
}

static const struct iio_buffer_setup_ops adis16607_buffer_ops = {
	.postenable = adis16607_buffer_postenable,
	.postdisable = adis16607_buffer_postdisable,
};

static int adis16607_set_watermark(struct iio_dev *indio_dev, unsigned int val)
{
	struct adis16607 *st = iio_priv(indio_dev);

	val = clamp_t(unsigned int, val, 1, ADIS16607_MAX_FIFO_WM);
	st->fifo_watermark = val;

	return 0;
}

/*
 * Check if the delta between two values is within specified range.
 */
static bool adis16607_delta_check(s16 val1, s16 val2, s16 delta_max)
{
	return abs(val1 - val2) <= delta_max;
}

/*
 * Perform sensor self-test for ADIS16607.
 * Reads self-test data before and after forcing self-test,
 * then verifies delta values are within acceptable ranges.
 */
static int adis16607_sensor_self_test(struct adis16607 *st)
{
	struct adis *adis = &st->adis;
	u16 st1_val[6], st2_val[6];
	s16 delta_max[6] = {
		ADIS16607_ACCEL_XY_DELTA_MAX,
		ADIS16607_ACCEL_XY_DELTA_MAX,
		ADIS16607_ACCEL_Z_DELTA_MAX,
		ADIS16607_GYRO_DELTA_MAX,
		ADIS16607_GYRO_DELTA_MAX,
		ADIS16607_GYRO_DELTA_MAX,
	};
	int ret, i;

	/* Enable sensor self-test */
	ret = __adis_write_reg_16(adis, ADIS16607_REG_SELF_TEST,
				  ADIS16607_SNSR_SELF_TEST_MASK);
	if (ret)
		return ret;

	/* Wait for self-test to settle before reading results */
	msleep(adis->data->timeouts->self_test_ms);

	/* Read first set of self-test data */
	for (i = 0; i < 6; i++) {
		ret = __adis_read_reg_16(adis, ADIS16607_REG_SELF_TEST_DATA(i),
					 &st1_val[i]);
		if (ret)
			return ret;
	}

	/* Force self-test */
	ret = __adis_write_reg_16(adis, ADIS16607_REG_SELF_TEST,
				  ADIS16607_SNSR_SELF_TEST_MASK | ADIS16607_ST_FORCE_MASK);
	if (ret)
		return ret;

	/* Wait for forced self-test to settle before reading results */
	msleep(adis->data->timeouts->self_test_ms);

	/* Read second set of self-test data */
	for (i = 0; i < 6; i++) {
		ret = __adis_read_reg_16(adis, ADIS16607_REG_SELF_TEST_DATA(i),
					 &st2_val[i]);
		if (ret)
			return ret;

		if (!adis16607_delta_check((s16)st1_val[i], (s16)st2_val[i],
					   delta_max[i])) {
			dev_err(&adis->spi->dev,
				"Self-test failed for axis %d: delta out of range\n", i);
			return -EINVAL;
		}
	}

	/* Clear self-test register */
	ret = __adis_write_reg_16(adis, ADIS16607_REG_SELF_TEST, 0);
	if (ret)
		return ret;

	return 0;
}

/*
 * Post-reset bring-up callback for struct adis_data (see adis16607_data
 * below). __adis_initial_startup() calls this, unlocked, right after it
 * has handled reset (hardware GPIO or SW reset, adislib's own job) and
 * before self-test - no mutex is taken here either, matching that
 * convention. Does the device-specific startup steps adislib has no
 * concept of: SPI Half-Duplex locking, dev_id verification, the
 * bootloader-busy check, a reset-status GPIO output (if wired up),
 * enabling 32-bit burst mode + DATA_CNTR, and the rest of USER_GPIO_CFG.
 */
static int adis16607_post_reset_setup(struct adis *adis)
{
	struct device *dev = &adis->spi->dev;
	u16 diag_stat, digital_status, dev_id;
	int ret;

	/* Additional 100ms required at initial startup per datasheet */
	msleep(100);

	/* Lock the device into SPI Half-Duplex mode */
	ret = __adis_write_reg_16(adis, ADIS16607_REG_SPI_HALFDUPLEX,
				  ADIS16607_LOCK_SPI_HALFDUPLEX);
	if (ret) {
		dev_err(dev, "failed to lock SPI Half-Duplex mode: %d\n", ret);
		return ret;
	}

	/* Verify the correct device is attached */
	ret = __adis_read_reg_16(adis, ADIS16607_REG_DEV_ID, &dev_id);
	if (ret)
		return ret;

	if (dev_id != adis->data->prod_id) {
		dev_err(dev, "device ID mismatch: expected 0x%04x, got 0x%04x\n",
			adis->data->prod_id, dev_id);
		return -ENODEV;
	}

	/* Read diag_stat to clear any latched faults */
	ret = __adis_read_reg_16(adis, ADIS16607_REG_DIAG_STAT, &diag_stat);
	if (ret)
		return ret;

	ret = __adis_check_status(adis);
	if (ret) {
		dev_err(dev, "status check failed: %d\n", ret);
		return ret;
	}

	/* Check that bootloader is not busy */
	ret = __adis_read_reg_16(adis, ADIS16607_REG_DIGITAL_STATUS, &digital_status);
	if (ret)
		return ret;

	if (digital_status & ADIS16607_BOOTLOADER_BUSY_MASK) {
		dev_err(dev, "bootloader is busy\n");
		return -EBUSY;
	}

	/* Route a reset-status indicator to its own pin, if reset-gpios is wired */
	if (device_property_present(dev, "reset-gpios")) {
		ret = __adis_update_bits_base(adis, ADIS16607_REG_USER_GPIO_CFG,
					      ADIS16607_RESET_GPIO_MASK,
					      FIELD_PREP(ADIS16607_RESET_GPIO_MASK, 1), 2);
		if (ret)
			return ret;
	}

	/* Enable 32-bit burst mode and DATA_CNTR - one-time, used by FIFO pops */
	ret = __adis_update_bits_base(adis, ADIS16607_REG_USER_DATA_CFG,
				      ADIS16607_BURST32_MASK | ADIS16607_DATA_CNTR_EN_MASK,
				      ADIS16607_BURST32_MASK | ADIS16607_DATA_CNTR_EN_MASK, 2);
	if (ret)
		return ret;

	/*
	 * DR_GPIO_MASK and DEV_NOK_MASK are updated here rather than written
	 * blind, so this doesn't clobber the RESET_GPIO_MASK bit possibly
	 * just set above. Per datasheet, DR pulses are what drive the FIFO
	 * controller's internal enqueue logic. DR and DEV_NOK are separate
	 * physical pins, so enabling DR_GPIO_MASK here does not put DR's
	 * full-sample-rate pulses onto the interrupt line - DEV_NOK_MASK
	 * routes the aggregate "any DIAG_STAT flag asserted" signal onto the
	 * one physically-wired interrupt pin, which is what actually fires
	 * on FIFO_THRESH_MET, at the (much lower) watermark rate, not the
	 * sample rate.
	 */
	return __adis_update_bits_base(adis, ADIS16607_REG_USER_GPIO_CFG,
				       ADIS16607_DR_GPIO_MASK | ADIS16607_DEV_NOK_MASK,
				       FIELD_PREP(ADIS16607_DR_GPIO_MASK, 1) |
				       FIELD_PREP(ADIS16607_DEV_NOK_MASK, 1), 2);
}

/*
 * Self-test callback for struct adis_data (see adis16607_data below).
 * Called by __adis_initial_startup() right after
 * adis16607_post_reset_setup().
 */
static int adis16607_self_test(struct adis *adis)
{
	struct adis16607 *st = container_of(adis, struct adis16607, adis);
	struct device *dev = &adis->spi->dev;
	u16 diag_stat;
	int ret;

	ret = adis16607_sensor_self_test(st);
	if (ret)
		return ret;

	/* Read diag_stat again to clear any faults latched during self-test */
	ret = __adis_read_reg_16(adis, ADIS16607_REG_DIAG_STAT, &diag_stat);
	if (ret)
		return ret;

	ret = __adis_check_status(adis);
	if (ret)
		dev_err(dev, "status check after self-test failed: %d\n", ret);

	return ret;
}

#ifdef CONFIG_DEBUG_FS
static int adis16607_show_firmware_id(void *arg, u64 *val)
{
	struct adis16607 *st = arg;
	u16 rev;
	int ret;

	ret = adis_read_reg_16(&st->adis, ADIS16607_REG_REV_ID, &rev);
	if (ret)
		return ret;

	*val = (rev >> 8) & 0xff;
	return 0;
}
DEFINE_DEBUGFS_ATTRIBUTE(adis16607_firmware_id_fops,
			 adis16607_show_firmware_id, NULL, "%llu\n");

static int adis16607_show_revision_id(void *arg, u64 *val)
{
	struct adis16607 *st = arg;
	u16 rev;
	int ret;

	ret = adis_read_reg_16(&st->adis, ADIS16607_REG_REV_ID, &rev);
	if (ret)
		return ret;

	*val = rev & 0xff;
	return 0;
}
DEFINE_DEBUGFS_ATTRIBUTE(adis16607_revision_id_fops,
			 adis16607_show_revision_id, NULL, "%llu\n");

static int adis16607_show_serial_number(void *arg, u64 *val)
{
	struct adis16607 *st = arg;
	u64 serial;
	u16 tmp;
	int ret;

	ret = adis_read_reg_16(&st->adis, ADIS16607_REG_SERIAL_NUM0, &tmp);
	if (ret)
		return ret;

	serial = (u64)tmp;

	ret = adis_read_reg_16(&st->adis, ADIS16607_REG_SERIAL_NUM1, &tmp);
	if (ret)
		return ret;

	serial |= ((u64)tmp << 16);

	ret = adis_read_reg_16(&st->adis, ADIS16607_REG_SERIAL_NUM2, &tmp);
	if (ret)
		return ret;

	serial |= ((u64)tmp << 32);

	*val = serial;
	return 0;
}
DEFINE_DEBUGFS_ATTRIBUTE(adis16607_serial_number_fops,
			 adis16607_show_serial_number, NULL, "0x%.8llx\n");

static int adis16607_show_dev_id(void *arg, u64 *val)
{
	struct adis16607 *st = arg;
	u16 dev_id;
	int ret;

	ret = adis_read_reg_16(&st->adis, ADIS16607_REG_DEV_ID, &dev_id);
	if (ret)
		return ret;

	*val = dev_id;
	return 0;
}
DEFINE_DEBUGFS_ATTRIBUTE(adis16607_dev_id_fops,
			 adis16607_show_dev_id, NULL, "%llu\n");

static void adis16607_debugfs_init(struct iio_dev *indio_dev)
{
	struct adis16607 *st = iio_priv(indio_dev);
	struct dentry *d = iio_get_debugfs_dentry(indio_dev);

	debugfs_create_file_unsafe("serial_number", 0400, d, st,
				   &adis16607_serial_number_fops);
	debugfs_create_file_unsafe("dev_id", 0400, d, st,
				   &adis16607_dev_id_fops);
	debugfs_create_file_unsafe("firmware_id", 0400, d, st,
				   &adis16607_firmware_id_fops);
	debugfs_create_file_unsafe("revision_id", 0400, d, st,
				   &adis16607_revision_id_fops);
}
#else
static void adis16607_debugfs_init(struct iio_dev *indio_dev)
{
}
#endif

static int adis16607_get_freq(struct adis16607 *st, u32 *freq)
{
	u16 dec = 0;
	int ret;

	mutex_lock(&st->adis.state_lock);

	ret = __adis_read_reg_16(&st->adis, ADIS16607_REG_DEC_RATE, &dec);
	if (ret)
		goto unlock;

	*freq = DIV_ROUND_CLOSEST(st->clk_freq, dec + 1);

unlock:
	mutex_unlock(&st->adis.state_lock);
	return ret;
}

static int adis16607_set_freq(struct adis16607 *st, u32 freq)
{
	u32 dec;
	int ret;

	if (!freq)
		return -EINVAL;

	mutex_lock(&st->adis.state_lock);

	dec = DIV_ROUND_CLOSEST(st->clk_freq, freq);
	if (dec)
		dec--;

	dec = min_t(u32, dec, st->info->max_dec);

	ret = __adis_write_reg_16(&st->adis, ADIS16607_REG_DEC_RATE, dec);
	if (!ret)
		st->sample_period_ns = DIV_ROUND_CLOSEST_ULL((u64)NSEC_PER_SEC * 1000ULL * (dec + 1),
							     st->clk_freq);

	mutex_unlock(&st->adis.state_lock);
	return ret;
}

static int adis16607_read_raw(struct iio_dev *indio_dev,
			      const struct iio_chan_spec *chan,
			      int *val, int *val2, long info)
{
	struct adis16607 *st = iio_priv(indio_dev);
	int ret;
	u32 tmp;

	switch (info) {
	case IIO_CHAN_INFO_RAW:
		return adis_single_conversion(indio_dev, chan, 0, val);

	case IIO_CHAN_INFO_SCALE:
		switch (chan->type) {
		case IIO_ANGL_VEL:
			*val = st->info->gyro_max_val;
			*val2 = st->info->gyro_max_scale;
			return IIO_VAL_FRACTIONAL;
		case IIO_ACCEL:
			*val = st->info->accel_max_val;
			*val2 = st->info->accel_max_scale;
			return IIO_VAL_FRACTIONAL;
		case IIO_TEMP:
			*val = st->info->temp_scale;
			return IIO_VAL_INT;
		case IIO_DELTA_ANGL:
			*val = st->info->deltang_max_val;
			*val2 = 23;
			return IIO_VAL_FRACTIONAL_LOG2;
		case IIO_DELTA_VELOCITY:
			*val = st->info->deltvel_max_val;
			*val2 = 23;
			return IIO_VAL_FRACTIONAL_LOG2;
		default:
			return -EINVAL;
		}

	case IIO_CHAN_INFO_OFFSET:
		/* Temperature: 0 raw counts = 25°C; offset = 25000 mdeg / 5 mdeg per LSB = 5000 */
		*val = 5000;
		return IIO_VAL_INT;

	case IIO_CHAN_INFO_SAMP_FREQ:
		ret = adis16607_get_freq(st, &tmp);
		if (ret)
			return ret;
		*val = tmp / 1000;
		*val2 = (tmp % 1000) * 1000;
		return IIO_VAL_INT_PLUS_MICRO;

	default:
		return -EINVAL;
	}
}

static int adis16607_write_raw(struct iio_dev *indio_dev,
			       const struct iio_chan_spec *chan,
			       int val, int val2, long info)
{
	struct adis16607 *st = iio_priv(indio_dev);
	u32 tmp;

	switch (info) {
	case IIO_CHAN_INFO_SAMP_FREQ:
		tmp = val * 1000 + val2 / 1000;
		return adis16607_set_freq(st, tmp);

	default:
		return -EINVAL;
	}
}

#define ADIS16607_MOD_CHAN(_type, _mod, _address, _si, _bits) { \
	.type = (_type), \
	.modified = 1, \
	.channel2 = (_mod), \
	.info_mask_separate = BIT(IIO_CHAN_INFO_RAW), \
	.info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SCALE), \
	.info_mask_shared_by_all = BIT(IIO_CHAN_INFO_SAMP_FREQ), \
	.address = (_address), \
	.scan_index = (_si), \
	.scan_type = { \
		.sign = 's', \
		.realbits = (_bits), \
		.storagebits = 32, \
		.endianness = IIO_BE, \
	}, \
}

#define ADIS16607_ACCEL_CHANNEL(_mod) \
	ADIS16607_MOD_CHAN(IIO_ACCEL, IIO_MOD_ ## _mod, \
			   ADIS16607_REG_ ## _mod ## _ACCEL, \
			   ADIS16607_SCAN_ACCEL_ ## _mod, 24)

#define ADIS16607_GYRO_CHANNEL(_mod) \
	ADIS16607_MOD_CHAN(IIO_ANGL_VEL, IIO_MOD_ ## _mod, \
			   ADIS16607_REG_ ## _mod ## _GYRO, \
			   ADIS16607_SCAN_GYRO_ ## _mod, 24)

#define ADIS16607_TEMP_CHANNEL() { \
	.type = IIO_TEMP, \
	.indexed = 1, \
	.channel = 0, \
	.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) | \
			      BIT(IIO_CHAN_INFO_SCALE) | \
			      BIT(IIO_CHAN_INFO_OFFSET), \
	.info_mask_shared_by_all = BIT(IIO_CHAN_INFO_SAMP_FREQ), \
	.address = ADIS16607_REG_TEMP, \
	.scan_index = ADIS16607_SCAN_TEMP, \
	.scan_type = { \
		.sign = 's', \
		.realbits = 16, \
		.storagebits = 16, \
		.endianness = IIO_BE, \
	}, \
}

#define ADIS16607_MOD_CHAN_DELTA(_type, _mod, _address, _si) { \
	.type = (_type), \
	.modified = 1, \
	.channel2 = (_mod), \
	.info_mask_separate = BIT(IIO_CHAN_INFO_RAW), \
	.info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SCALE), \
	.info_mask_shared_by_all = BIT(IIO_CHAN_INFO_SAMP_FREQ), \
	.address = (_address), \
	.scan_index = (_si), \
	.scan_type = { \
		.sign = 's', \
		.realbits = 24, \
		.storagebits = 32, \
		.endianness = IIO_BE, \
	}, \
}

#define ADIS16607_DELTANG_CHAN(_mod) \
	ADIS16607_MOD_CHAN_DELTA(IIO_DELTA_ANGL, IIO_MOD_ ## _mod, \
				 ADIS16607_REG_ ## _mod ## _DELTANG, \
				 ADIS16607_SCAN_DELTANG_ ## _mod)

#define ADIS16607_DELTVEL_CHAN(_mod) \
	ADIS16607_MOD_CHAN_DELTA(IIO_DELTA_VELOCITY, IIO_MOD_ ## _mod, \
				 ADIS16607_REG_ ## _mod ## _DELTVEL, \
				 ADIS16607_SCAN_DELTVEL_ ## _mod)

static const struct iio_chan_spec adis16607_channels[] = {
	ADIS16607_ACCEL_CHANNEL(X),
	ADIS16607_ACCEL_CHANNEL(Y),
	ADIS16607_ACCEL_CHANNEL(Z),
	ADIS16607_GYRO_CHANNEL(X),
	ADIS16607_GYRO_CHANNEL(Y),
	ADIS16607_GYRO_CHANNEL(Z),
	ADIS16607_DELTVEL_CHAN(X),
	ADIS16607_DELTVEL_CHAN(Y),
	ADIS16607_DELTVEL_CHAN(Z),
	ADIS16607_DELTANG_CHAN(X),
	ADIS16607_DELTANG_CHAN(Y),
	ADIS16607_DELTANG_CHAN(Z),
	ADIS16607_TEMP_CHANNEL(),
	IIO_CHAN_SOFT_TIMESTAMP(13),
};

static const struct iio_info adis16607_info = {
	.read_raw = adis16607_read_raw,
	.write_raw = adis16607_write_raw,
	.update_scan_mode = adis16607_update_scan_mode,
	.debugfs_reg_access = adis_debugfs_reg_access,
	.hwfifo_set_watermark = adis16607_set_watermark,
};

static const struct adis16607_sync adis16607_sync_modes[] = {
	{ ADIS16607_SYNC_DEFAULT, 0, 0 },
	{ ADIS16607_SYNC_DIRECT, 401, 8000 },
	{ ADIS16607_SYNC_SCALED, 1, 400 },
};

static const char * const adis16607_status_error_msgs[] = {
	[9] = "Boot Memory Failure",
	[11] = "Power Supply Failure",
	[12] = "Accelerometer Failure",
	[13] = "Gyroscope Failure",
};

static const struct adis_timeout adis16607_timeouts = {
	.reset_ms = 130,
	.sw_reset_ms = 50,
	.self_test_ms = 15,
};

static const struct adis_data adis16607_data = {
	.diag_stat_reg = ADIS16607_REG_DIAG_STAT,
	.diag_stat_size = 2,
	.prod_id_reg = ADIS16607_REG_DEV_ID,
	.prod_id = 0x6000,
	.unmasked_drdy = true,
	.has_fifo = true,
	.post_reset_setup = adis16607_post_reset_setup,
	.self_test = adis16607_self_test,
	.status_error_msgs = adis16607_status_error_msgs,
	.status_error_mask = BIT(9) | BIT(11) | BIT(12) | BIT(13),
	.timeouts = &adis16607_timeouts,
};

/*
 * ADIS16607-2: 450 deg/s gyro, 40g accel
 * ADIS16607-3: 2000 deg/s gyro, 40g accel
 */
#define ADIS16607_CHIP_INFO_COMMON					\
	.channels	= adis16607_channels,				\
	.num_channels	= ARRAY_SIZE(adis16607_channels),		\
	.accel_max_val	= IIO_G_TO_M_S_2(40),				\
	.accel_max_scale = 31250 << 8,					\
	.temp_scale	= 5,						\
	.deltvel_max_val = IIO_G_TO_M_S_2(40),				\
	.int_clk	= 8000,						\
	.max_dec	= 65535,					\
	.sync_mode	= adis16607_sync_modes,				\
	.num_sync	= ARRAY_SIZE(adis16607_sync_modes)

static const struct adis16607_chip_info adis16607_2_chip_info = {
	ADIS16607_CHIP_INFO_COMMON,
	.name		= "adis16607-2",
	.gyro_max_val	= IIO_DEGREE_TO_RAD(450),
	.gyro_max_scale	= 30000 << 8,
	.deltang_max_val = IIO_DEGREE_TO_RAD(450),
};

static const struct adis16607_chip_info adis16607_3_chip_info = {
	ADIS16607_CHIP_INFO_COMMON,
	.name		= "adis16607-3",
	.gyro_max_val	= IIO_DEGREE_TO_RAD(2000),
	.gyro_max_scale	= 31250 << 8,
	.deltang_max_val = IIO_DEGREE_TO_RAD(2000),
};

static int adis16607_config_clk(struct adis16607 *st)
{
	struct device *dev = &st->adis.spi->dev;
	const struct adis16607_sync *sync_mode_data = NULL;
	unsigned long ext_clk_hz;
	struct clk *clk;
	int i;

	clk = devm_clk_get_optional_enabled(dev, NULL);
	if (IS_ERR(clk))
		return PTR_ERR(clk);

	if (!clk) {
		st->clk_freq = (unsigned long)st->info->int_clk * 1000;
		st->sync_mode = ADIS16607_SYNC_DEFAULT;
		st->sample_period_ns = DIV_ROUND_CLOSEST_ULL((u64)NSEC_PER_SEC * 1000ULL,
							     st->clk_freq);
		return 0;
	}

	ext_clk_hz = clk_get_rate(clk);

	for (i = 1; i < st->info->num_sync; i++) {
		if (ext_clk_hz >= st->info->sync_mode[i].min_rate &&
		    ext_clk_hz <= st->info->sync_mode[i].max_rate) {
			sync_mode_data = &st->info->sync_mode[i];
			break;
		}
	}

	if (!sync_mode_data)
		return dev_err_probe(dev, -EINVAL,
				     "Clk rate: %lu not in a valid range\n",
				     ext_clk_hz);

	st->sync_mode = sync_mode_data->sync_mode;
	/* DIRECT: external clock is the effective internal rate */
	st->clk_freq = ext_clk_hz * 1000;

	/* Configure sync via USER_GPIO_CFG and USER_SYNC registers */
	if (st->sync_mode != ADIS16607_SYNC_DEFAULT) {
		int ret;

		/* Enable sync GPIO function */
		ret = adis_update_bits_base(&st->adis, ADIS16607_REG_USER_GPIO_CFG,
					    ADIS16607_SYNC_GPIO_MASK,
					    FIELD_PREP(ADIS16607_SYNC_GPIO_MASK, 1), 2);
		if (ret)
			return ret;

		/* Set sync mode in USER_SYNC register */
		if (st->sync_mode == ADIS16607_SYNC_SCALED) {
			u16 sync_scale;

			sync_scale = st->info->int_clk / ext_clk_hz;
			ret = adis_write_reg_16(&st->adis, ADIS16607_REG_USER_SYNC,
						ADIS16607_SYNC_MODE_MASK | sync_scale);
			if (ret)
				return ret;

			/* SCALED: effective rate is int_clk (ext_clk × sync_scale ≈ int_clk) */
			st->clk_freq = (unsigned long)st->info->int_clk * 1000;
		}
	}

	/* Default dec=0 after reset: period = 1 / clk_freq */
	st->sample_period_ns = DIV_ROUND_CLOSEST_ULL((u64)NSEC_PER_SEC * 1000ULL,
						      st->clk_freq);

	return 0;
}

static int adis16607_probe(struct spi_device *spi)
{
	struct device *dev = &spi->dev;
	struct iio_dev *indio_dev;
	struct adis16607 *st;
	int ret;

	indio_dev = devm_iio_device_alloc(dev, sizeof(*st));
	if (!indio_dev)
		return -ENOMEM;

	st = iio_priv(indio_dev);
	st->info = spi_get_device_match_data(spi);
	if (!st->info)
		return -EINVAL;

	indio_dev->name = st->info->name;
	indio_dev->channels = st->info->channels;
	indio_dev->num_channels = st->info->num_channels;
	indio_dev->info = &adis16607_info;
	indio_dev->modes = INDIO_DIRECT_MODE;

	st->fifo_watermark = ADIS16607_MAX_FIFO_WM;

	st->adis.ops = &adis16607_ops;

	ret = devm_regulator_get_enable(dev, "vdd");
	if (ret)
		return dev_err_probe(dev, ret, "Failed to get vdd regulator\n");

	ret = adis_init(&st->adis, indio_dev, spi, &adis16607_data);
	if (ret)
		return ret;

	ret = __adis_initial_startup(&st->adis);
	if (ret)
		return ret;

	ret = adis16607_config_clk(st);
	if (ret)
		return ret;

	/*
	 * Register fifo_buf cleanup before buffer/trigger setup so that on
	 * devm teardown (LIFO order) the IRQ handler is removed first, then
	 * fifo_buf is freed, avoiding a use-after-free in the handler.
	 */
	ret = devm_add_action_or_reset(dev, adis16607_fifo_buf_cleanup, st);
	if (ret)
		return ret;

	ret = devm_adis_setup_buffer_and_trigger_with_attrs(&st->adis, indio_dev,
		adis16607_trigger_handler, &adis16607_buffer_ops,
		adis16607_fifo_attributes);
	if (ret)
		return ret;

	ret = devm_iio_device_register(dev, indio_dev);
	if (ret)
		return ret;

	adis16607_debugfs_init(indio_dev);

	return 0;
}

static const struct spi_device_id adis16607_id[] = {
	{ "adis16607-2", (kernel_ulong_t)&adis16607_2_chip_info },
	{ "adis16607-3", (kernel_ulong_t)&adis16607_3_chip_info },
	{ }
};
MODULE_DEVICE_TABLE(spi, adis16607_id);

static const struct of_device_id adis16607_of_match[] = {
	{ .compatible = "adi,adis16607-2", .data = &adis16607_2_chip_info },
	{ .compatible = "adi,adis16607-3", .data = &adis16607_3_chip_info },
	{ }
};
MODULE_DEVICE_TABLE(of, adis16607_of_match);

static struct spi_driver adis16607_driver = {
	.driver = {
		.name = "adis16607",
		.of_match_table = adis16607_of_match,
	},
	.probe = adis16607_probe,
	.id_table = adis16607_id,
};
module_spi_driver(adis16607_driver);

MODULE_AUTHOR("Radu Sabau <radu.sabau@analog.com>");
MODULE_DESCRIPTION("Analog Devices ADIS16607 IMU driver");
MODULE_IMPORT_NS("IIO_ADISLIB");
MODULE_LICENSE("GPL");
