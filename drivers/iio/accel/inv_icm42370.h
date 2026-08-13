/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) 2020 Invensense, Inc.
 * Copyright (C) 2026 Axis Communications AB
 */

#ifndef INV_ICM42370_H_
#define INV_ICM42370_H_

#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/mutex.h>
#include <linux/regmap.h>
#include <linux/types.h>
#include <linux/regulator/consumer.h>

#include <linux/iio/common/inv_sensors_timestamp.h>
#include <linux/iio/iio.h>

#include "inv_icm42370_buffer.h"

#define INV_ICM42370_SENSOR_CONF_INIT { -1, -1, -1, -1 }

/* Registers in USER BANK 1 */
#define INV_ICM42370_REG_MCLK_RDY 0x0
#define INV_ICM42370_REG_DEVICE_CONFIG 0x01
#define INV_ICM42370_REG_SIGNAL_PATH_RESET 0x02
#define INV_ICM42370_REG_DRIVE_CONFIG1 0x03
#define INV_ICM42370_REG_DRIVE_CONFIG2 0x04
#define INV_ICM42370_REG_DRIVE_CONFIG3 0x05
#define INV_ICM42370_REG_INT_CONFIG 0x06
#define INV_ICM42370_REG_TEMP_DATA1 0x09
#define INV_ICM42370_REG_TEMP_DATA0 0x0A
#define INV_ICM42370_REG_ACCEL_DATA_X1 0x0B
#define INV_ICM42370_REG_ACCEL_DATA_X0 0x0C
#define INV_ICM42370_REG_ACCEL_DATA_Y1 0x0D
#define INV_ICM42370_REG_ACCEL_DATA_Y0 0x0E
#define INV_ICM42370_REG_ACCEL_DATA_Z1 0x0F
#define INV_ICM42370_REG_ACCEL_DATA_Z0 0x10
#define INV_ICM42370_REG_ACCEL_CONFIG0 0x21
#define INV_ICM42370_REG_PWR_MGMT0 0x1F
#define INV_ICM42370_REG_INTF_CONFIG6 0x23
#define INV_ICM42370_REG_FIFO_CONFIG1 0x28
#define INV_ICM42370_REG_FIFO_WATERMARK 0x29
#define INV_ICM42370_REG_INT_SOURCE0 0x2B
#define INV_ICM42370_REG_INT_STATUS 0x3A
#define INV_ICM42370_REG_TEMP_CONFIG0 0x34
#define INV_ICM42370_REG_INTF_CONFIG0 0x35
#define INV_ICM42370_REG_FIFO_COUNT 0x3D
#define INV_ICM42370_REG_FIFO_DATA 0x3F
#define INV_ICM42370_REG_WHO_AM_I 0x75
#define INV_ICM42370_REG_BLK_SEL_W 0x79
#define INV_ICM42370_REG_MADDR_W 0x7A
#define INV_ICM42370_REG_M_W 0x7B
#define INV_ICM42370_REG_BLK_SEL_R 0x7C
#define INV_ICM42370_REG_MADDR_R 0x7D
#define INV_ICM42370_REG_M_R 0x7E

#define INV_ICM42370_DRIVE_CONFIG1_I3C_DDR_MASK GENMASK(5, 3)
#define INV_ICM42370_DRIVE_CONFIG1_I3C_DDR(_rate) \
	FIELD_PREP(INV_ICM42370_DRIVE_CONFIG1_I3C_DDR_MASK, (_rate))

#define INV_ICM42370_DRIVE_CONFIG1_I3C_SDR_MASK GENMASK(2, 0)
#define INV_ICM42370_DRIVE_CONFIG1_I3C_SDR(_rate) \
	FIELD_PREP(INV_ICM42370_DRIVE_CONFIG1_I3C_SDR_MASK, (_rate))

#define INV_ICM42370_DRIVE_CONFIG2_I2C_MASK GENMASK(5, 3)
#define INV_ICM42370_DRIVE_CONFIG2_I2C(_rate) \
	FIELD_PREP(INV_ICM42370_DRIVE_CONFIG2_I2C_MASK, (_rate))

#define INV_ICM42370_DRIVE_CONFIG3_SPI_MASK GENMASK(2, 0)
#define INV_ICM42370_DRIVE_CONFIG3_SPI(_rate) \
	FIELD_PREP(INV_ICM42370_DRIVE_CONFIG3_SPI_MASK, (_rate))

