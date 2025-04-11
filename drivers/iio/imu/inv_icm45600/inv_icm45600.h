/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) 2025 Invensense, Inc.
 */

#ifndef INV_ICM45600_H_
#define INV_ICM45600_H_

#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/iio/common/inv_sensors_timestamp.h>
#include <linux/iio/iio.h>
#include <linux/mutex.h>
#include <linux/pm.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>

#include "inv_icm45600_buffer.h"

#define INV_ICM45600_REG_GET_BANK(_r)	FIELD_GET(GENMASK(15, 8), (_r))
#define INV_ICM45600_REG_GET_ADDR(_r)	FIELD_GET(GENMASK(7, 0), (_r))

enum inv_icm45600_chip {
	INV_CHIP_INVALID,
	INV_CHIP_ICM45605,
	INV_CHIP_ICM45686,
	INV_CHIP_ICM45688P,
	INV_CHIP_ICM45608,
	INV_CHIP_ICM45634,
	INV_CHIP_ICM45689,
	INV_CHIP_ICM45606,
	INV_CHIP_ICM45687,
	INV_CHIP_NB,
};

enum inv_icm45600_sensor_mode {
	INV_ICM45600_SENSOR_MODE_OFF,
	INV_ICM45600_SENSOR_MODE_STANDBY,
	INV_ICM45600_SENSOR_MODE_LOW_POWER,
	INV_ICM45600_SENSOR_MODE_LOW_NOISE,
	INV_ICM45600_SENSOR_MODE_NB,
};

/* gyroscope fullscale values */
enum inv_icm45600_gyro_fs {
	INV_ICM45600_GYRO_FS_2000DPS,
	INV_ICM45600_GYRO_FS_1000DPS,
	INV_ICM45600_GYRO_FS_500DPS,
	INV_ICM45600_GYRO_FS_250DPS,
	INV_ICM45600_GYRO_FS_125DPS,
	INV_ICM45600_GYRO_FS_62_5DPS,
	INV_ICM45600_GYRO_FS_31_25DPS,
	INV_ICM45600_GYRO_FS_15_625DPS,
	INV_ICM45600_GYRO_FS_NB,
};
enum inv_icm45686_gyro_fs {
	INV_ICM45686_GYRO_FS_4000DPS,
	INV_ICM45686_GYRO_FS_2000DPS,
	INV_ICM45686_GYRO_FS_1000DPS,
	INV_ICM45686_GYRO_FS_500DPS,
	INV_ICM45686_GYRO_FS_250DPS,
	INV_ICM45686_GYRO_FS_125DPS,
	INV_ICM45686_GYRO_FS_62_5DPS,
	INV_ICM45686_GYRO_FS_31_25DPS,
	INV_ICM45686_GYRO_FS_15_625DPS,
	INV_ICM45686_GYRO_FS_NB,
};

/* accelerometer fullscale values */
enum inv_icm45600_accel_fs {
	INV_ICM45600_ACCEL_FS_16G,
	INV_ICM45600_ACCEL_FS_8G,
	INV_ICM45600_ACCEL_FS_4G,
	INV_ICM45600_ACCEL_FS_2G,
	INV_ICM45600_ACCEL_FS_NB,
};
enum inv_icm45686_accel_fs {
	INV_ICM45686_ACCEL_FS_32G,
	INV_ICM45686_ACCEL_FS_16G,
	INV_ICM45686_ACCEL_FS_8G,
	INV_ICM45686_ACCEL_FS_4G,
	INV_ICM45686_ACCEL_FS_2G,
	INV_ICM45686_ACCEL_FS_NB,
};

/* ODR suffixed by LN or LP are Low-Noise or Low-Power mode only */
enum inv_icm45600_odr {
	INV_ICM45600_ODR_6400HZ_LN = 0x03,
	INV_ICM45600_ODR_3200HZ_LN,
	INV_ICM45600_ODR_1600HZ_LN,
	INV_ICM45600_ODR_800HZ_LN,
	INV_ICM45600_ODR_400HZ,
	INV_ICM45600_ODR_200HZ,
	INV_ICM45600_ODR_100HZ,
	INV_ICM45600_ODR_50HZ,
	INV_ICM45600_ODR_25HZ,
	INV_ICM45600_ODR_12_5HZ,
	INV_ICM45600_ODR_6_25HZ_LP,
	INV_ICM45600_ODR_3_125HZ_LP,
	INV_ICM45600_ODR_1_5625HZ_LP,
	INV_ICM45600_ODR_NB,
};

