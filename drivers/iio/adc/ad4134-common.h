/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Analog Devices AD4134 and similar ADCs common definitions and properties
 * Copyright (C) 2025 Analog Devices, Inc.
 * Author: Marcelo Schmitt <marcelo.schmitt@analog.com>
 */

#ifndef __DRIVERS_IIO_ADC_AD4134_COMMON_H__
#define __DRIVERS_IIO_ADC_AD4134_COMMON_H__

#include <linux/array_size.h>
#include <linux/bits.h>
#include <linux/compiler_attributes.h>
#include <linux/crc8.h>
#include <linux/iio/iio.h>
#include <linux/units.h>
#include <linux/types.h>

#define AD4134_RESET_TIME_US			(10 * MICRO)

#define AD4134_REG_READ_MASK			BIT(7)
#define AD4134_SPI_MAX_XFER_LEN			3

#define AD4134_EXT_CLOCK_MHZ			(48 * MEGA)

#define AD4134_NUM_CHANNELS			4

#define AD4134_IFACE_CONFIG_A_REG		0x00
#define AD4134_IFACE_CONFIG_B_REG		0x01
#define AD4134_IFACE_CONFIG_B_SINGLE_INSTR	BIT(7)

#define AD4134_DEVICE_CONFIG_REG		0x02
#define AD4134_DEVICE_CONFIG_POWER_MODE_MASK	BIT(0)
#define AD4134_POWER_MODE_HIGH_PERF		0x1

#define AD4134_SILICON_REV_REG			0x07
#define AD4134_SCRATCH_PAD_REG			0x0A
#define AD4134_STREAM_MODE_REG			0x0E
#define AD4134_SDO_PIN_SRC_SEL_REG		0x10
#define AD4134_SDO_PIN_SRC_SEL_SDO_SEL_MASK	BIT(2)

#define AD4134_DATA_PACKET_CONFIG_REG		0x11
#define AD4134_DATA_PACKET_CONFIG_FRAME_MASK	GENMASK(5, 4)
#define AD4134_DATA_PACKET_16BIT_FRAME		0x0
#define AD4134_DATA_PACKET_16BIT_CRC6_FRAME	0x1
#define AD4134_DATA_PACKET_24BIT_FRAME		0x2
#define AD4134_DATA_PACKET_24BIT_CRC6_FRAME	0x3

#define AD4134_DIG_IF_CFG_REG			0x12
#define AD4134_DIF_IF_CFG_FORMAT_MASK		GENMASK(1, 0)
#define AD4134_DATA_FORMAT_SINGLE_CH_MODE	0x0

#define AD4134_PW_DOWN_CTRL_REG			0x13
#define AD4134_DEVICE_STATUS_REG		0x15
#define AD4134_ODR_VAL_INT_LSB_REG		0x16
#define AD4134_CH3_OFFSET_MSB_REG		0x3E
#define AD4134_AIN_OR_ERROR_REG			0x48

#define AD4134_CH_VREG(x)			((x) + 0x50) /* chanX virtual register */
#define AD4134_VREG_CH(x)			((x) - 0x50) /* chan of virtual reg X */

#define AD4134_SPI_CRC_POLYNOM			0x07
#define AD4134_SPI_CRC_INIT_VALUE		0xA5
extern unsigned char ad4134_spi_crc_table[CRC8_TABLE_SIZE];

extern const struct regmap_access_table ad4134_regmap_rd_table;
extern const struct regmap_access_table ad4134_regmap_wr_table;

#define AD4134_SCAN_TYPE(_realbits, _storebits) {				\
	.sign = 's',								\
	.realbits = (_realbits),						\
	.storagebits = (_storebits),						\
	.shift = ((_storebits) - (_realbits)),					\
	.endianness = IIO_BE							\
}

struct iio_scan_type ad4134_scan_types[] = {
	AD4134_SCAN_TYPE(16, 16),
	AD4134_SCAN_TYPE(16, 24),
	AD4134_SCAN_TYPE(24, 24),
	AD4134_SCAN_TYPE(24, 32),
};

#define AD4134_CHANNEL(_index) {						\
	.type = IIO_VOLTAGE,							\
	.indexed = 1,								\
	.channel = (_index),							\
	.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),				\
	.info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SCALE),			\
	.scan_index = (_index),							\
	.has_ext_scan_type = 1,							\
	.ext_scan_type = ad4134_scan_types,					\
	.num_ext_scan_type = ARRAY_SIZE(ad4134_scan_types)			\
}

struct device;

struct ad4134_state {
	struct device *dev;
	struct regmap *regmap;
	unsigned long sys_clk_rate;
	int refin_mv;
	struct gpio_desc *odr_gpio;
	unsigned int current_scan_type;
	/*
	 * DMA (thus cache coherency maintenance) requires the transfer buffers
	 * to live in their own cache lines.
	 */
	u8 rx_buf[AD4134_SPI_MAX_XFER_LEN] __aligned(IIO_DMA_MINALIGN);
	u8 tx_buf[AD4134_SPI_MAX_XFER_LEN];
};

struct ad4134_chip_info {
	const char *name;
};

extern const struct ad4134_chip_info ad4134_chip_info;

struct ad4134_bus_ops {
	int (*config_iio_dev)(struct iio_dev *indio_dev);
	struct regmap *(*init_regmap)(struct ad4134_state *st);
	int (*setup)(struct ad4134_state *st);
};

struct ad4134_bus_info {
	const struct ad4134_chip_info *chip_info;
	const struct ad4134_bus_ops *bops;
};

int ad4134_probe(struct device *dev, const struct ad4134_bus_info *bus_info);

#endif /* __DRIVERS_IIO_ADC_AD4134_COMMON_H__ */