#define INV_ICM42370_SIGNAL_PATH_RESET_FIFO_FLUSH BIT(2)
#define INV_ICM42370_FIFO_CONFIG_MODE_MASK BIT(1)
#define INV_ICM42370_FIFO_CONFIG_BYPASS_MASK BIT(0)
#define INV_ICM42370_FIFO_CONFIG_STREAM \
	FIELD_PREP(INV_ICM42370_FIFO_CONFIG_MODE_MASK, 0)
#define INV_ICM42370_FIFO_CONFIG_STOP_ON_FULL \
	FIELD_PREP(INV_ICM42370_FIFO_CONFIG_MODE_MASK, 1)
#define INV_ICM42370_FIFO_CONFIG_BYPASS \
	FIELD_PREP(INV_ICM42370_FIFO_CONFIG_BYPASS_MASK, 1)

#define INV_ICM42370_INT_CONFIG_INT2_LATCHED BIT(5)
#define INV_ICM42370_INT_CONFIG_INT2_PUSH_PULL BIT(4)
#define INV_ICM42370_INT_CONFIG_INT2_ACTIVE_HIGH BIT(3)
#define INV_ICM42370_INT_CONFIG_INT2_ACTIVE_LOW 0x00
#define INV_ICM42370_INT_CONFIG_INT1_LATCHED BIT(2)
#define INV_ICM42370_INT_CONFIG_INT1_PUSH_PULL BIT(1)
#define INV_ICM42370_INT_CONFIG_INT1_ACTIVE_HIGH BIT(0)
#define INV_ICM42370_INT_CONFIG_INT1_ACTIVE_LOW 0x00

#define INV_ICM42370_INT_STATUS_FIFO_THS BIT(2)
#define INV_ICM42370_INT_STATUS_FIFO_FULL BIT(1)
#define INV_ICM42370_INT_SOURCE0_FIFO_THS_INT1_EN BIT(2)

/* Registers in MREG1 USER BANK 1 */
#define INV_ICM42370_REG_TMST_CONFIG1 0x00
#define INV_ICM42370_REG_FIFO_CONFIG5 0x01
#define INV_ICM42370_REG_FIFO_CONFIG6 0x02
#define INV_ICM42370_REG_INT_CONFIG1 0x05
#define INV_ICM42370_REG_OFFSET_USER4 0x52
#define INV_ICM42370_REG_OFFSET_USER5 0x53
#define INV_ICM42370_REG_OFFSET_USER6 0x54
#define INV_ICM42370_REG_OFFSET_USER7 0x55
#define INV_ICM42370_REG_OFFSET_USER8 0x56

#define INV_ICM42370_TMST_CONFIG_TMST_DELTA_EN BIT(2)
#define INV_ICM42370_TMST_CONFIG_TMST_EN BIT(0)

#define INV_ICM42370_FIFO_CONFIG5_WM_GT_TH BIT(5)
#define INV_ICM42370_FIFO_CONFIG5_RESUME_PARTIAL_RD BIT(4)
#define INV_ICM42370_FIFO_CONFIG5_ACCEL_EN BIT(0)

#define INV_ICM42370_INT_CONFIG1_ASYNC_RESET BIT(4)
#define INV_ICM42370_FIFO_FLUSH_BIT_MASK BIT(2)
#define INV_ICM42370_INTF_CONFIG0_FIFO_COUNT_ENDIAN BIT(5)
#define INV_ICM42370_INTF_CONFIG0_SENSOR_DATA_ENDIAN BIT(4)

#define INV_ICM42370_FIFO_WATERMARK_VAL(_wm) cpu_to_le16((_wm) & GENMASK(11, 0))

/* FIFO is 2048 bytes, let 12 samples for reading latency */
#define INV_ICM42370_FIFO_WATERMARK_MAX (2048 - 12 * 16)

#define INV_ICM42370_PWR_MGMT0(_mode) FIELD_PREP(GENMASK(1, 0), (_mode))
#define INV_ICM42370_ACCEL_CONFIG0_FS(_fs) FIELD_PREP(GENMASK(6, 5), (_fs))
#define INV_ICM42370_ACCEL_CONFIG0_ODR(_odr) FIELD_PREP(GENMASK(3, 0), (_odr))
#define INV_ICM42370_TEMP_FILT_BW_DLPF(_dlpf) FIELD_PREP(GENMASK(6, 4), (_dlpf))

#define INV_ICM42370_MCLK_RDY_BIT BIT(3)
#define INV_ICM42370_SOFT_RESET_BIT BIT(4)
#define INV_ICM42370_WHOAMI_VALUE 0x0D
#define INV_ICM42370_ACCEL_MODE_LN 0x03
#define INV_ICM42370_DATA_INVALID -32768
#define INV_ICM42370_ACCEL_STARTUP_TIME_MS 10

