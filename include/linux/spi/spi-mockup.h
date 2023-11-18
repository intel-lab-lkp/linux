/* SPDX-License-Identifier: GPL-2.0 */

#ifndef __LINUX_SPI_MOCKUP_H
#define __LINUX_SPI_MOCKUP_H

#define SPI_BUFSIZ_MAX		0x1000

struct spi_msg_ctx {
	int ret;
	unsigned cs_off:1;
	unsigned cs_change:1;
	unsigned tx_nbits:3;
	unsigned rx_nbits:3;
	__u8 data[SPI_BUFSIZ_MAX];
};

#endif
