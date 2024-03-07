/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __LINUX_SPI_XILINX_SPI_H
#define __LINUX_SPI_XILINX_SPI_H

#include <linux/types.h>

struct spi_board_info;

/**
 * struct xspi_platform_data - Platform data of the Xilinx SPI driver
 * @force_irq:		If set, forces QSPI transaction requirements.
 * @num_chipselect:	Number of chip select by the IP.
 * @bits_per_word:	Number of bits per word.
 * @num_devices:	Number of devices in the devices array.
 * @devices:		Devices to add when the driver is probed.
 */
struct xspi_platform_data {
	bool force_irq;
	u8 num_chipselect;
	u8 bits_per_word;
	u8 num_devices;
	struct spi_board_info *devices;
};

#endif /* __LINUX_SPI_XILINX_SPI_H */