#define INV_ICM42370_TEMP_CHAN(_index) \
	{								\
		.type = IIO_TEMP,					\
		.info_mask_separate =					\
			BIT(IIO_CHAN_INFO_RAW) |			\
			BIT(IIO_CHAN_INFO_OFFSET) |			\
			BIT(IIO_CHAN_INFO_SCALE),			\
		.scan_index = _index,					\
		.scan_type = {						\
			.sign = 's',					\
			.realbits = 16,					\
			.storagebits = 16,				\
		},							\
	}

#define INV_ICM42370_ACCEL_CHAN(_modifier, _index, _ext_info) \
	{								\
		.type = IIO_ACCEL,					\
		.modified = 1,						\
		.channel2 = _modifier,					\
		.info_mask_separate =					\
			BIT(IIO_CHAN_INFO_RAW) |			\
			BIT(IIO_CHAN_INFO_CALIBBIAS),			\
		.info_mask_shared_by_type =				\
			BIT(IIO_CHAN_INFO_SCALE),			\
		.info_mask_shared_by_type_available =			\
			BIT(IIO_CHAN_INFO_SCALE) |			\
			BIT(IIO_CHAN_INFO_CALIBBIAS),			\
		.info_mask_shared_by_all =				\
			BIT(IIO_CHAN_INFO_SAMP_FREQ),			\
		.info_mask_shared_by_all_available =			\
			BIT(IIO_CHAN_INFO_SAMP_FREQ),			\
		.scan_index = _index,					\
		.scan_type = {						\
			.sign = 's',					\
			.realbits = 16,					\
			.storagebits = 16,				\
			.endianness = IIO_BE,				\
		},							\
		.ext_info = _ext_info,					\
	}

static const int inv_icm42370_accel_odr[] = {
	1, 562500,
	3, 125000,
	6, 250000,
	12, 500000,
	25, 0,
	50, 0,
	100, 0,
	200, 0,
	400, 0,
	800, 0,
	1600, 0,
};

enum inv_icm42370_chip {
	INV_CHIP_INVALID,
	INV_CHIP_ICM42370,
	INV_CHIP_NB,
};

enum inv_icm42370_accel_scan {
	INV_ICM42370_ACCEL_SCAN_X,
	INV_ICM42370_ACCEL_SCAN_Y,
	INV_ICM42370_ACCEL_SCAN_Z,
	INV_ICM42370_ACCEL_SCAN_TEMP,
	INV_ICM42370_ACCEL_SCAN_TIMESTAMP,
};

enum inv_icm42370_sensor_mode {
	INV_ICM42370_SENSOR_MODE_OFF,
	INV_ICM42370_SENSOR_MODE_STANDBY,
	INV_ICM42370_SENSOR_MODE_LOW_POWER,
	INV_ICM42370_SENSOR_MODE_LOW_NOISE,
	INV_ICM42370_SENSOR_MODE_NB,
};

enum inv_icm42370_filter {
	/* Low-Noise mode sensor data filter bandwidth */
	INV_ICM42370_UI_FILT_BW_LP_FILTER_BYPASSED,
	/* Low-Power mode sensor data filter (averaging) */
	INV_ICM42370_FILTER_AVG_2X,
	INV_ICM42370_FILTER_AVG_4X,
	INV_ICM42370_FILTER_AVG_8X,
	INV_ICM42370_FILTER_AVG_16X,
	INV_ICM42370_FILTER_AVG_32X,
	INV_ICM42370_FILTER_AVG_64X,
	INV_ICM42370_FILTER_AVG_NB,
};

enum inv_icm42370_slew_rate {
	INV_ICM42370_SLEW_RATE_20_60NS,
	INV_ICM42370_SLEW_RATE_12_36NS,
	INV_ICM42370_SLEW_RATE_6_19NS,
	INV_ICM42370_SLEW_RATE_4_14NS,
	INV_ICM42370_SLEW_RATE_2_8NS,
	INV_ICM42370_SLEW_RATE_INF_2NS,
};

enum inv_icm42370_accel_fs {
	INV_ICM42370_ACCEL_FS_16G,
	INV_ICM42370_ACCEL_FS_8G,
	INV_ICM42370_ACCEL_FS_4G,
	INV_ICM42370_ACCEL_FS_2G,
	INV_ICM42370_ACCEL_FS_NB,
};