struct inv_icm45600_sensor_conf {
	int mode;
	int fs;
	int odr;
	int filter;
};
#define INV_ICM45600_SENSOR_CONF_INIT		{-1, -1, -1, -1}

struct inv_icm45600_conf {
	struct inv_icm45600_sensor_conf gyro;
	struct inv_icm45600_sensor_conf accel;
};

struct inv_icm45600_suspended {
	enum inv_icm45600_sensor_mode gyro;
	enum inv_icm45600_sensor_mode accel;
};

/**
 *  struct inv_icm45600_state - driver state variables
 *  @lock:		lock for serializing multiple registers access.
 *  @chip:		chip identifier.
 *  @name:		chip name.
 *  @map:		regmap pointer.
 *  @vdd_supply:	VDD voltage regulator for the chip.
 *  @vddio_supply:	I/O voltage regulator for the chip.
 *  @orientation:	sensor chip orientation relative to main hardware.
 *  @conf:		chip sensors configurations.
 *  @suspended:		suspended sensors configuration.
 *  @indio_gyro:	gyroscope IIO device.
 *  @indio_accel:	accelerometer IIO device.
 *  @timestamp:		interrupt timestamps.
 *  @fifo:		FIFO management structure.
 *  @buffer:		data transfer buffer aligned for DMA.
 */
struct inv_icm45600_state {
	struct mutex lock;
	enum inv_icm45600_chip chip;
	const char *name;
	struct regmap *map;
	struct regulator *vdd_supply;
	struct regulator *vddio_supply;
	struct iio_mount_matrix orientation;
	struct inv_icm45600_conf conf;
	struct inv_icm45600_suspended suspended;
	struct iio_dev *indio_gyro;
	struct iio_dev *indio_accel;
	struct {
		int64_t gyro;
		int64_t accel;
	} timestamp;
	struct inv_icm45600_fifo fifo;
	uint8_t buffer[2] __aligned(IIO_DMA_MINALIGN);
};


/**
 * struct inv_icm45600_sensor_state - sensor state variables
 * @scales:		table of scales.
 * @scales_len:		length (nb of items) of the scales table.
 * @power_mode:		sensor requested power mode (for common frequencies)
 * @ts:			timestamp module states.
 */
struct inv_icm45600_sensor_state {
	const int *scales;
	size_t scales_len;
	enum inv_icm45600_sensor_mode power_mode;
	struct inv_sensors_timestamp ts;
};

/* Virtual register addresses: @bank on MSB (16 bits), @address on LSB */

/* Indirect register access */
#define INV_ICM45600_REG_IREG_ADDR			0x7C
#define INV_ICM45600_REG_IREG_DATA			0x7E

/* Direct acces registers */
#define INV_ICM45600_REG_MISC2				0x007F
#define INV_ICM45600_MISC2_SOFT_RESET			BIT(1)

#define INV_ICM45600_REG_DRIVE_CONFIG0			0x0032
#define INV_ICM45600_DRIVE_CONFIG0_I2C_MASK		GENMASK(6, 4)
#define INV_ICM45600_DRIVE_CONFIG0_I2C(_rate)		\
		FIELD_PREP(INV_ICM45600_DRIVE_CONFIG0_I2C_MASK, (_rate))
#define INV_ICM45600_I2C_SLEW_RATE_7NS				\
		INV_ICM45600_DRIVE_CONFIG0_I2C(2)
#define INV_ICM45600_I2C_SLEW_RATE_20NS				\
		INV_ICM45600_DRIVE_CONFIG0_I2C(0)
