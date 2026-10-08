// SPDX-License-Identifier: GPL-2.0-only
/*
 * ADIS16607 IMU driver
 *
 * Copyright 2026 Analog Devices Inc.
 */
#include <linux/bitfield.h>
#include <linux/bitops.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/iio/buffer.h>
#include <linux/iio/iio.h>
#include <linux/iio/imu/adis.h>
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
#define ADIS16607_REG_USER_GPIO_CFG	0x2F
#define ADIS16607_REG_SPI_HALFDUPLEX	0x32
#define ADIS16607_REG_SOFT_RESET	0x36
#define ADIS16607_REG_SELF_TEST		0x39
#define ADIS16607_REG_DEC_RATE		0x3A
#define ADIS16607_REG_DIGITAL_STATUS	0x4E

/* SPI Half-Duplex lock value */
#define ADIS16607_LOCK_SPI_HALFDUPLEX	0xB4B4

/* Masks */
#define ADIS16607_RESET_GPIO_MASK	GENMASK(2, 0)
#define ADIS16607_BOOTLOADER_BUSY_MASK	BIT(0)

/* Self-test registers and masks */
#define ADIS16607_REG_SELF_TEST_DATA(x)	((x) + 0x23)
#define ADIS16607_SNSR_SELF_TEST_MASK	BIT(6)
#define ADIS16607_ST_FORCE_MASK		BIT(7)

/* Self-test delta check limits */
#define ADIS16607_ACCEL_XY_DELTA_MAX	260
#define ADIS16607_ACCEL_Z_DELTA_MAX	4000
#define ADIS16607_GYRO_DELTA_MAX	2600

struct adis16607_chip_info {
	const struct iio_chan_spec *channels;
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
};

struct adis16607 {
	const struct adis16607_chip_info *info;
	struct adis adis;
	unsigned long clk_freq;	/* millihertz */
	u64 sample_period_ns;
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
 * bootloader-busy check, and (if wired up) a reset-status GPIO output.
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

	return 0;
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
	.debugfs_reg_access = adis_debugfs_reg_access,
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
	.max_dec	= 65535

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

/*
 * No external clock sync support yet: always run on the internal clock.
 * Default dec=0 after reset, so period = 1 / clk_freq.
 */
static int adis16607_config_clk(struct adis16607 *st)
{
	st->clk_freq = (unsigned long)st->info->int_clk * 1000;
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