enum inv_icm42370_odr {
	INV_ICM42370_ODR_1_6KHZ_LN = 5,
	INV_ICM42370_ODR_800HZ_LN,
	INV_ICM42370_ODR_400HZ,
	INV_ICM42370_ODR_200HZ,
	INV_ICM42370_ODR_100HZ,
	INV_ICM42370_ODR_50HZ,
	INV_ICM42370_ODR_25HZ,
	INV_ICM42370_ODR_12_5HZ,
	INV_ICM42370_ODR_6_25HZ_LP,
	INV_ICM42370_ODR_3_125HZ_LP,
	INV_ICM42370_ODR_1_5625HZ_LP,
	INV_ICM42370_ODR_NB,
};

enum inv_icm42370_mregs {
	INV_ICM42370_MREG1,
	INV_ICM42370_MREG2 = 0x28,
	INV_ICM42370_MREG3 = 0x50,
};

enum inv_icm42370_temp_filter {
	INV_ICM42370_TEMP_FILT_BW_DLPF_BYPASS,
	INV_ICM42370_TEMP_FILT_BW_DLPF_180HZ,
	INV_ICM42370_TEMP_FILT_BW_DLPF_72HZ,
	INV_ICM42370_TEMP_FILT_BW_DLPF_34HZ,
	INV_ICM42370_TEMP_FILT_BW_DLPF_16HZ,
	INV_ICM42370_TEMP_FILT_BW_DLPF_8HZ,
	INV_ICM42370_TEMP_FILT_BW_DLPF_4HZ,
	INV_ICM42370_TEMP_FILT_BW_DLPF_NB,
};

struct inv_icm42370_conf {
	int mode;
	int fs;
	int odr;
	int filter;
};

/**
 * struct inv_icm42370_data - driver state variables
 * @lock:		lock for serializing multiple register access.
 * @name:		chip name.
 * @map:		regmap pointer.
 * @indio_accel:	accelerometer IIO device.
 * @timestamp:		interrupt timestamp.
 * @orientation:	sensor chip orientation relative to main hardware.
 * @fifo:		FIFO state and configuration.
 * @chip:		chip identifier.
 * @conf:		chip sensors configurations.
 * @filter:		sensor filter.
 * @scales:		table of scales.
 * @scales_len:		length (nb of items) of the scales table.
 * @power_mode:		sensor requested power mode (for common frequencies).
 * @accel_calibbias:	accelerometer calibration bias for X, Y, and Z axes.
 * @ts:			timestamp module states.
 * @buffer:		buffer for reading data registers, aligned for DMA.
 */
struct inv_icm42370_data {
	struct mutex lock;
	const char *name;
	struct regmap *map;
	struct iio_dev *indio_accel;
	s64 timestamp;
	struct iio_mount_matrix orientation;
	struct inv_icm42370_fifo fifo;
	enum inv_icm42370_chip chip;
	struct inv_icm42370_conf conf;
	enum inv_icm42370_filter filter;
	const int *scales;
	size_t scales_len;
	enum inv_icm42370_sensor_mode power_mode;
	s16 accel_calibbias[3];
	struct inv_sensors_timestamp ts;
	u8 buffer[2] __aligned(IIO_DMA_MINALIGN);
};

static struct inv_icm42370_conf inv_icm42370_default_conf = {
	.mode = INV_ICM42370_SENSOR_MODE_LOW_NOISE,
	.fs = INV_ICM42370_ACCEL_FS_16G,
	.odr = INV_ICM42370_ODR_400HZ,
	.filter = INV_ICM42370_FILTER_AVG_16X,
};

typedef int (*inv_icm42370_bus_setup)(struct inv_icm42370_data *);
extern const struct regmap_config inv_icm42370_regmap_config;

u32 inv_icm42370_odr_to_period(enum inv_icm42370_odr odr);

int inv_icm42370_core_probe(struct regmap *regmap, int chip, int irq,
			    inv_icm42370_bus_setup bus_setup);

struct iio_dev *inv_icm42370_accel_init(struct iio_dev *indio_dev,
					struct inv_icm42370_data *data);

int inv_icm42370_set_accel_conf(struct inv_icm42370_data *data,
				struct inv_icm42370_conf *conf,
				unsigned int *sleep_ms);

int inv_icm42370_accel_parse_fifo(struct iio_dev *indio_dev);
int inv_icm42370_mreg_write(struct inv_icm42370_data *data, u8 bank, u8 addr, u8 val);
int inv_icm42370_mreg_read(struct inv_icm42370_data *data, u8 bank, u8 addr, u8 *val);
int inv_icm42370_set_pwr_mgmt0(struct inv_icm42370_data *data,
			       enum inv_icm42370_sensor_mode accel,
			       unsigned int *sleep_ms);

#endif