#define INV_ICM45600_DRIVE_CONFIG0_SPI_MASK		GENMASK(3, 1)
#define INV_ICM45600_DRIVE_CONFIG0_SPI(_rate)		\
		FIELD_PREP(INV_ICM45600_DRIVE_CONFIG0_SPI_MASK, (_rate))
#define INV_ICM45600_SPI_SLEW_RATE_0_5NS			\
		INV_ICM45600_DRIVE_CONFIG0_SPI(6)
#define INV_ICM45600_SPI_SLEW_RATE_4NS				\
		INV_ICM45600_DRIVE_CONFIG0_SPI(5)
#define INV_ICM45600_SPI_SLEW_RATE_5NS				\
		INV_ICM45600_DRIVE_CONFIG0_SPI(4)
#define INV_ICM45600_SPI_SLEW_RATE_7NS				\
		INV_ICM45600_DRIVE_CONFIG0_SPI(3)
#define INV_ICM45600_SPI_SLEW_RATE_10NS				\
		INV_ICM45600_DRIVE_CONFIG0_SPI(2)
#define INV_ICM45600_SPI_SLEW_RATE_14NS				\
		INV_ICM45600_DRIVE_CONFIG0_SPI(1)
#define INV_ICM45600_SPI_SLEW_RATE_38NS				\
		INV_ICM45600_DRIVE_CONFIG0_SPI(0)

#define INV_ICM45600_REG_DRIVE_CONFIG1			0x0033
#define INV_ICM45600_DRIVE_CONFIG1_I3C_DDR_SLEW_MASK	GENMASK(5, 3)
#define INV_ICM45600_DRIVE_CONFIG1_I3C_DDR_SLEW(_rate)		\
		FIELD_PREP(INV_ICM45600_DRIVE_CONFIG1_I3C_DDR_SLEW_MASK, (_rate))
#define INV_ICM45600_I3C_DDR_SLEW_0_5NS				\
		INV_ICM45600_DRIVE_CONFIG1_I3C_DDR_SLEW(6)
#define INV_ICM45600_I3C_DDR_SLEW_4NS				\
		INV_ICM45600_DRIVE_CONFIG1_I3C_DDR_SLEW(5)
#define INV_ICM45600_I3C_DDR_SLEW_5NS				\
		INV_ICM45600_DRIVE_CONFIG1_I3C_DDR_SLEW(4)
#define INV_ICM45600_I3C_DDR_SLEW_7NS				\
		INV_ICM45600_DRIVE_CONFIG1_I3C_DDR_SLEW(3)
#define INV_ICM45600_I3C_DDR_SLEW_10NS				\
		INV_ICM45600_DRIVE_CONFIG1_I3C_DDR_SLEW(2)
#define INV_ICM45600_I3C_DDR_SLEW_14NS				\
		INV_ICM45600_DRIVE_CONFIG1_I3C_DDR_SLEW(1)
#define INV_ICM45600_I3C_DDR_SLEW_38NS				\
		INV_ICM45600_DRIVE_CONFIG1_I3C_DDR_SLEW(0)
#define INV_ICM45600_DRIVE_CONFIG1_I3C_SDR_SLEW_MASK	GENMASK(2, 0)
#define INV_ICM45600_DRIVE_CONFIG1_I3C_SDR_SLEW(_rate)		\
		FIELD_PREP(INV_ICM45600_DRIVE_CONFIG1_I3C_SDR_SLEW_MASK, (_rate))
#define INV_ICM45600_I3C_SDR_SLEW_0_5NS				\
		INV_ICM45600_DRIVE_CONFIG1_I3C_SDR_SLEW(6)
#define INV_ICM45600_I3C_SDR_SLEW_4NS				\
		INV_ICM45600_DRIVE_CONFIG1_I3C_SDR_SLEW(5)
#define INV_ICM45600_I3C_SDR_SLEW_5NS				\
		INV_ICM45600_DRIVE_CONFIG1_I3C_SDR_SLEW(4)
#define INV_ICM45600_I3C_SDR_SLEW_7NS				\
		INV_ICM45600_DRIVE_CONFIG1_I3C_SDR_SLEW(3)
#define INV_ICM45600_I3C_SDR_SLEW_10NS				\
		INV_ICM45600_DRIVE_CONFIG1_I3C_SDR_SLEW(2)
#define INV_ICM45600_I3C_SDR_SLEW_14NS				\
		INV_ICM45600_DRIVE_CONFIG1_I3C_SDR_SLEW(1)
#define INV_ICM45600_I3C_SDR_SLEW_38NS				\
		INV_ICM45600_DRIVE_CONFIG1_I3C_SDR_SLEW(0)

#define INV_ICM45600_REG_INT1_CONFIG2			0x0018
#define INV_ICM45600_INT1_CONFIG2_PUSH_PULL		BIT(2)
#define INV_ICM45600_INT1_CONFIG2_LATCHED		BIT(1)
#define INV_ICM45600_INT1_CONFIG2_ACTIVE_HIGH		BIT(0)
#define INV_ICM45600_INT1_CONFIG2_ACTIVE_LOW		0x00

#define INV_ICM45600_REG_FIFO_CONFIG0			0x001D
#define INV_ICM45600_FIFO_CONFIG0_MODE_MASK		GENMASK(7, 6)
#define INV_ICM45600_FIFO_CONFIG0_MODE_BYPASS			\
		FIELD_PREP(INV_ICM45600_FIFO_CONFIG0_MODE_MASK, 0)
#define INV_ICM45600_FIFO_CONFIG0_MODE_STREAM			\
		FIELD_PREP(INV_ICM45600_FIFO_CONFIG0_MODE_MASK, 1)
#define INV_ICM45600_FIFO_CONFIG0_MODE_STOP_ON_FULL		\
		FIELD_PREP(INV_ICM45600_FIFO_CONFIG0_MODE_MASK, 2)
#define INV_ICM45600_FIFO_CONFIG0_FIFO_DEPTH_MAX	0x1F

#define INV_ICM45600_REG_FIFO_CONFIG2			0x0020
#define INV_ICM45600_REG_FIFO_CONFIG2_FIFO_FLUSH	BIT(7)
#define INV_ICM45600_REG_FIFO_CONFIG2_WM_GT_TH		BIT(3)

#define INV_ICM45600_REG_FIFO_CONFIG3			0x0021
#define INV_ICM45600_FIFO_CONFIG3_ES1_EN		BIT(5)
#define INV_ICM45600_FIFO_CONFIG3_ES0_EN		BIT(4)
#define INV_ICM45600_FIFO_CONFIG3_HIRES_EN		BIT(3)
#define INV_ICM45600_FIFO_CONFIG3_GYRO_EN		BIT(2)
#define INV_ICM45600_FIFO_CONFIG3_ACCEL_EN		BIT(1)
#define INV_ICM45600_FIFO_CONFIG3_IF_EN			BIT(0)

#define INV_ICM45600_REG_FIFO_CONFIG4			0x0022
#define INV_ICM45600_FIFO_CONFIG4_COMP_EN		BIT(2)
#define INV_ICM45600_FIFO_CONFIG4_TMST_FSYNC_EN		BIT(1)
#define INV_ICM45600_FIFO_CONFIG4_ES0_9B		BIT(0)

/* all sensor data are 16 bits (2 registers wide) in big-endian */
#define INV_ICM45600_REG_TEMP_DATA			0x000C
#define INV_ICM45600_REG_ACCEL_DATA_X			0x0000
#define INV_ICM45600_REG_ACCEL_DATA_Y			0x0002
#define INV_ICM45600_REG_ACCEL_DATA_Z			0x0004
#define INV_ICM45600_REG_GYRO_DATA_X			0x0006
#define INV_ICM45600_REG_GYRO_DATA_Y			0x0008
#define INV_ICM45600_REG_GYRO_DATA_Z			0x000A
#define INV_ICM45600_DATA_INVALID			-32768

#define INV_ICM45600_REG_INT_STATUS			0x0019
#define INV_ICM45600_INT_STATUS_RESET_DONE		BIT(7)
#define INV_ICM45600_INT_STATUS_AUX1_AGC_RDY		BIT(6)
#define INV_ICM45600_INT_STATUS_AP_AGC_RDY		BIT(5)
#define INV_ICM45600_INT_STATUS_AP_FSYNC		BIT(4)
#define INV_ICM45600_INT_STATUS_AUX1_DRDY		BIT(3)
#define INV_ICM45600_INT_STATUS_DATA_RDY		BIT(2)
#define INV_ICM45600_INT_STATUS_FIFO_THS		BIT(1)
#define INV_ICM45600_INT_STATUS_FIFO_FULL		BIT(0)

/*
 * FIFO access registers
 * FIFO count is 16 bits (2 registers)
 * FIFO data is a continuous read register to read FIFO content
 */
#define INV_ICM45600_REG_FIFO_COUNT			0x0012
#define INV_ICM45600_REG_FIFO_DATA			0x0014

#define INV_ICM45600_REG_PWR_MGMT0			0x0010
#define INV_ICM45600_PWR_MGMT0_GYRO(_mode)		\
		FIELD_PREP(GENMASK(3, 2), (_mode))
#define INV_ICM45600_PWR_MGMT0_ACCEL(_mode)		\
		FIELD_PREP(GENMASK(1, 0), (_mode))

#define INV_ICM45600_REG_ACCEL_CONFIG0			0x001B
#define INV_ICM45600_ACCEL_CONFIG0_FS_MASK		GENMASK(6, 4)
#define INV_ICM45600_ACCEL_CONFIG0_FS(_fs)		\
		FIELD_PREP(INV_ICM45600_ACCEL_CONFIG0_FS_MASK, (_fs))
#define INV_ICM45600_ACCEL_CONFIG0_FS_16G		\
		INV_ICM45600_ACCEL_CONFIG0_FS(1)
#define INV_ICM45600_ACCEL_CONFIG0_ODR(_odr)		\
		FIELD_PREP(GENMASK(3, 0), (_odr))
#define INV_ICM45600_REG_GYRO_CONFIG0			0x001C
#define INV_ICM45600_GYRO_CONFIG0_FS_MASK		GENMASK(7, 4)
#define INV_ICM45600_GYRO_CONFIG0_FS(_fs)		\
		FIELD_PREP(INV_ICM45600_GYRO_CONFIG0_FS_MASK, (_fs))
#define INV_ICM45600_GYRO_CONFIG0_FS_2000DPS		\
		INV_ICM45600_GYRO_CONFIG0_FS(1)
#define INV_ICM45600_GYRO_CONFIG0_ODR(_odr)		\
		FIELD_PREP(GENMASK(3, 0), (_odr))

#define INV_ICM45600_REG_SMC_CONTROL_0			0xA258
#define INV_ICM45600_SMC_CONTROL_0_ACCEL_LP_CLK_SEL	BIT(4)
#define INV_ICM45600_SMC_CONTROL_0_TMST_EN		BIT(0)

/* FIFO watermark is 16 bits (2 registers wide) in little-endian */
#define INV_ICM45600_REG_FIFO_WATERMARK			0x001E
#define INV_ICM45600_FIFO_WATERMARK_VAL(_wm)		\
		cpu_to_le16(_wm)
/* FIFO is configured for 8kb */
#define INV_ICM45600_FIFO_SIZE_MAX			(8 * 1024)

#define INV_ICM45600_REG_INT1_CONFIG0			0x0016
#define INV_ICM45600_INT1_CONFIG0_RESET_DONE_EN		BIT(7)
#define INV_ICM45600_INT1_CONFIG0_AUX1_AGC_RDY_EN	BIT(6)
#define INV_ICM45600_INT1_CONFIG0_AP_AGC_RDY_EN		BIT(5)
#define INV_ICM45600_INT1_CONFIG0_AP_FSYNC_EN		BIT(4)
#define INV_ICM45600_INT1_CONFIG0_AUX1_DRDY_EN		BIT(3)
#define INV_ICM45600_INT1_CONFIG0_DRDY_EN		BIT(2)
#define INV_ICM45600_INT1_CONFIG0_FIFO_THS_EN		BIT(1)
#define INV_ICM45600_INT1_CONFIG0_FIFO_FULL_EN		BIT(0)

#define INV_ICM45600_REG_WHOAMI				0x0072
#define INV_ICM45600_WHOAMI_ICM45605			0xE5
#define INV_ICM45600_WHOAMI_ICM45686			0xE9
#define INV_ICM45600_WHOAMI_ICM45688P			0xE7
#define INV_ICM45600_WHOAMI_ICM45608			0x81
#define INV_ICM45600_WHOAMI_ICM45634			0x82
#define INV_ICM45600_WHOAMI_ICM45689			0x83
#define INV_ICM45600_WHOAMI_ICM45606			0x84
#define INV_ICM45600_WHOAMI_ICM45687			0x85

/* Gyro USER offset */
#define INV_ICM45600_IPREG_SYS1_REG_42			0xA42A
#define INV_ICM45600_IPREG_SYS1_REG_56			0xA438
#define INV_ICM45600_IPREG_SYS1_REG_70			0xA446
/* Gyro Averaging filter */
#define INV_ICM45600_IPREG_SYS1_REG_170			0xA4AA
#define INV_ICM45600_IPREG_SYS1_REG_170_MASK		GENMASK(4, 1)
#define INV_ICM45600_GYRO_LP_AVG_SEL_8X			\
	FIELD_PREP_CONST(INV_ICM45600_IPREG_SYS1_REG_170_MASK, 5)
#define INV_ICM45600_GYRO_LP_AVG_SEL_2X			\
	FIELD_PREP_CONST(INV_ICM45600_IPREG_SYS1_REG_170_MASK, 1)
/* Accel USER offset */
#define INV_ICM45600_IPREG_SYS2_REG_24			0xA518
#define INV_ICM45600_IPREG_SYS2_REG_32			0xA520
#define INV_ICM45600_IPREG_SYS2_REG_40			0xA528
/* Accel averaging filter */
#define INV_ICM45600_IPREG_SYS2_REG_129			0xA581
#define INV_ICM45600_ACCEL_LP_AVG_SEL_4X		0x0002

/* Sleep times required by the driver */
#define INV_ICM45600_POWER_UP_TIME_MS		100
#define INV_ICM45600_RESET_TIME_MS		1
#define INV_ICM45600_ACCEL_STARTUP_TIME_MS	60
#define INV_ICM45600_GYRO_STARTUP_TIME_MS	60
#define INV_ICM45600_GYRO_STOP_TIME_MS		150
#define INV_ICM45600_SUSPEND_DELAY_MS		2000
#define INV_ICM45600_IREG_DELAY_US		4

typedef int (*inv_icm45600_bus_setup)(struct inv_icm45600_state *);

extern const struct dev_pm_ops inv_icm45600_pm_ops;

const struct iio_mount_matrix *
inv_icm45600_get_mount_matrix(const struct iio_dev *indio_dev,
			      const struct iio_chan_spec *chan);

uint32_t inv_icm45600_odr_to_period(enum inv_icm45600_odr odr);

int inv_icm45600_set_accel_conf(struct inv_icm45600_state *st,
				struct inv_icm45600_sensor_conf *conf,
				unsigned int *sleep_ms);

int inv_icm45600_set_gyro_conf(struct inv_icm45600_state *st,
			       struct inv_icm45600_sensor_conf *conf,
			       unsigned int *sleep_ms);

int inv_icm45600_debugfs_reg(struct iio_dev *indio_dev, unsigned int reg,
			     unsigned int writeval, unsigned int *readval);

int inv_icm45600_core_probe(struct regmap *regmap, int chip,
			    bool reset, inv_icm45600_bus_setup bus_setup);

struct iio_dev *inv_icm45600_gyro_init(struct inv_icm45600_state *st);

int inv_icm45600_gyro_parse_fifo(struct iio_dev *indio_dev);

struct iio_dev *inv_icm45600_accel_init(struct inv_icm45600_state *st);

int inv_icm45600_accel_parse_fifo(struct iio_dev *indio_dev);

#endif
